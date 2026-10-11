/* Exercise daemon internals with captured Wayland requests, never real input. */
#define main daemon_main
#include "../agent-pointer.c"
#undef main
#include <assert.h>
#include <stdarg.h>

static struct xkb_state *received_state;
static xkb_keysym_t received_keys[64];
static size_t received_count;
static unsigned received_releases;
static unsigned keymap_uploads;

uint32_t __wrap_wl_proxy_get_version(struct wl_proxy *proxy) {
    (void)proxy;
    return 1;
}

int __wrap_wl_display_roundtrip(struct wl_display *d) {
    (void)d;
    return 0;
}

struct wl_proxy *__wrap_wl_proxy_marshal_flags(struct wl_proxy *proxy,
        uint32_t opcode, const struct wl_interface *interface,
        uint32_t version, uint32_t flags, ...) {
    (void)interface; (void)version; (void)flags;
    if (proxy == (struct wl_proxy *)keyboard_manager)
        return (struct wl_proxy *)keyboard;
    assert(proxy == (struct wl_proxy *)keyboard);
    va_list ap;
    va_start(ap, flags);
    if (opcode == ZWP_VIRTUAL_KEYBOARD_V1_KEYMAP) {
        assert(va_arg(ap, uint32_t) == WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1);
        int fd = va_arg(ap, int);
        uint32_t size = va_arg(ap, uint32_t);
        char *data = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
        assert(data != MAP_FAILED);
        struct xkb_keymap *uploaded = xkb_keymap_new_from_string(
            xkb_ctx, data, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
        assert(uploaded);
        if (received_state) xkb_state_unref(received_state);
        received_state = xkb_state_new(uploaded);
        assert(received_state);
        xkb_keymap_unref(uploaded);
        munmap(data, size);
        ++keymap_uploads;
    } else if (opcode == ZWP_VIRTUAL_KEYBOARD_V1_MODIFIERS) {
        uint32_t depressed = va_arg(ap, uint32_t);
        uint32_t latched = va_arg(ap, uint32_t);
        uint32_t locked = va_arg(ap, uint32_t);
        uint32_t group = va_arg(ap, uint32_t);
        xkb_state_update_mask(received_state, depressed, latched, locked, 0, 0, group);
    } else if (opcode == ZWP_VIRTUAL_KEYBOARD_V1_KEY) {
        (void)va_arg(ap, uint32_t);
        uint32_t evdev = va_arg(ap, uint32_t);
        uint32_t state = va_arg(ap, uint32_t);
        if (state == WL_KEYBOARD_KEY_STATE_PRESSED) {
            assert(received_count < sizeof(received_keys) / sizeof(received_keys[0]));
            received_keys[received_count++] = xkb_state_key_get_one_sym(received_state, evdev + 8);
        } else {
            ++received_releases;
        }
    } else {
        assert(0 && "Unexpected Wayland request");
    }
    va_end(ap);
    return NULL;
}

static void expect_keysym(const char *name) {
    xkb_keysym_t sym = xkb_keysym_from_name(name, XKB_KEYSYM_NO_FLAGS);
    assert(sym != XKB_KEY_NoSymbol);
    received_count = received_releases = 0;
    assert(send_keysym(sym) == 0);
    assert(received_count == 1 && received_keys[0] == sym);
    assert(received_releases == 1);
    assert(xkb_state_serialize_mods(received_state, XKB_STATE_MODS_EFFECTIVE) == 0);
    assert(xkb_state_serialize_layout(received_state, XKB_STATE_LAYOUT_EFFECTIVE) == 0);
}

static void test_keyboard(void) {
    xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    assert(xkb_ctx);
    struct xkb_rule_names names = {.layout = "us,ru"};
    seat0_keymap = xkb_keymap_new_from_names(xkb_ctx, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
    assert(seat0_keymap);
    keyboard_manager = (struct zwp_virtual_keyboard_manager_v1 *)(uintptr_t)1;
    keyboard = (struct zwp_virtual_keyboard_v1 *)(uintptr_t)2;
    assert(setup_keyboard() == 0);
    assert(keymap_uploads == 1);
    expect_keysym("Cyrillic_ef");
    expect_keysym("KP_1");
    expect_keysym("A");
    expect_keysym("exclam");
    expect_keysym("Return");

    received_count = received_releases = 0;
    assert(send_text("аA!") == 0);
    assert(received_count == 3 && received_releases == 3);
    assert(received_keys[0] == XKB_KEY_Cyrillic_a);
    assert(received_keys[1] == XKB_KEY_A && received_keys[2] == XKB_KEY_exclam);
    received_count = 0;
    assert(send_keysym(xkb_utf32_to_keysym(0x1f600)) == -1);
    assert(received_count == 0);

    /* Feed an actual keymap callback, freeing seat0's original reference.
     * The lookup snapshot must remain usable and match the uploaded map. */
    names.layout = "fr";
    struct xkb_keymap *replacement = xkb_keymap_new_from_names(
        xkb_ctx, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
    assert(replacement);
    char *data = xkb_keymap_get_as_string(replacement, XKB_KEYMAP_FORMAT_TEXT_V1);
    assert(data);
    FILE *file = tmpfile();
    assert(file);
    size_t size = strlen(data) + 1;
    assert(fwrite(data, 1, size, file) == size);
    assert(fflush(file) == 0);
    int fd = dup(fileno(file));
    assert(fd >= 0);
    source_keymap(NULL, NULL, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, (uint32_t)size);
    assert(!seat0_keymap_error && keymap != seat0_keymap);
    assert(keymap_uploads == 1);
    expect_keysym("Cyrillic_ef");
    expect_keysym("A");
    fclose(file);
    free(data);
    xkb_keymap_unref(replacement);
    xkb_keymap_unref(keymap);
    xkb_keymap_unref(seat0_keymap);
    xkb_state_unref(received_state);
    xkb_context_unref(xkb_ctx);
    puts("PASS keyboard: layouts, key types, mixed text, unavailable symbols, keymap replacement");
}

static void expect_command(const char *data, size_t len, const char *expected,
                           const char *error) {
    int fds[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    assert(send(fds[0], data, len, 0) == (ssize_t)len);
    assert(shutdown(fds[0], SHUT_WR) == 0);
    char cmd[4096], reply[2048];
    int result = read_command(fds[1], cmd, sizeof(cmd), reply, sizeof(reply));
    if (expected) {
        assert(result == 0 && !strcmp(cmd, expected));
    } else {
        assert(result == -1 && strstr(reply, error));
    }
    close(fds[0]);
    close(fds[1]);
}

static void test_stream(void) {
    int fds[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        close(fds[1]);
        assert(send(fds[0], "type Hello", 10, 0) == 10);
        struct timespec delay = {.tv_nsec = 50000000};
        nanosleep(&delay, NULL);
        assert(send(fds[0], " world\n", 7, 0) == 7);
        close(fds[0]);
        _exit(0);
    }
    close(fds[0]);
    char cmd[4096], reply[2048];
    assert(read_command(fds[1], cmd, sizeof(cmd), reply, sizeof(reply)) == 0);
    assert(!strcmp(cmd, "type Hello world"));
    close(fds[1]);
    int status;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);

    expect_command("status\n", 7, "status", NULL);
    expect_command("status\r\n", 8, "status", NULL);
    expect_command("type Hello", 10, NULL, "incomplete");
    expect_command("status\0quit\n", 12, NULL, "NUL");
    expect_command("type Hello\rquit\n", 16, NULL, "carriage return");
    char max_command[4096];
    memset(max_command, 'a', sizeof(max_command));
    max_command[4094] = '\n';
    max_command[4095] = '\0';
    char expected[4095];
    memset(expected, 'a', 4094);
    expected[4094] = '\0';
    expect_command(max_command, 4095, expected, NULL);
    max_command[4094] = 'a';
    max_command[4095] = '\n';
    expect_command(max_command, sizeof(max_command), NULL, "too long");

    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    assert(send(fds[0], "status", 6, 0) == 6);
    uint32_t start = milliseconds();
    assert(read_command(fds[1], cmd, sizeof(cmd), reply, sizeof(reply)) == -1);
    assert(strstr(reply, "timeout") && milliseconds() - start >= 1900);
    close(fds[0]);
    close(fds[1]);
    puts("PASS stream: fragmentation, CRLF, EOF, NUL, size boundaries, timeout");
}

int main(void) {
    test_keyboard();
    test_stream();
    return 0;
}
