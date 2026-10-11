#define _POSIX_C_SOURCE 200809L
#include <wayland-client.h>
#include "virtual-pointer-client-protocol.h"
#include "virtual-keyboard-client-protocol.h"
#include <xkbcommon/xkbcommon.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <ctype.h>
#include <linux/input-event-codes.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <poll.h>
#include <unistd.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <signal.h>
#include <sys/wait.h>
#include <json-c/json.h>
#include <limits.h>

static struct wl_display *display;
static struct wl_seat *agent;
static struct zwlr_virtual_pointer_manager_v1 *manager;
static struct zwlr_virtual_pointer_v1 *pointer;
static struct zwp_virtual_keyboard_manager_v1 *keyboard_manager;
static struct zwp_virtual_keyboard_v1 *keyboard;
static struct xkb_context *xkb_ctx;
static struct xkb_keymap *keymap;
static int seat_found;
static struct wl_keyboard *seat0_keyboard;
static struct xkb_keymap *seat0_keymap;
static int seat0_keymap_error;
static int no_create_seat;
static uint32_t held_buttons;
static uint32_t agent_global_name;
struct seat_candidate { struct wl_seat *seat; uint32_t global_name; uint32_t caps; int is_seat0; };
static volatile sig_atomic_t running = 1;
static char socket_path[sizeof(((struct sockaddr_un *)0)->sun_path)];

static void stop_handler(int sig) { (void)sig; running = 0; }
static uint32_t milliseconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}
/* A wl_keyboard keymap is the *effective* compositor keymap.  Wayland does
 * not expose the original xkb_rule_names (rules/model/layout/variant/options).
 * Reusing this keymap is more accurate than attempting to guess those names. */
