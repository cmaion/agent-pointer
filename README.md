# agent-pointer

`agent-pointer` provides virtual keyboard and pointer control for **Sway/SwayFX**
through a local Unix socket and MCP bridge. It lets trusted local agents inspect
windows and screenshots, then operate applications through their visible UI.
This helps with desktop tasks in apps that have no agent-facing API, and opens
up UI-based automation and exploration without needing app-specific integrations.

The daemon puts virtual input on a dedicated Wayland seat named `agent`,
leaving physical devices on `seat0`. A Unix socket accepts input commands;
the Python MCP bridge adds window discovery and screenshots via `swaymsg`
and `grim`.

Tested by the project author on SwayFX 0.6 / Sway 1.12, Ubuntu 26.04.1,
with two outputs. Other compositors have not been verified.

## Build and run

Requirements: a running Sway/SwayFX session, `swaymsg`, a C compiler, Make,
`pkg-config`, `wayland-scanner`, Wayland client development files,
libxkbcommon **1.0.0 or newer**, and json-c. The MCP bridge needs Python 3;
screenshots also need `grim`. `socat` is optional for manual commands.

On Ubuntu:

```bash
sudo apt install build-essential pkg-config libwayland-dev libxkbcommon-dev \
  libjson-c-dev libwayland-bin grim socat python3
make
./agent-pointer
```

Run the daemon inside your Sway session. It creates the `agent` seat if absent
using `swaymsg 'seat agent fallback disable'`. Use `--no-create-seat` to require
an existing seat. **Never enable fallback on `agent`: physical devices must
remain on `seat0`.**

The required protocol XMLs are included in [`protocols/`](protocols/README.md).
Make generates the client bindings and builds the executable locally; generated
files and binaries are not tracked. To use different protocol definitions:

```bash
make VIRTUAL_POINTER_XML=/path/to/wlr-virtual-pointer-unstable-v1.xml \
     VIRTUAL_KEYBOARD_XML=/path/to/virtual-keyboard-unstable-v1.xml
```

### Install (optional)

```bash
sudo make install                    # /usr/local/bin
# Or install for your user:
make install PREFIX="$HOME/.local"
```

Both `agent-pointer` and `agent-pointer-mcp` are installed. Use `make uninstall`
with the same prefix (and `sudo` when needed) to remove them. `make clean`
removes local build outputs.

## Connect Codex

Start the daemon, then register the MCP bridge **from a terminal inside your
Sway session**:

```bash
codex mcp add agent-pointer \
  --env "SWAYSOCK=$SWAYSOCK" \
  --env "WAYLAND_DISPLAY=$WAYLAND_DISPLAY" \
  --env "XDG_RUNTIME_DIR=$XDG_RUNTIME_DIR" \
  -- python3 /usr/local/bin/agent-pointer-mcp

codex mcp list
```

For a user-local installation, substitute `"$HOME/.local/bin/agent-pointer-mcp"`.
Without installing, use the absolute path to `agent-pointer-mcp.py`.

Remove an existing registration with `codex mcp remove agent-pointer` before
re-adding it. Restart the Codex session after configuration changes. `Auth:
Unsupported` is normal for this local stdio server; `Status: enabled` only means
it is configured. Invoke `status` to check the daemon connection.

## Connect OpenCode v2

Start the daemon, then register the local MCP server from a terminal inside
your Sway session:

```sh
opencode mcp add agent-pointer -- \
  python3 /usr/local/bin/agent-pointer-mcp
opencode mcp list
```

Replace the bridge path with its absolute path: for a user-local install, use
`python3 "$HOME/.local/bin/agent-pointer-mcp"`; without installing, use the
absolute path to `agent-pointer-mcp.py`. Add `--global` before `--` to make the
registration available in all projects.

The bridge needs `SWAYSOCK`, `WAYLAND_DISPLAY`, and `XDG_RUNTIME_DIR` from the
OpenCode server's environment. Make sure that server is running inside the
active Sway session. The CLI command registers the server but does not set
per-server environment variables; if the server does not inherit them, use the
`environment` option in OpenCode's [MCP server config](https://opencode.ai/v2/docs/mcp-servers/).

Use `/mcps` in OpenCode to check the server, then ask the agent to invoke
`status` to verify the daemon connection.

## MCP tools

The bridge uses newline-delimited JSON-RPC over stdin/stdout and needs no
third-party Python packages. Its environment must point to the active Sway
session: `SWAYSOCK` for IPC, and `WAYLAND_DISPLAY` / `XDG_RUNTIME_DIR` for Wayland
and the input socket. `AGENT_POINTER_SOCKET` can override the bridge's socket
path.

| Tool | Purpose |
| --- | --- |
| `status` | Check daemon connection and keymap |
| `list_outputs` | List active outputs and logical geometry |
| `list_windows` | List window metadata, including hidden windows and visibility |
| `screenshot` / `screenshot_region` | Capture an output or logical region as PNG |
| `move_pointer` | Move to local logical coordinates on a named output |
| `focus_window` | Target a visible window by ID using the agent pointer |
| `click` | Click left, right or middle |
| `button_down` / `button_up` | Hold and release a button for dragging |
| `release_buttons` | Release all held buttons |
| `scroll` | Scroll vertically or horizontally, from −100 to 100 steps |
| `press_key` | Press a named XKB keysym, such as `Return` or `Escape` |
| `key_chord` | Send a shortcut such as `Ctrl+A` or `Ctrl+Shift+Tab` |
| `type_text` | Type single-line text available in the captured XKB keymap |

