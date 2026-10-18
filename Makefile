#
# MiniEAP
#
# Target layout:
#   make                 console client (the classic single binary)
#   make gui             Qt GUI application (minieap-qt)
#   make install         install both (GUI only when built)
#   make clean
#
# For the GUI, CMake is the recommended path on every platform:
#   cmake -B build && cmake --build build
#
# This file keeps the original module-selection mechanism from config.mk for
# the console client, and adds the GUI without changing the way the engine is
# built. Modules whose name starts with "gui_" are handled here, not by
# config.mk.
#

include config.mk

#### Populate [C|LD]FLAGS ####
COMMON_CFLAGS := $(CUSTOM_CFLAGS) $(CFLAGS) -Wall -Wpedantic -D_GNU_SOURCE
COMMON_LDFLAGS := $(CUSTOM_LDFLAGS) $(LDFLAGS)
LIBS += $(CUSTOM_LIBS)

# You are not cross-compiling for macOS on Linux, I guess?
ifeq ($(shell uname -s),Linux)
COMMON_LDFLAGS += -T minieap_init_func.lds
endif

ifeq ($(ENABLE_ICONV),true)
COMMON_CFLAGS += -DENABLE_ICONV
ifeq ($(LIBICONV_STANDALONE),true)
COMMON_LDFLAGS += -liconv
endif
endif

ifeq ($(ENABLE_GBCONV),true)
COMMON_CFLAGS += -DENABLE_GBCONV
endif

ifeq ($(ENABLE_DEBUG),true)
COMMON_CFLAGS += -DDEBUG
endif

ifeq ($(STATIC_BUILD),true)
COMMON_LDFLAGS += -static
COMMON_LDFLAGS := $(filter-out -l%,$(COMMON_LDFLAGS))
endif

#### Common bits ####
COMMON_C_INCLUDES := include
COMMON_MODULES := \
	util \
	main \
	if_impl_manager \
	packet_plugin_manager

BUILD_MODULES := $(PLUGIN_MODULES) $(COMMON_MODULES)

minieap: $(BUILD_MODULES)
	$(CC) -o minieap \
        $(COMMON_LDFLAGS) \
        $(foreach objs,$(addsuffix _LDFLAGS,$(BUILD_MODULES)),$($(objs))) \
        $(foreach objs,$(addsuffix _PRIV_OBJS,$(BUILD_MODULES)),$($(objs))) \
        $(LIBS)

#
# The new files that are part of the engine on POSIX:
#   util/runtime.c     logging hook + shutdown flag shared with the GUI
#   util/proc_util.c   --dhcp-script runner
#   util/net_util_win32.c and src/bridge/* are Windows only and excluded by
#   the module wildcards (util/minieap.mk uses *.c, so runtime.c and
#   proc_util.c are picked up automatically).
#

.PHONY: clean
clean:
	rm -f minieap minieap.service minieap.1.gz

define my-dir
$(dir $(lastword $(MAKEFILE_LIST)))
endef

define all-c-files-under
$(subst $(LOCAL_PATH)/,,$(wildcard $(LOCAL_PATH)/$(if $(1),/,)*.c))
endef

APPEND := $(shell pwd)/append.mk
MK_LIST := $(shell find . -name minieap.mk)
include $(MK_LIST)

#### Install ####

DESTDIR ?=
PREFIX ?= /usr/local
BINDIR ?= /sbin
SYSCONFDIR ?= /etc
SYSTEMDDIR ?= $(SYSCONFDIR)/systemd
DATADIR ?= $(PREFIX)/share

minieap.%: minieap.%.in
	sed "s|:TARGET:|$(PREFIX)$(BINDIR)|g;s|:SYSCONFDIR:|$(SYSCONFDIR)|g" $< > $@

minieap.1.gz: minieap.1
	gzip -k $<

.PHONY: install
install: minieap minieap.1.gz minieap.service
	install -d $(DESTDIR)$(PREFIX)$(BINDIR)/
	install -m 755 minieap $(DESTDIR)$(PREFIX)$(BINDIR)/
	install -d $(DESTDIR)$(SYSCONFDIR)/
	install -m 644 minieap.conf $(DESTDIR)$(SYSCONFDIR)/
	install -d $(DESTDIR)$(PREFIX)/share/man/man1/
	install -m 644 minieap.1.gz $(DESTDIR)$(PREFIX)/share/man/man1/
	install -d $(DESTDIR)$(SYSTEMDDIR)/system/
	install -m 644 minieap.service $(DESTDIR)$(SYSTEMDDIR)/system/
	-systemctl enable minieap

.PHONY: uninstall
uninstall:
	rm -f $(DESTDIR)$(PREFIX)$(BINDIR)/minieap
	rm -f $(DESTDIR)$(PREFIX)/share/man/man1/minieap.1.gz
	-systemctl disable minieap
	rm -f $(DESTDIR)$(SYSTEMDDIR)/system/minieap.service

# ============================================================================
# Qt GUI
#
# The GUI links the same engine sources as the console client, plus the bridge
# and the Qt front end. Both are compiled with -DMINIEAP_EMBEDDED (so minieap.c
# provides no main()) and with exit() redirected to the bridge, which unwinds a
# failing engine instead of terminating the application.
#
# Requires a Qt 6 installation:
#   make gui                               # uses qmake on PATH
#   make gui QMAKE=/opt/Qt/6.10.0/gcc_64/bin/qmake
#
# The actual Qt build is delegated to qmake so moc/translation are handled by
# the toolchain; see gui/minieap-qt.pro.
# ============================================================================
QMAKE ?= qmake6
LLVM_MINGW := $(shell $(QMAKE) -query QMAKE_SPEC 2>/dev/null)

.PHONY: gui
gui:
	@command -v $(QMAKE) >/dev/null 2>&1 || { \
		echo "error: '$(QMAKE)' not found. Pass QMAKE=/path/to/qmake6"; exit 1; }
	$(QMAKE) gui/minieap-qt.pro -o Makefile.gui
	$(MAKE) -f Makefile.gui
	@echo "built gui/minieap-qt (or the platform equivalent)"

.PHONY: gui-clean
gui-clean:
	rm -f Makefile.gui .qmake.stash
	rm -rf gui/build

.PHONY: clean-gui
clean-gui: gui-clean