static void source_keymap(void *data, struct wl_keyboard *kb,
                          uint32_t format, int fd, uint32_t size)
{
    (void)data;
    (void)kb;

    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 ||
        !size || size > 16u * 1024u * 1024u) {
        fprintf(stderr, "[keymap] invalid format or size\n");
        close(fd);
        seat0_keymap_error = 1;
        return;
    }

    void *mapped = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    int saved_errno = mapped == MAP_FAILED ? errno : 0;
    close(fd);

    if (mapped == MAP_FAILED) {
        errno = saved_errno;
        perror("[keymap] mmap");
        seat0_keymap_error = 1;
        return;
    }

    /* xkbcommon expects a NUL-terminated string. Copy the mapped data
     * so parsing cannot read beyond the supplied Wayland keymap size. */
    char *buf = malloc((size_t)size + 1);

    if (!buf) {
        perror("[keymap] malloc");
        munmap(mapped, size);
        seat0_keymap_error = 1;
        return;
    }

    memcpy(buf, mapped, size);
    buf[size] = '\0';

    munmap(mapped, size);

    if (!xkb_ctx)
        xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);

    if (!xkb_ctx) {
        fprintf(stderr, "[keymap] xkb_context_new failed\n");
        free(buf);
        seat0_keymap_error = 1;
        return;
    }

    struct xkb_keymap *new_map = xkb_keymap_new_from_string(
        xkb_ctx,
        buf,
        XKB_KEYMAP_FORMAT_TEXT_V1,
        XKB_KEYMAP_COMPILE_NO_FLAGS
    );

    free(buf);

    if (!new_map) {
        fprintf(stderr, "[keymap] XKB compilation failed\n");
        seat0_keymap_error = 1;
        return;
    }

    if (seat0_keymap)
        xkb_keymap_unref(seat0_keymap);

    seat0_keymap = new_map;
    seat0_keymap_error = 0;
}
static void source_enter(void *d, struct wl_keyboard *k, uint32_t serial, struct wl_surface *s, struct wl_array *keys) {
    (void)d; (void)k; (void)serial; (void)s; (void)keys;
}
static void source_leave(void *d, struct wl_keyboard *k, uint32_t serial, struct wl_surface *s) {
    (void)d; (void)k; (void)serial; (void)s;
}
static void source_key(void *d, struct wl_keyboard *k, uint32_t serial, uint32_t time, uint32_t key, uint32_t state) {
    (void)d; (void)k; (void)serial; (void)time; (void)key; (void)state;
}
static void source_modifiers(void *d, struct wl_keyboard *k, uint32_t serial, uint32_t dep, uint32_t lat, uint32_t lock, uint32_t group) {
    (void)d; (void)k; (void)serial; (void)dep; (void)lat; (void)lock; (void)group;
}
static void source_repeat(void *d, struct wl_keyboard *k, int32_t rate, int32_t delay) {
    (void)d; (void)k; (void)rate; (void)delay;
}
static const struct wl_keyboard_listener source_keyboard_listener = {
    source_keymap, source_enter, source_leave, source_key, source_modifiers, source_repeat
};
static void maybe_bind_seat0_keyboard(struct seat_candidate *candidate) {
    if (!candidate->is_seat0 || seat0_keyboard || !(candidate->caps & WL_SEAT_CAPABILITY_KEYBOARD)) return;
    seat0_keyboard = wl_seat_get_keyboard(candidate->seat);
    if (seat0_keyboard) wl_keyboard_add_listener(seat0_keyboard, &source_keyboard_listener, NULL);
}
static void seat_caps(void *data, struct wl_seat *s, uint32_t caps) {
    (void)s;
    struct seat_candidate *candidate = data;
    candidate->caps = caps;
    maybe_bind_seat0_keyboard(candidate);
}
static void seat_name(void *data, struct wl_seat *s, const char *name) {
    struct seat_candidate *candidate = data;
    if (!strcmp(name, "agent")) {
        agent = s;
        agent_global_name = candidate->global_name;
        seat_found = 1;
    } else if (!strcmp(name, "seat0")) {
        candidate->is_seat0 = 1;
        maybe_bind_seat0_keyboard(candidate);
    }
}
static const struct wl_seat_listener seat_listener = {seat_caps, seat_name};
static void registry_global(void *data, struct wl_registry *registry, uint32_t name,
                            const char *interface, uint32_t version) {
    (void)data;
    if (!strcmp(interface, wl_seat_interface.name)) {
        struct wl_seat *s = wl_registry_bind(registry, name, &wl_seat_interface,
                                             version < 2 ? version : 2);
        struct seat_candidate *candidate = calloc(1, sizeof(*candidate));
        if (!candidate) { fprintf(stderr, "Out of memory\n"); exit(1); }
        candidate->seat = s;
        candidate->global_name = name;
        wl_seat_add_listener(s, &seat_listener, candidate);
    } else if (!strcmp(interface, zwlr_virtual_pointer_manager_v1_interface.name) && version >= 2) {
        manager = wl_registry_bind(registry, name,
                                   &zwlr_virtual_pointer_manager_v1_interface, 2);
    } else if (!strcmp(interface, zwp_virtual_keyboard_manager_v1_interface.name)) {
        keyboard_manager = wl_registry_bind(registry, name,
                                &zwp_virtual_keyboard_manager_v1_interface, 1);
    }
}
static void registry_remove(void *data, struct wl_registry *r, uint32_t name) {
    (void)data; (void)r;
    if (seat_found && name == agent_global_name) {
        fprintf(stderr, "Agent seat disappeared; stopping daemon\n");
        running = 0;
    }
}
static const struct wl_registry_listener registry_listener = {registry_global, registry_remove};

