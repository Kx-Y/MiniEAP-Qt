# ---------------------------------------------------------------------------
# qmake project for the MiniEAP Qt GUI.
#
#   qmake minieap-qt.pro && make        (or nmake on Windows)
#
# CMake is the recommended build for every platform (see ../CMakeLists.txt);
# this file exists for people whose workflow is qmake, and for `make gui`.
# It builds the same sources with the same flags the CMake build uses.
# ---------------------------------------------------------------------------

QT += core gui widgets network
CONFIG += c++17
TEMPLATE = app
TARGET = minieap-qt

# Windows GUI subsystem: no console window behind the window
win32: CONFIG += windows

# --------------------------------------------------------------------- engine
ENGINE_ROOT = ..

INCLUDEPATH += $$ENGINE_ROOT/include
INCLUDEPATH += $$ENGINE_ROOT/packet_plugin/rjv3/rjv3_hashes

DEFINES += MINIEAP_EMBEDDED
# The engine's failure paths call exit(); in a GUI that must unwind, not quit
DEFINES += exit=minieap_embedded_exit

win32 {
    DEFINES += _WIN32_WINNT=0x0A00 NOMINMAX UNICODE _UNICODE
    DEFINES += PKT_PLUGIN_RJV3_ENABLED PKT_PLUGIN_PRINTER_ENABLED
    DEFINES += IF_IMPL_NPCAP_ENABLED

    SOURCES += \
        $$ENGINE_ROOT/util/net_util_win32.c \
        $$ENGINE_ROOT/util/npcap_device_name.c \
        $$ENGINE_ROOT/if_impl/npcap/if_impl_npcap.c \
        $$ENGINE_ROOT/src/bridge/pcap_dyn.c

    LIBS += -liphlpapi -lws2_32 -lole32 -lshell32 -luuid -luserenv -lversion \
            -luxtheme -ldwmapi
} else {
    unix:!macx {
        DEFINES += PKT_PLUGIN_RJV3_ENABLED PKT_PLUGIN_PRINTER_ENABLED
        DEFINES += IF_IMPL_SOCKRAW_ENABLED ENABLE_ICONV
        LIBS += -liconv
        SOURCES += $$ENGINE_ROOT/if_impl/sockraw/if_impl_sockraw.c
    }
    macx {
        DEFINES += PKT_PLUGIN_RJV3_ENABLED PKT_PLUGIN_PRINTER_ENABLED
        DEFINES += IF_IMPL_BPF_ENABLED ENABLE_ICONV
        SOURCES += $$ENGINE_ROOT/if_impl/bpf/if_impl_bpf.c
    }
    SOURCES += $$ENGINE_ROOT/util/net_util.c
}

ENGINE_SOURCES = \
    $$ENGINE_ROOT/config.c \
    $$ENGINE_ROOT/eap_state_machine.c \
    $$ENGINE_ROOT/md5.c \
    $$ENGINE_ROOT/minieap.c \
    $$ENGINE_ROOT/packet_builder.c \
    $$ENGINE_ROOT/util/conf_parser.c \
    $$ENGINE_ROOT/util/gbconv.c \
    $$ENGINE_ROOT/util/linkedlist.c \
    $$ENGINE_ROOT/util/logging.c \
    $$ENGINE_ROOT/util/misc.c \
    $$ENGINE_ROOT/util/packet_util.c \
    $$ENGINE_ROOT/util/pid_lock.c \
    $$ENGINE_ROOT/util/proc_util.c \
    $$ENGINE_ROOT/util/runtime.c \
    $$ENGINE_ROOT/util/sched_alarm.c \
    $$ENGINE_ROOT/if_impl/if_impl.c \
    $$ENGINE_ROOT/packet_plugin/packet_plugin.c \
    $$ENGINE_ROOT/packet_plugin/printer/packet_plugin_printer.c

# The Ruijie v3 plugin and its hash implementations
RJV3_SOURCES = \
    $$ENGINE_ROOT/packet_plugin/rjv3/packet_plugin_rjv3.c \
    $$ENGINE_ROOT/packet_plugin/rjv3/packet_plugin_rjv3_keepalive.c \
    $$ENGINE_ROOT/packet_plugin/rjv3/packet_plugin_rjv3_priv.c \
    $$ENGINE_ROOT/packet_plugin/rjv3/packet_plugin_rjv3_prop.c \
    $$ENGINE_ROOT/packet_plugin/rjv3/rjv3_hashes/byte_order.c \
    $$ENGINE_ROOT/packet_plugin/rjv3/rjv3_hashes/checkV4.c \
    $$ENGINE_ROOT/packet_plugin/rjv3/rjv3_hashes/rjcrc16.c \
    $$ENGINE_ROOT/packet_plugin/rjv3/rjv3_hashes/rjencode.c \
    $$ENGINE_ROOT/packet_plugin/rjv3/rjv3_hashes/rjmd5.c \
    $$ENGINE_ROOT/packet_plugin/rjv3/rjv3_hashes/rjripemd128.c \
    $$ENGINE_ROOT/packet_plugin/rjv3/rjv3_hashes/rjsha1.c \
    $$ENGINE_ROOT/packet_plugin/rjv3/rjv3_hashes/rjtiger.c \
    $$ENGINE_ROOT/packet_plugin/rjv3/rjv3_hashes/rjtiger_sbox.c \
    $$ENGINE_ROOT/packet_plugin/rjv3/rjv3_hashes/rjwhirlpool.c \
    $$ENGINE_ROOT/packet_plugin/rjv3/rjv3_hashes/rjwhirlpool_sbox.c

SOURCES += $$ENGINE_SOURCES $$RJV3_SOURCES

# ---------------------------------------------------------------- bridge/GUI
SOURCES += \
    main.cpp \
    mainwindow.cpp \
    enginebridge.cpp \
    profile.cpp \
    profiledialog.cpp \
    netinterfaces.cpp \
    $$ENGINE_ROOT/src/bridge/bridge.c

HEADERS += \
    mainwindow.h \
    enginebridge.h \
    profile.h \
    profiledialog.h \
    netinterfaces.h \
    appversion.h

# Keep object files out of the source directories
OBJECTS_DIR = build/obj
MOC_DIR = build/moc
RCC_DIR = build/rcc
UI_DIR = build/ui
