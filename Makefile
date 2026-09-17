CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -Wall -Wextra -Wpedantic -std=c17
BUILD_DIR := build
OBJ_DIR := $(BUILD_DIR)/obj
CONFIG_EXAMPLE := config/services.conf.example

# Installing to a home directory is the default because that is where a
# per-user facilitator belongs.  A distribution package overrides both:
#
#   make PREFIX=/usr DESTDIR="$$pkgdir" install
#
PREFIX ?= $(HOME)/.local
BINDIR ?= $(PREFIX)/bin
LIBDIR ?= $(PREFIX)/lib
DATADIR ?= $(PREFIX)/share

# The release version stands on its own so that a build from a source
# tarball, which has no git history to ask, still reports something
# meaningful.  A working tree adds the revision it was built from.
VERSION ?= 0.1.0
GIT_REVISION := $(shell git rev-parse --short HEAD 2>/dev/null)
GIT_DIRTY := $(shell test -n "$$(git status --porcelain 2>/dev/null)" && echo -dirty)
ifeq ($(GIT_REVISION),)
FULL_VERSION := $(VERSION)
else
FULL_VERSION := $(VERSION)+$(GIT_REVISION)$(GIT_DIRTY)
endif

CPPFLAGS += -Isrc -DPROXENOS_VERSION=\"$(FULL_VERSION)\"

# Each program compiles against the libraries it actually uses: the command
# line tool and the router need GLib alone, the window needs GTK, and only the
# panel plugin needs the XFCE panel headers.
GLIB_CFLAGS := $(shell pkg-config --cflags glib-2.0)
GLIB_LIBS := $(shell pkg-config --libs glib-2.0)
GTK_CFLAGS := $(shell pkg-config --cflags gtk+-3.0)
GTK_LIBS := $(shell pkg-config --libs gtk+-3.0)
PANEL_CFLAGS := $(shell pkg-config --cflags libxfce4panel-2.0)
PANEL_LIBS := $(shell pkg-config --libs libxfce4panel-2.0)

PROGRAMS := $(BUILD_DIR)/proxenos $(BUILD_DIR)/proxenos-cli $(BUILD_DIR)/proxenos-router $(BUILD_DIR)/proxenos-panel.so

.PHONY: all clean check install install-config update-icon-cache uninstall

all: $(PROGRAMS)

$(OBJ_DIR):
	mkdir -p $@