static int setup_keyboard(void) {
    if (!seat0_keymap || !xkb_ctx) return -1;
    /* Own the snapshot uploaded below, independently of later seat0 updates. */
    keymap = xkb_keymap_ref(seat0_keymap);
    char *data = xkb_keymap_get_as_string(keymap, XKB_KEYMAP_FORMAT_TEXT_V1);
    if (!data) return -1;
    size_t size = strlen(data) + 1;
    char path[] = "/tmp/agent-keymap-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) { free(data); return -1; }
    unlink(path);
    if (ftruncate(fd, (off_t)size) < 0) { close(fd); free(data); return -1; }
    size_t done = 0;
    while (done < size) {
        ssize_t n = write(fd, data + done, size - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { close(fd); free(data); return -1; }
        done += (size_t)n;
    }
    free(data);
    keyboard = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(keyboard_manager, agent);
    if (!keyboard) { close(fd); return -1; }
    zwp_virtual_keyboard_v1_keymap(keyboard, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, (uint32_t)size);
    close(fd);
    return wl_display_roundtrip(display) < 0 ? -1 : 0;
}

static void emit_key(uint32_t evdev, int down) {
    zwp_virtual_keyboard_v1_key(keyboard, milliseconds(), evdev,
             down ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED);
}
static void emit_mods(xkb_mod_mask_t mods, xkb_layout_index_t layout) {
    zwp_virtual_keyboard_v1_modifiers(keyboard, mods, 0, 0, layout);
}
static int send_keysym(xkb_keysym_t sym) {
    struct xkb_state *state = xkb_state_new(keymap);
    if (!state) return -1;
    xkb_keycode_t min = xkb_keymap_min_keycode(keymap);
    xkb_keycode_t max = xkb_keymap_max_keycode(keymap);
    for (xkb_keycode_t kc = min; kc <= max; kc++) {
        if (kc < 8) continue;
        for (xkb_layout_index_t layout = 0; layout < xkb_keymap_num_layouts_for_key(keymap,kc); layout++) {
            for (xkb_level_index_t level = 0; level < xkb_keymap_num_levels_for_key(keymap,kc,layout); level++) {
                const xkb_keysym_t *syms = NULL;
                int count = xkb_keymap_key_get_syms_by_level(keymap,kc,layout,level,&syms);
                if (count != 1 || syms[0] != sym) continue;
                xkb_mod_mask_t masks[64];
                size_t num_masks = xkb_keymap_key_get_mods_for_level(
                    keymap, kc, layout, level, masks, sizeof(masks) / sizeof(masks[0]));
                for (size_t i = 0; i < num_masks; i++) {
                    xkb_state_update_mask(state, masks[i], 0, 0, 0, 0, layout);
                    if (xkb_state_key_get_one_sym(state, kc) != sym) continue;
                    emit_mods(masks[i], layout);
                    emit_key(kc-8, 1);
                    emit_key(kc-8, 0);
                    emit_mods(0, 0);
                    xkb_state_unref(state);
                    return 0;
                }
            }
        }
    }
    xkb_state_unref(state);
    return -1;
}
static int send_text(const char *s) {
    while (*s) {
        uint32_t cp;
        unsigned char a = (unsigned char)*s++;
        if (a < 0x80) cp=a;
        else {
            int n = a >= 0xf0 && a <= 0xf4 ? 3 : a >= 0xe0 && a <= 0xef ? 2 : a >= 0xc2 && a <= 0xdf ? 1 : -1;
            if (n < 0) return -1;
            cp = a & (n==1 ? 0x1f : n==2 ? 0x0f : 0x07);
            for (int i=0;i<n;i++) {
                if (!*s) return -1; /* truncated UTF-8 sequence */
                unsigned char b=(unsigned char)*s++;
                if ((b & 0xc0) != 0x80) return -1;
                cp=(cp<<6)|(b&0x3f);
            }
            if (cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff) ||
                (n == 1 && cp < 0x80) || (n == 2 && cp < 0x800) ||
                (n == 3 && cp < 0x10000)) return -1;
        }
        xkb_keysym_t sym = xkb_utf32_to_keysym(cp);
        if (sym == XKB_KEY_NoSymbol || send_keysym(sym) < 0) return -1;
    }
    return 0;
}
static int send_named_key(const char *name) {
    xkb_keysym_t sym=xkb_keysym_from_name(name, XKB_KEYSYM_CASE_INSENSITIVE);
    return sym == XKB_KEY_NoSymbol ? -1 : send_keysym(sym);
}

