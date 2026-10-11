# Wayland protocol XML definitions

This directory includes the upstream protocol definitions needed to build:

- `wlr-virtual-pointer-unstable-v1.xml`
- `virtual-keyboard-unstable-v1.xml`

Both XML files retain their upstream copyright and MIT-style license notices.
They are not necessarily included in the distribution's `wayland-protocols`
package. No download is required during the build.

To use definitions from a trusted wlroots / wlr-protocols checkout instead:

```sh
make VIRTUAL_POINTER_XML=/path/to/wlr-virtual-pointer-unstable-v1.xml \
     VIRTUAL_KEYBOARD_XML=/path/to/virtual-keyboard-unstable-v1.xml
```

The generated headers and C files are produced by `wayland-scanner` and are ignored
by Git. The repository does not silently download external build inputs.