$(OBJ_DIR)/%.o: src/%.c src/proxenos-common.h | $(OBJ_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(GLIB_CFLAGS) -c -o $@ $<

$(OBJ_DIR)/proxenos-ui.o: src/proxenos-ui.c src/proxenos-common.h | $(OBJ_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(GTK_CFLAGS) -c -o $@ $<

# The panel plugin is a shared module, so its objects are position independent.
$(OBJ_DIR)/proxenos-panel.o: src/proxenos-panel.c src/proxenos-common.h | $(OBJ_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(PANEL_CFLAGS) -fPIC -c -o $@ $<

$(OBJ_DIR)/proxenos-common-pic.o: src/proxenos-common.c src/proxenos-common.h | $(OBJ_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(GLIB_CFLAGS) -fPIC -c -o $@ $<

$(BUILD_DIR)/proxenos-cli: $(OBJ_DIR)/proxenos.o $(OBJ_DIR)/proxenos-common.o
	$(CC) $(CFLAGS) -o $@ $^ $(GLIB_LIBS)

$(BUILD_DIR)/proxenos-router: $(OBJ_DIR)/proxenos-router.o $(OBJ_DIR)/proxenos-common.o
	$(CC) $(CFLAGS) -o $@ $^ $(GLIB_LIBS)

$(BUILD_DIR)/proxenos: $(OBJ_DIR)/proxenos-ui.o $(OBJ_DIR)/proxenos-common.o
	$(CC) $(CFLAGS) -o $@ $^ $(GTK_LIBS)

$(BUILD_DIR)/proxenos-panel.so: $(OBJ_DIR)/proxenos-panel.o $(OBJ_DIR)/proxenos-common-pic.o
	$(CC) $(CFLAGS) -shared -o $@ $^ $(PANEL_LIBS)

# Exercises the helpers and the router's parsing without touching the
# installed configuration or binding any port.
TESTS := $(BUILD_DIR)/proxenos-selftest $(BUILD_DIR)/proxenos-router-test

check: $(TESTS)
	@for test in $(TESTS); do $$test || exit 1; done

$(BUILD_DIR)/proxenos-selftest: tests/selftest.c src/proxenos-common.c src/proxenos-common.h | $(OBJ_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(GLIB_CFLAGS) -o $@ tests/selftest.c src/proxenos-common.c $(GLIB_LIBS)

# The test includes the router source, so its unused helpers are expected.
$(BUILD_DIR)/proxenos-router-test: tests/router-test.c src/proxenos-router.c src/proxenos-common.c | $(OBJ_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(GLIB_CFLAGS) -Wno-unused-function -o $@ \
		tests/router-test.c src/proxenos-common.c $(GLIB_LIBS)

install: all install-config
	install -Dm755 $(BUILD_DIR)/proxenos $(DESTDIR)$(BINDIR)/proxenos
	install -Dm755 $(BUILD_DIR)/proxenos-cli $(DESTDIR)$(BINDIR)/proxenos-cli
	install -Dm755 $(BUILD_DIR)/proxenos-router $(DESTDIR)$(BINDIR)/proxenos-router
	install -Dm755 scripts/proxenos-manjaro-routing $(DESTDIR)$(BINDIR)/proxenos-manjaro-routing
	install -Dm755 $(BUILD_DIR)/proxenos-panel.so $(DESTDIR)$(LIBDIR)/xfce4/panel/plugins/proxenos-panel.so
	install -Dm644 desktop/proxenos.desktop $(DESTDIR)$(DATADIR)/applications/proxenos.desktop
	install -Dm644 desktop/proxenos-panel.desktop $(DESTDIR)$(DATADIR)/xfce4/panel/plugins/proxenos-panel.desktop
	install -Dm644 assets/proxenos.svg $(DESTDIR)$(DATADIR)/icons/hicolor/scalable/apps/proxenos.svg
	install -Dm644 assets/icons/hicolor/scalable/apps/proxenos-symbolic.svg $(DESTDIR)$(DATADIR)/icons/hicolor/scalable/apps/proxenos-symbolic.svg
	install -Dm644 $(CONFIG_EXAMPLE) $(DESTDIR)$(DATADIR)/proxenos/services.conf.example
	install -Dm644 LICENSE $(DESTDIR)$(DATADIR)/licenses/proxenos/LICENSE
	@$(MAKE) --no-print-directory update-icon-cache

# GTK reads an installed icon through $(DATADIR)/icons/hicolor/icon-theme.cache
# when that file is at least as new as the theme root, and installing a new
# icon touches only the scalable/apps subdirectory.  An icon added next to a
# cache written earlier is therefore invisible: the program looks it up, GTK
# says no, and the program quietly uses whatever fallback it has.  Rebuilding
# the cache is the fix; where the tool is missing, making the root newer than
# the cache is enough, because GTK then ignores the cache and reads the
# directories.  A packaging build does neither, since the package manager runs
# its own hook after the files are in place.
update-icon-cache:
ifeq ($(DESTDIR),)
	@if command -v gtk-update-icon-cache >/dev/null 2>&1; then \
		gtk-update-icon-cache -q -t -f "$(DATADIR)/icons/hicolor" && \
		echo "refreshed $(DATADIR)/icons/hicolor/icon-theme.cache"; \
	else \
		touch "$(DATADIR)/icons/hicolor"; \
		echo "gtk-update-icon-cache not found: touched $(DATADIR)/icons/hicolor instead"; \
	fi
else
	@echo "packaging build: icon cache left to the package manager"
endif

# Seeds the live configuration once, and never overwrites it.  A package build
# sets DESTDIR and must not write into anyone's home directory, so this step
# does nothing there: the package ships the example, and services.conf appears
# the first time the owner of the account installs for themselves.
install-config:
ifeq ($(DESTDIR),)
	@if test ! -e "$(HOME)/.config/proxenos/services.conf"; then \
		install -Dm644 $(CONFIG_EXAMPLE) "$(HOME)/.config/proxenos/services.conf"; \
		echo "seeded $(HOME)/.config/proxenos/services.conf"; \
	fi
else
	@echo "packaging build: $(HOME)/.config left untouched"
endif

# Remove all installed Proxenos programs and desktop integrations before a
# clean reinstall. Service registrations are deliberately preserved.
uninstall:
	rm -f $(DESTDIR)$(BINDIR)/proxenos
	rm -f $(DESTDIR)$(BINDIR)/proxenos-cli
	rm -f $(DESTDIR)$(BINDIR)/proxenos-ui
	rm -f $(DESTDIR)$(BINDIR)/proxenos-router
	rm -f $(DESTDIR)$(BINDIR)/proxenos-manjaro-routing
	rm -f $(DESTDIR)$(LIBDIR)/xfce4/panel/plugins/proxenos-panel.so
	rm -f $(DESTDIR)$(DATADIR)/applications/proxenos.desktop
	rm -f $(DESTDIR)$(DATADIR)/xfce4/panel/plugins/proxenos-panel.desktop
	rm -f $(DESTDIR)$(DATADIR)/icons/hicolor/scalable/apps/proxenos.svg
	rm -f $(DESTDIR)$(DATADIR)/icons/hicolor/scalable/apps/proxenos-symbolic.svg
	rm -f $(DESTDIR)$(DATADIR)/proxenos/services.conf.example
	rm -f $(DESTDIR)$(DATADIR)/licenses/proxenos/LICENSE

clean:
	rm -rf $(BUILD_DIR)