static int button_code(const char *side, uint32_t *code, uint32_t *bit) {
    if (!strcmp(side, "left")) { *code = BTN_LEFT; *bit = 1u; }
    else if (!strcmp(side, "right")) { *code = BTN_RIGHT; *bit = 2u; }
    else if (!strcmp(side, "middle")) { *code = BTN_MIDDLE; *bit = 4u; }
    else return -1;
    return 0;
}
static void pointer_button(uint32_t code, int down) {
    zwlr_virtual_pointer_v1_button(pointer, milliseconds(), code,
        down ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED);
    zwlr_virtual_pointer_v1_frame(pointer);
}
static void release_held_buttons(void) {
    if (!pointer) return;
    if (held_buttons & 1u) pointer_button(BTN_LEFT, 0);
    if (held_buttons & 2u) pointer_button(BTN_RIGHT, 0);
    if (held_buttons & 4u) pointer_button(BTN_MIDDLE, 0);
    held_buttons = 0;
    wl_display_flush(display);
}
static int modifier_mask(const char *name, xkb_mod_mask_t *mask) {
    const char *xkb_name = NULL;
    if (!strcasecmp(name, "ctrl") || !strcasecmp(name, "control")) xkb_name = XKB_MOD_NAME_CTRL;
    else if (!strcasecmp(name, "alt")) xkb_name = XKB_MOD_NAME_ALT;
    else if (!strcasecmp(name, "shift")) xkb_name = XKB_MOD_NAME_SHIFT;
    else if (!strcasecmp(name, "super") || !strcasecmp(name, "meta") || !strcasecmp(name, "win")) xkb_name = XKB_MOD_NAME_LOGO;
    else return -1;
    xkb_mod_index_t idx = xkb_keymap_mod_get_index(keymap, xkb_name);
    if (idx == XKB_MOD_INVALID || idx >= sizeof(xkb_mod_mask_t) * 8) return -1;
    *mask |= (xkb_mod_mask_t)1 << idx;
    return 0;
}
/* Find a physical key whose level-zero symbol matches the requested keysym.
 * The shortcut modifiers are sent via the virtual-keyboard modifiers event. */
static int send_chord(const char *spec) {
    char buf[128];
    if (strlen(spec) >= sizeof(buf)) return -1;
    strcpy(buf, spec);
    xkb_mod_mask_t mods = 0;
    char *save = NULL;
    char *part = strtok_r(buf, "+", &save);
    if (!part) return -1;
    char *last = part;
    while ((part = strtok_r(NULL, "+", &save)) != NULL) {
        if (modifier_mask(last, &mods) < 0) return -1;
        last = part;
    }
    if (!mods || !*last) return -1;
    xkb_keysym_t sym = xkb_keysym_from_name(last, XKB_KEYSYM_CASE_INSENSITIVE);
    if (sym == XKB_KEY_NoSymbol) return -1;
    xkb_keycode_t min = xkb_keymap_min_keycode(keymap);
    xkb_keycode_t max = xkb_keymap_max_keycode(keymap);
    for (xkb_keycode_t kc = min; kc <= max; ++kc) {
        const xkb_keysym_t *syms = NULL;
        int count = xkb_keymap_key_get_syms_by_level(keymap, kc, 0, 0, &syms);
        if (count == 1 && syms[0] == sym && kc >= 8) {
            emit_mods(mods, 0);
            emit_key(kc - 8, 1);
            emit_key(kc - 8, 0);
            emit_mods(0, 0);
            return 0;
        }
    }
    return -1;
}
static int scroll_pointer(const char *axis, int steps) {
    if (steps < -100 || steps > 100) return -1;
    uint32_t direction;
    if (!strcmp(axis, "vertical")) direction = WL_POINTER_AXIS_VERTICAL_SCROLL;
    else if (!strcmp(axis, "horizontal")) direction = WL_POINTER_AXIS_HORIZONTAL_SCROLL;
    else return -1;
    if (steps) {
        /* 15 units per wheel detent is the conventional wl_pointer value. */
        zwlr_virtual_pointer_v1_axis(pointer, milliseconds(), direction, wl_fixed_from_int(steps * 15));
        zwlr_virtual_pointer_v1_frame(pointer);
    }
    return 0;
}

/* Query Sway's current logical output geometry for every command. This also
 * handles hotplug, layout rearrangement and fractional output scaling. */