`focus_window` rejects hidden windows. It does not switch workspaces, issue IPC
focus commands, or guarantee keyboard focus. Its optional `click: true` can
activate a control under the pointer; it defaults to `false`.

Example prompts:

> Call `status`, `list_outputs`, and `list_windows`. Summarize the desktop without
> moving the pointer, clicking, or typing.

> Capture a screenshot of `eDP-1` and describe what is visible. Do not interact
> with applications.

> Identify the visible Terminal window and target it with `focus_window`,
> `click: false`. Ask me before clicking or entering text.

## Unix socket commands

The daemon listens on `$XDG_RUNTIME_DIR/agent-pointer.sock`. Each connection
accepts **one newline-terminated command**, up to 4095 bytes including the
newline, with a two-second read deadline. Incomplete and oversized commands
are rejected without executing their prefix. Replies start with `OK` or `ERR`.

```bash
SOCK="$XDG_RUNTIME_DIR/agent-pointer.sock"
printf 'status\n'                 | socat - UNIX-CONNECT:"$SOCK"
printf 'outputs\n'                | socat - UNIX-CONNECT:"$SOCK"
printf 'move-to eDP-1 720 450\n'   | socat - UNIX-CONNECT:"$SOCK"
printf 'click left\n'             | socat - UNIX-CONNECT:"$SOCK"
printf 'chord Ctrl+A\n'           | socat - UNIX-CONNECT:"$SOCK"
printf 'type Hello from agent!\n' | socat - UNIX-CONNECT:"$SOCK"
```

| Command | Arguments |
| --- | --- |
| `status`, `outputs`, `release`, `quit` | None |
| `move-to` | `OUTPUT X Y`: local logical output coordinates |
| `move` | `X Y WIDTH HEIGHT`: normalized coordinates across the output layout |
| `click`, `down`, `up` | `left`, `right`, or `middle` |
| `scroll` | `vertical` or `horizontal`, then signed steps (−100 to 100) |
| `chord` | A shortcut such as `Ctrl+A`; final key must be a base-level symbol in the first layout |
| `key` | An XKB keysym name |
| `type` | Single-line UTF-8 text |

For a drag, send `down left`, one or more `move-to` commands, then `up left` in
separate connections. Use `release` to recover from a held button.

Coordinates use **Sway logical units**, not necessarily physical pixels.
`outputs` reports logical dimensions and global positions. Screenshot pixel
dimensions may differ on scaled displays.

## Keyboard behavior

At startup, the daemon captures `seat0`'s effective compiled XKB keymap and
keeps a referenced snapshot for both lookup and virtual input. **Restart the
daemon after changing keyboard configuration** to use the new keymap.

Text and named keys use the matched layout group and the modifier combinations
specified by XKB, including keypad and nonstandard key types. This is not
arbitrary Unicode insertion: characters must be directly available in the
keymap. Compose sequences and input methods are not synthesized. A text request
can type a prefix before encountering an unavailable character and returning
an error; do not blindly retry it. MCP text is limited to 4089 UTF-8 bytes.

## Tests

```bash
make test
```

The C tests capture Wayland requests and check layout groups, key types, keymap
ownership, and fragmented socket commands. Python tests use a fake daemon and
mocked Sway/grim responses for MCP, validation, window targeting, and screenshot
geometry. Tests require the build dependencies and installed XKB layout data,
but **no compositor or real desktop input**.

Before release, also test on Sway: output discovery and movement, a click and
drag in a disposable window, text and shortcuts, and daemon restart after a
keyboard configuration change.

## Security and limitations

- For local, trusted automation only. The socket has owner-only permissions,
  but any process with access can send real input.
- Separate seats are **not a security or visual privacy boundary**. Screenshots
  can capture sensitive information from either seat.
- Pointer movement may affect compositor focus-on-hover policy. Overlapping
  windows and changing desktop state can make geometric targeting unsafe, even
  when the target window is visible.
- Shells and applications must support multiple Wayland seats. Noctalia 5.2.1
  was observed to require a local fix for handling additional `wl_seat` globals.

## Troubleshooting

- **Observation tools fail:** check `SWAYSOCK`, `WAYLAND_DISPLAY`, and
  `XDG_RUNTIME_DIR` in the MCP environment. Verify `grim` is installed.
- **Codex shows enabled but tools fail:** restart Codex, then invoke `status`.
- **Socket already exists:** stop the previous daemon. After an unclean exit,
  remove the stale socket only after confirming no daemon is running.
- **Keyboard configuration changed:** restart the daemon to refresh its snapshot.

## License

[MIT](LICENSE). Vendored protocol definitions retain their upstream copyright
and license notices.
