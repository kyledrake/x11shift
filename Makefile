CC ?= cc
PKG_CONFIG ?= pkg-config
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
DATADIR ?= $(PREFIX)/share

APP = x11shift
RESOURCE_SOURCE = src/x11shift-resources.c
SOURCES = src/main.c src/schedule.c src/x11_gamma.c src/resume_monitor.c $(RESOURCE_SOURCE)
OBJECTS = $(SOURCES:.c=.o)
PACKAGES = gtk+-3.0 glib-2.0 gio-2.0 x11 xrandr
CPPFLAGS += -Isrc
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -Wno-deprecated-declarations \
	$(shell $(PKG_CONFIG) --cflags $(PACKAGES))
LDLIBS += $(shell $(PKG_CONFIG) --libs $(PACKAGES)) -lm

.PHONY: all clean install uninstall test

all: $(APP)

$(APP): $(OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $(OBJECTS) $(LDLIBS)

$(RESOURCE_SOURCE): data/x11shift.gresource.xml data/x11shift.svg
	glib-compile-resources data/x11shift.gresource.xml --sourcedir=data \
		--target=$@ --generate-source

src/%.o: src/%.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

tests/test_schedule: tests/test_schedule.c src/schedule.c src/schedule.h
	$(CC) -Isrc -std=c11 -Wall -Wextra -Wpedantic -O2 \
		-o $@ tests/test_schedule.c src/schedule.c -lm

tests/test_gamma: tests/test_gamma.c src/x11_gamma.c src/x11_gamma.h
	$(CC) $(CPPFLAGS) -std=c11 -Wall -Wextra -Wpedantic -O2 \
		$(shell $(PKG_CONFIG) --cflags glib-2.0 x11 xrandr) \
		-o $@ tests/test_gamma.c src/x11_gamma.c \
		$(shell $(PKG_CONFIG) --libs glib-2.0) -lm

tests/test_resume: tests/test_resume.c src/resume_monitor.c src/resume_monitor.h
	$(CC) $(CPPFLAGS) -std=c11 -Wall -Wextra -Wpedantic -O2 \
		$(shell $(PKG_CONFIG) --cflags gio-2.0) \
		-o $@ tests/test_resume.c src/resume_monitor.c \
		$(shell $(PKG_CONFIG) --libs gio-2.0)

test: tests/test_schedule tests/test_gamma tests/test_resume
	./tests/test_schedule
	./tests/test_gamma
	./tests/test_resume

install: $(APP)
	install -d $(DESTDIR)$(BINDIR)
	install -m 0755 $(APP) $(DESTDIR)$(BINDIR)/$(APP)
	install -d $(DESTDIR)$(DATADIR)/applications
	install -m 0644 data/x11shift.desktop \
		$(DESTDIR)$(DATADIR)/applications/x11shift.desktop
	install -d $(DESTDIR)$(DATADIR)/icons/hicolor/scalable/apps
	install -m 0644 data/x11shift.svg \
		$(DESTDIR)$(DATADIR)/icons/hicolor/scalable/apps/x11shift.svg
	install -d $(DESTDIR)$(DATADIR)/metainfo
	install -m 0644 data/io.x11shift.X11Shift.metainfo.xml \
		$(DESTDIR)$(DATADIR)/metainfo/io.x11shift.X11Shift.metainfo.xml
	install -d $(DESTDIR)$(DATADIR)/man/man1
	install -m 0644 data/x11shift.1 $(DESTDIR)$(DATADIR)/man/man1/x11shift.1

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(APP)
	rm -f $(DESTDIR)$(DATADIR)/applications/x11shift.desktop
	rm -f $(DESTDIR)$(DATADIR)/icons/hicolor/scalable/apps/x11shift.svg
	rm -f $(DESTDIR)$(DATADIR)/metainfo/io.x11shift.X11Shift.metainfo.xml
	rm -f $(DESTDIR)$(DATADIR)/man/man1/x11shift.1

clean:
	rm -f $(APP) $(OBJECTS) $(RESOURCE_SOURCE) tests/test_schedule tests/test_gamma tests/test_resume