struct output_info { char name[128]; int x, y, width, height; };
static int get_outputs(struct output_info *out, size_t cap, size_t *count) {
    *count = 0;
    int fds[2];
    if (pipe(fds) < 0) return -1;
    pid_t pid = fork();
    if (pid < 0) { close(fds[0]); close(fds[1]); return -1; }
    if (!pid) {
        close(fds[0]);
        if (dup2(fds[1], STDOUT_FILENO) < 0) _exit(127);
        close(fds[1]);
        execlp("swaymsg", "swaymsg", "-t", "get_outputs", "-r", (char *)NULL);
        _exit(127);
    }
    close(fds[1]);
    size_t used = 0;
    const size_t limit = 1024 * 1024;
    char *buf = malloc(limit + 1);
    if (!buf) { close(fds[0]); waitpid(pid, NULL, 0); return -1; }
    int err = 0;
    while (used < limit) {
        ssize_t r = read(fds[0], buf + used, limit - used);
        if (r < 0 && errno == EINTR) continue;
        if (r < 0) { err = 1; break; }
        if (!r) break;
        used += (size_t)r;
    }
    if (used == limit) err = 1;
    close(fds[0]);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) { if (errno != EINTR) { err = 1; break; } }
    if (err || !WIFEXITED(status) || WEXITSTATUS(status)) { free(buf); return -1; }
    buf[used] = 0;
    struct json_object *root = json_tokener_parse(buf);
    free(buf);
    if (!root || !json_object_is_type(root, json_type_array)) {
        if (root) json_object_put(root);
        return -1;
    }
    int len = json_object_array_length(root);
    for (int i = 0; i < len; i++) {
        struct json_object *item = json_object_array_get_idx(root, i);
        struct json_object *active, *name, *rect, *jx, *jy, *jw, *jh;
        if (!json_object_object_get_ex(item, "active", &active) || !json_object_get_boolean(active)) continue;
        if (!json_object_object_get_ex(item, "name", &name) || !json_object_is_type(name, json_type_string)) continue;
        if (!json_object_object_get_ex(item, "rect", &rect) || !json_object_is_type(rect, json_type_object)) continue;
        if (!json_object_object_get_ex(rect, "x", &jx) || !json_object_object_get_ex(rect, "y", &jy) ||
            !json_object_object_get_ex(rect, "width", &jw) || !json_object_object_get_ex(rect, "height", &jh)) continue;
        if (*count >= cap) { json_object_put(root); return -1; }
        struct output_info *o = &out[(*count)++];
        const char *output_name = json_object_get_string(name);
        if (strlen(output_name) >= sizeof(o->name)) { json_object_put(root); return -1; }
        snprintf(o->name, sizeof(o->name), "%s", output_name);
        o->x = json_object_get_int(jx); o->y = json_object_get_int(jy);
        o->width = json_object_get_int(jw); o->height = json_object_get_int(jh);
        if (o->width <= 0 || o->height <= 0) { --*count; }
    }
    json_object_put(root);
    return 0;
}
static int move_to_output(const char *name, int x, int y, char *reply, size_t n) {
    struct output_info outputs[32]; size_t count;
    if (get_outputs(outputs, 32, &count) < 0) { snprintf(reply, n, "ERR cannot query Sway outputs\n"); return -1; }
    const struct output_info *target = NULL;
    int64_t minx = INT_MAX, miny = INT_MAX, maxx = INT_MIN, maxy = INT_MIN;
    for (size_t i = 0; i < count; i++) {
        const struct output_info *o = &outputs[i];
        if (!strcmp(name, o->name)) target = o;
        if (o->x < minx) minx = o->x;
        if (o->y < miny) miny = o->y;
        if ((int64_t)o->x + o->width > maxx) maxx = (int64_t)o->x + o->width;
        if ((int64_t)o->y + o->height > maxy) maxy = (int64_t)o->y + o->height;
    }
    if (!target) { snprintf(reply, n, "ERR unknown or inactive output\n"); return -1; }
    if (x < 0 || y < 0 || x >= target->width || y >= target->height) {
        snprintf(reply, n, "ERR coordinates outside output (%dx%d)\n", target->width, target->height);
        return -1;
    }
    int64_t width = maxx - minx, height = maxy - miny;
    int64_t px = (int64_t)target->x + x - minx;
    int64_t py = (int64_t)target->y + y - miny;
    if (width <= 0 || height <= 0 || width > UINT32_MAX || height > UINT32_MAX ||
        px < 0 || py < 0 || px >= width || py >= height) {
        snprintf(reply, n, "ERR invalid global output geometry\n"); return -1;
    }
    zwlr_virtual_pointer_v1_motion_absolute(pointer, milliseconds(), (uint32_t)px,
                                              (uint32_t)py, (uint32_t)width, (uint32_t)height);
    zwlr_virtual_pointer_v1_frame(pointer);
    if (wl_display_flush(display) < 0) { snprintf(reply, n, "ERR wayland flush\n"); return -1; }
    snprintf(reply, n, "OK output=%s local=%d,%d global=%lld,%lld\n", name, x, y,
             (long long)((int64_t)target->x + x), (long long)((int64_t)target->y + y));
    return 0;
}
static void list_outputs(char *reply, size_t n) {
    struct output_info outputs[32]; size_t count;
    if (get_outputs(outputs, 32, &count) < 0) { snprintf(reply, n, "ERR cannot query Sway outputs\n"); return; }
    size_t used = (size_t)snprintf(reply, n, "OK outputs=%zu", count);
    for (size_t i = 0; i < count && used < n; i++) {
        int w = snprintf(reply + used, n - used, " %s:%dx%d@%d,%d", outputs[i].name,
                         outputs[i].width, outputs[i].height, outputs[i].x, outputs[i].y);
        if (w < 0) break;
        used += (size_t)w;
    }
    if (used >= n - 1) { snprintf(reply, n, "ERR too many outputs for reply buffer\n"); return; }
    snprintf(reply + used, n - used, "\n");
}

