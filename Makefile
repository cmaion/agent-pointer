CC ?= cc
PKG_CONFIG ?= pkg-config
WAYLAND_SCANNER ?= wayland-scanner
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
PYTHON ?= python3

# Vendored protocol definitions; override to use a local upstream checkout.
VIRTUAL_POINTER_XML ?= protocols/wlr-virtual-pointer-unstable-v1.xml
VIRTUAL_KEYBOARD_XML ?= protocols/virtual-keyboard-unstable-v1.xml

PKGS := wayland-client xkbcommon json-c
CFLAGS ?= -O2 -g
CPPFLAGS += -D_POSIX_C_SOURCE=200809L
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic

GENERATED := virtual-pointer-client-protocol.h virtual-pointer-protocol.c \
             virtual-keyboard-client-protocol.h virtual-keyboard-protocol.c

.PHONY: all clean test check-deps install uninstall
all: agent-pointer

check-deps:
	@$(PKG_CONFIG) --exists $(PKGS) || { echo 'Missing pkg-config dependencies: $(PKGS)' >&2; exit 1; }
	@$(PKG_CONFIG) --atleast-version=1.0.0 xkbcommon || { echo 'libxkbcommon >= 1.0.0 is required' >&2; exit 1; }
	@command -v $(WAYLAND_SCANNER) >/dev/null || { echo 'Missing wayland-scanner' >&2; exit 1; }

agent-pointer tests/test_daemon: | check-deps

virtual-pointer-client-protocol.h: $(VIRTUAL_POINTER_XML)
	$(WAYLAND_SCANNER) client-header $< $@
virtual-pointer-protocol.c: $(VIRTUAL_POINTER_XML)
	$(WAYLAND_SCANNER) private-code $< $@
virtual-keyboard-client-protocol.h: $(VIRTUAL_KEYBOARD_XML)
	$(WAYLAND_SCANNER) client-header $< $@
virtual-keyboard-protocol.c: $(VIRTUAL_KEYBOARD_XML)
	$(WAYLAND_SCANNER) private-code $< $@

agent-pointer: agent-pointer.c $(GENERATED)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(shell $(PKG_CONFIG) --cflags $(PKGS)) \
	  -o $@ agent-pointer.c virtual-pointer-protocol.c virtual-keyboard-protocol.c \
	  $(LDFLAGS) $(shell $(PKG_CONFIG) --libs $(PKGS)) $(LDLIBS)

tests/test_daemon: tests/test_daemon.c agent-pointer.c $(GENERATED)
	$(CC) $(CPPFLAGS) $(CFLAGS) -UNDEBUG $(shell $(PKG_CONFIG) --cflags $(PKGS)) \
	  -o $@ tests/test_daemon.c virtual-pointer-protocol.c virtual-keyboard-protocol.c \
	  $(LDFLAGS) -Wl,--wrap=wl_proxy_get_version -Wl,--wrap=wl_proxy_marshal_flags \
	  -Wl,--wrap=wl_display_roundtrip $(shell $(PKG_CONFIG) --libs $(PKGS)) $(LDLIBS)

test: tests/test_daemon
	./tests/test_daemon
	$(PYTHON) -m unittest discover -s tests -v
	$(PYTHON) -m py_compile agent-pointer-mcp.py

install: agent-pointer
	install -d $(DESTDIR)$(BINDIR)
	install -m 0755 agent-pointer $(DESTDIR)$(BINDIR)/agent-pointer
	install -m 0755 agent-pointer-mcp.py $(DESTDIR)$(BINDIR)/agent-pointer-mcp

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/agent-pointer $(DESTDIR)$(BINDIR)/agent-pointer-mcp

clean:
	rm -f agent-pointer tests/test_daemon $(GENERATED)
