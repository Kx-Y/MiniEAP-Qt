# ============================================================================
# MiniEAP - portable 802.1x client with an optional Qt GUI
#
# Three ways to build, all producing the same programs:
#
#   cmake -B build && cmake --build build      recommended, all platforms
#   make                                       console client only (POSIX)
#   scripts/build-windows.ps1                  Windows, no CMake needed
#
# CMake builds the engine twice from one source list:
#   - minieap (console): faithful to the original single-binary client,
#     including exit() on failure and main() in minieap.c
#   - minieap-qt (GUI): engine compiled with MINIEAP_EMBEDDED and exit()
#     redirected to the bridge, so a failing engine unwinds into the GUI
# ============================================================================
cmake_minimum_required(VERSION 3.20)

project(MiniEAP
        VERSION 0.93
        DESCRIPTION "Extensible 802.1x client with a Qt GUI"
        LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Executables and their runtime dependencies (DLLs) end up side by side
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

include(GNUInstallDirs)

# ---------------------------------------------------------------------------
# Options: which modules end up inside the engine
# ---------------------------------------------------------------------------
option(MINIEAP_BUILD_GUI "Build the Qt GUI application" ON)
option(MINIEAP_BUILD_CONSOLE "Build the classic console client" ON)

option(MINIEAP_WITH_PRINTER "Build the sample packet printer plugin" ON)
option(MINIEAP_WITH_RJV3 "Build the Ruijie v3 packet plugin" ON)
option(MINIEAP_DEBUG_LOGGING "Compile in debug log lines" OFF)

if(WIN32)
    option(MINIEAP_WITH_NPCAP "Build the Npcap capture module" ON)
    option(MINIEAP_HAVE_PCAP_H
           "Npcap SDK headers are installed: use <pcap.h> instead of the bundled declarations" OFF)
    set(MINIEAP_HAVE_PCAP_H ${MINIEAP_HAVE_PCAP_H})
    set(MINIEAP_WITH_SOCKRAW OFF)
elseif(APPLE)
    option(MINIEAP_WITH_BPF "Build the BPF capture module" ON)
    option(MINIEAP_WITH_LIBPCAP "Build the libpcap capture module" OFF)
    set(MINIEAP_WITH_SOCKRAW OFF)
else()
    option(MINIEAP_WITH_SOCKRAW "Build the raw-socket capture module" ON)
    option(MINIEAP_WITH_LIBPCAP "Build the libpcap capture module" OFF)
endif()

# POSIX converts GBK server messages with iconv. Windows uses the Win32 code
# page API instead, so ENABLE_ICONV is never set there.
if(NOT WIN32)
    option(MINIEAP_ENABLE_ICONV "Use iconv to decode GBK server messages" ON)
else()
    set(MINIEAP_ENABLE_ICONV OFF)
endif()

# ---------------------------------------------------------------------------
# Engine sources
# ---------------------------------------------------------------------------
set(MINIEAP_ENGINE_SOURCES
    config.c
    eap_state_machine.c
    md5.c
    minieap.c
    packet_builder.c

    util/conf_parser.c
    util/gbconv.c
    util/linkedlist.c
    util/logging.c
    util/misc.c
    util/packet_util.c
    util/pid_lock.c
    util/proc_util.c
    util/runtime.c
    util/sched_alarm.c

    if_impl/if_impl.c
    packet_plugin/packet_plugin.c
)

if(WIN32)
    # npcap_device_name.c maps friendly adapter names to Npcap device paths
    list(APPEND MINIEAP_ENGINE_SOURCES
        util/net_util_win32.c util/npcap_device_name.c src/bridge/pcap_dyn.c)
else()
    list(APPEND MINIEAP_ENGINE_SOURCES util/net_util.c)
endif()

set(MINIEAP_ENGINE_DEFS "")
set(MINIEAP_ENGINE_LIBS "")

# ---- packet plugins ----
if(MINIEAP_WITH_RJV3)
    list(APPEND MINIEAP_ENGINE_SOURCES
        packet_plugin/rjv3/packet_plugin_rjv3.c
        packet_plugin/rjv3/packet_plugin_rjv3_keepalive.c
        packet_plugin/rjv3/packet_plugin_rjv3_priv.c
        packet_plugin/rjv3/packet_plugin_rjv3_prop.c
        packet_plugin/rjv3/rjv3_hashes/byte_order.c
        packet_plugin/rjv3/rjv3_hashes/checkV4.c
        packet_plugin/rjv3/rjv3_hashes/rjcrc16.c
        packet_plugin/rjv3/rjv3_hashes/rjencode.c
        packet_plugin/rjv3/rjv3_hashes/rjmd5.c
        packet_plugin/rjv3/rjv3_hashes/rjripemd128.c
        packet_plugin/rjv3/rjv3_hashes/rjsha1.c
        packet_plugin/rjv3/rjv3_hashes/rjtiger.c
        packet_plugin/rjv3/rjv3_hashes/rjtiger_sbox.c
        packet_plugin/rjv3/rjv3_hashes/rjwhirlpool.c
        packet_plugin/rjv3/rjv3_hashes/rjwhirlpool_sbox.c
    )
    list(APPEND MINIEAP_ENGINE_DEFS PKT_PLUGIN_RJV3_ENABLED)
endif()

if(MINIEAP_WITH_PRINTER)
    list(APPEND MINIEAP_ENGINE_SOURCES packet_plugin/printer/packet_plugin_printer.c)
    list(APPEND MINIEAP_ENGINE_DEFS PKT_PLUGIN_PRINTER_ENABLED)
endif()

# ---- interface (capture) modules ----
if(WIN32)
    if(MINIEAP_WITH_NPCAP)
        list(APPEND MINIEAP_ENGINE_SOURCES if_impl/npcap/if_impl_npcap.c)
        list(APPEND MINIEAP_ENGINE_DEFS IF_IMPL_NPCAP_ENABLED)
        if(MINIEAP_HAVE_PCAP_H)
            list(APPEND MINIEAP_ENGINE_DEFS MINIEAP_HAVE_PCAP_H)
        endif()
    endif()
elseif(APPLE)
    if(MINIEAP_WITH_BPF)
        list(APPEND MINIEAP_ENGINE_SOURCES if_impl/bpf/if_impl_bpf.c)
        list(APPEND MINIEAP_ENGINE_DEFS IF_IMPL_BPF_ENABLED)
    endif()
else()
    if(MINIEAP_WITH_SOCKRAW)
        list(APPEND MINIEAP_ENGINE_SOURCES if_impl/sockraw/if_impl_sockraw.c)
        list(APPEND MINIEAP_ENGINE_DEFS IF_IMPL_SOCKRAW_ENABLED)
    endif()
    if(MINIEAP_WITH_LIBPCAP)
        list(APPEND MINIEAP_ENGINE_SOURCES if_impl/libpcap/if_impl_libpcap.c)
        list(APPEND MINIEAP_ENGINE_DEFS IF_IMPL_LIBPCAP_ENABLED)
        find_library(PCAP_LIBRARY NAMES pcap REQUIRED)
        list(APPEND MINIEAP_ENGINE_LIBS ${PCAP_LIBRARY})
    endif()
endif()

if(MINIEAP_DEBUG_LOGGING)
    list(APPEND MINIEAP_ENGINE_DEFS DEBUG)
endif()

if(MINIEAP_ENABLE_ICONV)
    list(APPEND MINIEAP_ENGINE_DEFS ENABLE_ICONV)
    find_package(Iconv REQUIRED)
    list(APPEND MINIEAP_ENGINE_LIBS Iconv::Iconv)
    if(Iconv_LIBRARIES AND NOT Iconv_IS_BUILT_IN)
        list(APPEND MINIEAP_ENGINE_DEFS LIBICONV_STANDALONE)
    endif()
endif()

# Locate the C++ runtime that accompanies the compiler. Qt for Windows is
# built against libc++ (llvm-mingw), so libc++.dll / libunwind.dll must sit
# next to the executable; windeployqt does not copy them.
if(WIN32 AND NOT MSVC)
    get_filename_component(MINIEAP_CXX_RUNTIME_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
    # llvm-mingw keeps the DLLs next to the compiler drivers
    message(STATUS "C++ runtime directory: ${MINIEAP_CXX_RUNTIME_DIR}")
endif()

# ---------------------------------------------------------------------------
# Engine library
#
# The engine is compiled twice, with two differences:
#
#   console variant  - main() comes from minieap.c, exit() is the real one
#   GUI variant      - MINIEAP_EMBEDDED (no main()) and exit() redirected to
#                      minieap_embedded_exit() in src/bridge/bridge.c
#
# The redirect must NOT leak into the console build, so it cannot be a
# directory-wide COMPILE_DEFINITIONS. set_source_files_properties() would leak
# because that property is global to the directory and applies to every target
# that compiles the file - which made the console client fail to link with
# "undefined reference to minieap_embedded_exit" on Linux/macOS.
#
# Instead the GUI variant is compiled from a private copy of the sources in a
# generated directory, so its per-file definitions cannot escape.
# ---------------------------------------------------------------------------
function(minieap_configure_engine target)
    target_include_directories(${target}
        PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/packet_plugin/rjv3/rjv3_hashes
    )
    target_compile_definitions(${target} PRIVATE ${MINIEAP_ENGINE_DEFS})
    target_link_libraries(${target} PUBLIC ${MINIEAP_ENGINE_LIBS})

    if(WIN32)
        # iphlpapi: adapter/DNS/route lookups. ws2_32: sockets.
        target_link_libraries(${target} PUBLIC iphlpapi ws2_32)
    elseif(NOT APPLE)
        target_link_libraries(${target} PUBLIC ${CMAKE_DL_LIBS})
    endif()

    if(MSVC)
        target_compile_options(${target} PRIVATE /W3)
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -Wno-format)
    endif()
endfunction()

# Console variant: the original single-binary program, one compile of minieap.c
if(MINIEAP_BUILD_CONSOLE)
    add_executable(minieap ${MINIEAP_ENGINE_SOURCES})
    minieap_configure_engine(minieap)

    # The section-based module registry needs the linker script that places
    # the marker symbols; GNU ld only (the layout is ELF specific).
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT APPLE
       AND CMAKE_SIZEOF_VOID_P EQUAL 8)
        target_link_options(minieap PRIVATE
            -T ${CMAKE_CURRENT_SOURCE_DIR}/minieap_init_func.lds)
    endif()

    install(TARGETS minieap RUNTIME DESTINATION ${CMAKE_INSTALL_SBINDIR})
