BINARY_NAME := extra-type
VIZ_NAME    := extra-type-settings
PREFIX      ?= /usr/local
BINDIR      ?= $(PREFIX)/bin
SHAREDIR    ?= $(PREFIX)/share
APPSDIR     ?= $(SHAREDIR)/applications
ICONDIR     ?= $(SHAREDIR)/icons/hicolor/scalable/apps

DESKTOP_TEMPLATE := extra-type.desktop.in
DESKTOP_FILE     := extra-type.desktop
ICON_FILE        := extra-type.svg

CC         ?= gcc
CFLAGS     ?= -O2 -Wall -Wextra -Wpedantic
PKG_CONFIG ?= pkg-config

VIZ_CFLAGS  := $(shell $(PKG_CONFIG) --cflags gtk4 gtk4-layer-shell-0 libpipewire-0.3 libspa-0.2)
VIZ_LDFLAGS := $(shell $(PKG_CONFIG) --libs gtk4 gtk4-layer-shell-0 libpipewire-0.3 libspa-0.2) -lm

VIZ_CFLAGS_SETTINGS := $(shell $(PKG_CONFIG) --cflags gtk4 libadwaita-1 libsoup-3.0 json-glib-1.0)
VIZ_LDFLAGS_SETTINGS := $(shell $(PKG_CONFIG) --libs gtk4 libadwaita-1 libsoup-3.0 json-glib-1.0)

SRC_TS := $(shell find src -name '*.ts' -o -name '*.js' | sort)

.PHONY: all install uninstall clean

all: daemon viz

build:
	mkdir -p build

build/$(BINARY_NAME): $(SRC_TS) | build
	bun build src/index.ts --compile --outfile $@

build/extra-type-viz: viz/spectrum.c | build
	$(CC) $(CFLAGS) $(VIZ_CFLAGS) viz/spectrum.c -o $@ $(VIZ_LDFLAGS)

build/$(VIZ_NAME): viz/settingsWindow.c | build
	$(CC) $(CFLAGS) $(VIZ_CFLAGS_SETTINGS) viz/settingsWindow.c -o $@ $(VIZ_LDFLAGS_SETTINGS)

build/extra-type-hotkey: viz/hotkey.c | build
	$(CC) $(CFLAGS) viz/hotkey.c -o $@

daemon viz: build/$(BINARY_NAME) build/extra-type-viz build/$(VIZ_NAME) build/extra-type-hotkey

install: build/$(BINARY_NAME) build/extra-type-viz build/$(VIZ_NAME) build/extra-type-hotkey
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 build/$(BINARY_NAME) $(DESTDIR)$(BINDIR)/$(BINARY_NAME)
	install -m 755 build/extra-type-viz $(DESTDIR)$(BINDIR)/extra-type-viz
	install -m 755 build/$(VIZ_NAME) $(DESTDIR)$(BINDIR)/$(VIZ_NAME)
	install -m 755 build/extra-type-hotkey $(DESTDIR)$(BINDIR)/extra-type-hotkey
	install -d $(DESTDIR)$(APPSDIR) $(DESTDIR)$(ICONDIR)
	sed -e 's|@BINDIR@|$(BINDIR)|g' $(DESKTOP_TEMPLATE) > $(DESTDIR)$(APPSDIR)/$(DESKTOP_FILE)
	install -m 644 $(ICON_FILE) $(DESTDIR)$(ICONDIR)/$(ICON_FILE)

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(BINARY_NAME)
	rm -f $(DESTDIR)$(BINDIR)/extra-type-viz
	rm -f $(DESTDIR)$(BINDIR)/$(VIZ_NAME)
	rm -f $(DESTDIR)$(BINDIR)/extra-type-hotkey
	rm -f $(DESTDIR)$(APPSDIR)/$(DESKTOP_FILE)
	rm -f $(DESTDIR)$(ICONDIR)/$(ICON_FILE)

clean:
	rm -rf build