static int handle_command(const char *cmd, char *reply, size_t n) {
    unsigned x, y, w, h;
    int steps, local_x, local_y;
    char output_name[128];
    char side[16], extra;
    if (!strcmp(cmd, "outputs")) {
        list_outputs(reply, n);
    } else if (sscanf(cmd, "move-to %127s %d %d %c", output_name, &local_x, &local_y, &extra) == 3) {
        move_to_output(output_name, local_x, local_y, reply, n);
    } else if (sscanf(cmd, "move %u %u %u %u %c", &x, &y, &w, &h, &extra) == 4) {
        if (!w || !h || x >= w || y >= h) { snprintf(reply,n,"ERR coordinates outside dimensions\n"); return 0; }
        zwlr_virtual_pointer_v1_motion_absolute(pointer, milliseconds(), x, y, w, h);
        zwlr_virtual_pointer_v1_frame(pointer);
        if (wl_display_flush(display) < 0) { snprintf(reply,n,"ERR wayland flush\n"); return 0; }
        snprintf(reply,n,"OK\n");
    } else if (sscanf(cmd, "click %15s %c", side, &extra) == 1) {
        uint32_t code, bit;
        if (button_code(side, &code, &bit) < 0) snprintf(reply,n,"ERR unknown button\n");
        else if (held_buttons & bit) snprintf(reply,n,"ERR button already held\n");
        else { pointer_button(code, 1); pointer_button(code, 0); snprintf(reply,n,"OK\n"); }
    } else if (sscanf(cmd, "down %15s %c", side, &extra) == 1) {
        uint32_t code, bit;
        if (button_code(side, &code, &bit) < 0) snprintf(reply,n,"ERR unknown button\n");
        else if (held_buttons & bit) snprintf(reply,n,"ERR button already held\n");
        else { pointer_button(code, 1); held_buttons |= bit; snprintf(reply,n,"OK\n"); }
    } else if (sscanf(cmd, "up %15s %c", side, &extra) == 1) {
        uint32_t code, bit;
        if (button_code(side, &code, &bit) < 0) snprintf(reply,n,"ERR unknown button\n");
        else if (!(held_buttons & bit)) snprintf(reply,n,"ERR button not held\n");
        else { pointer_button(code, 0); held_buttons &= ~bit; snprintf(reply,n,"OK\n"); }
    } else if (!strcmp(cmd, "release")) {
        release_held_buttons(); snprintf(reply,n,"OK\n");
    } else if (sscanf(cmd, "scroll %15s %d %c", side, &steps, &extra) == 2) {
        if (scroll_pointer(side, steps) < 0) snprintf(reply,n,"ERR scroll axis or steps (-100..100)\n");
        else snprintf(reply,n,"OK\n");
    } else if (!strncmp(cmd, "chord ", 6)) {
        if (send_chord(cmd + 6) < 0) snprintf(reply,n,"ERR chord: use Ctrl+A, Ctrl+Shift+Tab, Alt+Tab, etc. (base-level key)\n");
        else snprintf(reply,n,"OK\n");
    } else if (!strncmp(cmd,"type ",5)) {
        if (send_text(cmd+5) < 0) snprintf(reply,n,"ERR character not directly available in seat0 keymap\n");
        else { wl_display_flush(display); snprintf(reply,n,"OK\n"); }
    } else if (!strncmp(cmd,"key ",4)) {
        if (send_named_key(cmd+4) < 0) snprintf(reply,n,"ERR unknown or unavailable key\n");
        else { wl_display_flush(display); snprintf(reply,n,"OK\n"); }
    } else if (!strcmp(cmd,"status")) snprintf(reply,n,"OK seat=agent pointer=connected keyboard=connected keymap=seat0-effective\n");
    else if (!strcmp(cmd,"quit")) { release_held_buttons(); snprintf(reply,n,"OK quitting\n"); return 1; }
    else snprintf(reply,n,"ERR use: outputs | move-to OUTPUT X Y | move X Y WIDTH HEIGHT | click BUTTON | down BUTTON | up BUTTON | release | scroll vertical|horizontal STEPS | chord Ctrl+A | key NAME | type TEXT | status | quit\n");
    return 0;
}
/* Never execute a stream prefix: require a newline within the size limit
 * and a total 2s deadline, regardless of fragmentation. */