endif()

# ---------------------------------------------------------------------------
# Qt GUI
# ---------------------------------------------------------------------------
if(MINIEAP_BUILD_GUI)
    find_package(Qt6 6.2 REQUIRED COMPONENTS Widgets Network)

    set(CMAKE_AUTOMOC ON)
    set(CMAKE_AUTORCC ON)

    # Private copy of the engine sources for the embedded variant
    set(MINIEAP_GUI_SRC_DIR ${CMAKE_BINARY_DIR}/gui_engine_src)
    set(MINIEAP_EMBEDDED_SOURCES "")
    foreach(_src ${MINIEAP_ENGINE_SOURCES})
        set(_dst ${MINIEAP_GUI_SRC_DIR}/${_src})
        configure_file(${CMAKE_CURRENT_SOURCE_DIR}/${_src} ${_dst} COPYONLY)
        list(APPEND MINIEAP_EMBEDDED_SOURCES ${_dst})
    endforeach()
    set_source_files_properties(${MINIEAP_EMBEDDED_SOURCES} PROPERTIES
        COMPILE_DEFINITIONS "exit=minieap_embedded_exit")

    add_library(minieap_engine_gui STATIC
        ${MINIEAP_EMBEDDED_SOURCES}
        ${CMAKE_CURRENT_SOURCE_DIR}/src/bridge/bridge.c)
    minieap_configure_engine(minieap_engine_gui)

    # The copies live in a flat mirror of the tree; plugin sources include
    # headers from their own directory, which for the originals resolves via
    # the source file's location. Point at the copied directories explicitly.
    target_include_directories(minieap_engine_gui PUBLIC
        ${MINIEAP_GUI_SRC_DIR}
        ${MINIEAP_GUI_SRC_DIR}/packet_plugin/rjv3
        ${MINIEAP_GUI_SRC_DIR}/packet_plugin/rjv3/rjv3_hashes>
    )
    target_include_directories(minieap_engine_gui PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/packet_plugin/rjv3
    )

    # Embedded mode: no main() from minieap.c
    target_compile_definitions(minieap_engine_gui PRIVATE MINIEAP_EMBEDDED)

    set(MINIEAP_GUI_SOURCES
        gui/main.cpp
        gui/mainwindow.cpp
        gui/mainwindow.h
        gui/enginebridge.cpp
        gui/enginebridge.h
        gui/profile.cpp
        gui/profile.h
        gui/profiledialog.cpp
        gui/profiledialog.h
        gui/netinterfaces.cpp
        gui/netinterfaces.h
        gui/appversion.h
    )

    add_executable(minieap-qt ${MINIEAP_GUI_SOURCES})
    target_link_libraries(minieap-qt PRIVATE minieap_engine_gui Qt6::Widgets Qt6::Network)
    target_compile_definitions(minieap-qt PRIVATE
        MINIEAP_EMBEDDED
        MINIEAP_GUI_VERSION="${PROJECT_VERSION}"
    )

    if(WIN32)
        set_target_properties(minieap-qt PROPERTIES WIN32_EXECUTABLE ON)
        # The GUI links the C engine objects which were compiled with the
        # exit() redirect; the bridge (src/bridge/bridge.c) provides it.
    elseif(APPLE)
        set_target_properties(minieap-qt PROPERTIES MACOSX_BUNDLE ON)
    endif()

    install(TARGETS minieap-qt
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
            BUNDLE  DESTINATION .)

    # Copy the Qt runtime next to the executable, so the build tree and the
    # installed tree are both runnable without setting PATH.
    if(WIN32)
        add_custom_command(TARGET minieap-qt POST_BUILD
            COMMAND Qt6::windeployqt
                    --no-translations --no-system-d3d-compiler --no-opengl-sw
                    "$<TARGET_FILE:minieap-qt>"
            COMMENT "Deploying Qt runtime next to minieap-qt.exe"
            VERBATIM)

        # Qt for Windows is built against libc++, and windeployqt does not
        # copy that runtime: without libc++.dll / libunwind.dll the executable
        # exits immediately with no message. Copy whatever the compiler ships
        # next to the binary.
        foreach(_cxx_runtime libc++.dll libunwind.dll libwinpthread-1.dll)
            if(EXISTS "${MINIEAP_CXX_RUNTIME_DIR}/${_cxx_runtime}")
                add_custom_command(TARGET minieap-qt POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                            "${MINIEAP_CXX_RUNTIME_DIR}/${_cxx_runtime}"
                            "$<TARGET_FILE_DIR:minieap-qt>"
                    COMMENT "Copying ${_cxx_runtime}"
                    VERBATIM)
            endif()
        endforeach()
    endif()
endif()

# ---------------------------------------------------------------------------
# Console extras: config, man page, systemd unit (POSIX installs)
# ---------------------------------------------------------------------------
if(UNIX AND MINIEAP_BUILD_CONSOLE)
    install(FILES minieap.conf DESTINATION ${CMAKE_INSTALL_SYSCONFDIR})
    install(FILES minieap.1 DESTINATION ${CMAKE_INSTALL_MANDIR}/man1)
    # minieap.service.in uses :TARGET: and :SYSCONFDIR: placeholders, the
    # same ones the Makefile substitutes with sed
    file(READ ${CMAKE_CURRENT_SOURCE_DIR}/minieap.service.in _service_template)
    string(REPLACE ":TARGET:" "${CMAKE_INSTALL_FULL_SBINDIR}"
           _service "${_service_template}")
    string(REPLACE ":SYSCONFDIR:" "${CMAKE_INSTALL_FULL_SYSCONFDIR}"
           _service "${_service}")
    file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/minieap.service "${_service}")
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/minieap.service
            DESTINATION ${CMAKE_INSTALL_SYSCONFDIR}/systemd/system)
endif()