static int read_command(int client, char *cmd, size_t size, char *reply, size_t n) {
    size_t used = 0;
    uint32_t start = milliseconds();
    while (running && used < size - 1) {
        uint32_t elapsed = milliseconds() - start;
        if (elapsed >= 2000) break;
        struct pollfd fd = {client, POLLIN, 0};
        int ret = poll(&fd, 1, (int)(2000 - elapsed));
        if (ret < 0 && errno == EINTR) continue;
        if (ret == 0) break;
        if (ret < 0) { snprintf(reply, n, "ERR socket read failed\n"); return -1; }
        ssize_t len = recv(client, cmd + used, size - 1 - used, MSG_DONTWAIT);
        if (len < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (len <= 0) {
            snprintf(reply, n, len == 0 ? "ERR incomplete command (newline required)\n" : "ERR socket read failed\n");
            return -1;
        }
        used += (size_t)len;
        char *newline = memchr(cmd, '\n', used);
        size_t command_len = newline ? (size_t)(newline - cmd) : used;
        if (memchr(cmd, '\0', command_len)) {
            snprintf(reply, n, "ERR NUL in command\n"); return -1;
        }
        if (newline) {
            if (command_len && cmd[command_len - 1] == '\r') --command_len;
            if (memchr(cmd, '\r', command_len)) {
                snprintf(reply, n, "ERR carriage return in command\n"); return -1;
            }
            cmd[command_len] = '\0';
            return 0;
        }
    }
    snprintf(reply, n, used >= size - 1 ? "ERR command too long\n" : "ERR command timeout (newline required)\n");
    return -1;
}
static int serve(void) {
    int server = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server < 0) { perror("socket"); return 1; }
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", socket_path);
    if (access(socket_path, F_OK) == 0) { fprintf(stderr,"Socket exists: %s\n",socket_path); close(server); return 1; }
    mode_t old = umask(0077);
    int bound = bind(server, (struct sockaddr *)&addr, sizeof(addr));
    umask(old);
    if (bound < 0 || listen(server, 8) < 0) { perror("bind/listen"); close(server); return 1; }
    printf("agent-pointer is ready.\n"
           "  Seat: agent (virtual keyboard and pointer)\n"
           "  Keymap: copied from seat0; restart after keyboard configuration changes\n"
           "  Socket: %s\n", socket_path);
    fflush(stdout);
    int wayland_fd = wl_display_get_fd(display);
    while (running) {
        while (wl_display_prepare_read(display) != 0) {
            if (wl_display_dispatch_pending(display) < 0) { running = 0; break; }
        }
        if (!running) break;
        if (wl_display_flush(display) < 0 && errno != EAGAIN) { wl_display_cancel_read(display); break; }
        struct pollfd fds[2] = {{wayland_fd, POLLIN, 0}, {server, POLLIN, 0}};
        int ret = poll(fds, 2, 500);
        if (ret < 0) { wl_display_cancel_read(display); if (errno == EINTR) continue; perror("poll"); break; }
        if (fds[0].revents & POLLIN) {
            if (wl_display_read_events(display) < 0 || wl_display_dispatch_pending(display) < 0) break;
        } else wl_display_cancel_read(display);
        if (fds[0].revents & (POLLERR|POLLHUP|POLLNVAL)) break;
        if (fds[1].revents & POLLIN) {
            int client = accept(server, NULL, NULL);
            if (client >= 0) {
                char cmd[4096], reply[2048];
                int quit = 0;
                if (read_command(client, cmd, sizeof(cmd), reply, sizeof(reply)) == 0) {
                    quit = handle_command(cmd, reply, sizeof(reply));
                    if (wl_display_flush(display) < 0 && errno != EAGAIN)
                        snprintf(reply, sizeof(reply), "ERR Wayland flush failed\n");
                }
                (void)send(client, reply, strlen(reply), MSG_NOSIGNAL);
                if (quit) running = 0;
                close(client);
            }
        }
    }
    release_held_buttons();
    close(server);
    unlink(socket_path);
    return 0;
}
static int create_agent_seat(void) {
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return -1; }
    if (pid == 0) {
        execlp("swaymsg", "swaymsg", "-q", "seat agent fallback disable", (char *)NULL);
        _exit(127);
    }
    int status;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) { perror("waitpid"); return -1; }
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}
int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--no-create-seat")) no_create_seat = 1;
        else {
            fprintf(stderr, "Usage: %s [--no-create-seat]\n", argv[0]);
            return 2;
        }
    }
    signal(SIGINT, stop_handler);
    signal(SIGTERM, stop_handler);
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    if (!runtime || strlen(runtime) > sizeof(socket_path)-32) { fprintf(stderr,"Invalid XDG_RUNTIME_DIR\n"); return 1; }
    snprintf(socket_path,sizeof(socket_path),"%s/agent-pointer.sock",runtime);
    if (access(socket_path, F_OK) == 0) {
        fprintf(stderr, "Socket already exists: %s (stop previous daemon first)\n", socket_path);
        return 1;
    }
    display = wl_display_connect(NULL);
    if (!display) { fprintf(stderr,"Cannot connect to Wayland\n"); return 1; }
    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    if (wl_display_roundtrip(display) < 0 || wl_display_roundtrip(display) < 0 || !manager || !keyboard_manager) {
        fprintf(stderr,"Missing virtual pointer manager v2 or keyboard manager\n"); return 1;
    }
    /* The source keyboard sends its keymap asynchronously after get_keyboard. */
    if (wl_display_roundtrip(display) < 0 || !seat0_keymap) {
        fprintf(stderr,"Cannot obtain effective keymap from seat0 (error=%d); refusing to guess\n", seat0_keymap_error);
        return 1;
    }
    if (!seat_found) {
        if (no_create_seat) {
            fprintf(stderr,"Agent seat absent (--no-create-seat)\n"); return 1;
        }
        fprintf(stderr,"Creating the agent seat; physical input stays on seat0.\n");
        if (create_agent_seat() < 0) {
            fprintf(stderr,"Cannot create the agent seat; run swaymsg 'seat agent fallback disable' for details\n"); return 1;
        }
        if (wl_display_roundtrip(display) < 0 || wl_display_roundtrip(display) < 0 || !seat_found) {
            fprintf(stderr,"Agent seat was not advertised after creation\n"); return 1;
        }
    }
    pointer = zwlr_virtual_pointer_manager_v1_create_virtual_pointer_with_output(manager, agent, NULL);
    if (!pointer || wl_display_roundtrip(display) < 0) { fprintf(stderr,"Cannot create virtual pointer\n"); return 1; }
    if (setup_keyboard() < 0) { fprintf(stderr,"Cannot create virtual keyboard using seat0 keymap\n"); return 1; }
    int result = serve();
    xkb_keymap_unref(keymap);
    xkb_keymap_unref(seat0_keymap);
    xkb_context_unref(xkb_ctx);
    wl_display_disconnect(display);
    return result;
}
