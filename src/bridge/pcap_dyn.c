/*
 * wpcap.dll loader.
 *
 * Resolves the pcap entry points used by the capture modules at runtime, so
 * the Windows build needs neither the Npcap SDK to compile nor the import
 * library to link. See include/pcap_dyn.h and include/pcap_dyn_bind.h.
 *
 * On POSIX libpcap is linked normally and this file compiles to nothing.
 */
#ifdef _WIN32

#include "pcap_dyn.h"
#include "pcap_dyn_bind.h"

#include <stdio.h>
#include <string.h>
#include <windows.h>

/* The table the modules' pcap_* macros point at */
PCAP_DYN_BINDINGS g_pcap;

static int g_initialised = 0;
static PCAP_DYN_RESULT g_result = PCAP_DYN_ERR_NOT_INSTALLED;
static HMODULE g_handle = NULL;
static char g_detail[256];

/*
 * GetProcAddress returns FARPROC, which is not a function pointer of any
 * specific signature. Converting through void* is the documented way to store
 * it, and every signature below matches the WinPcap API exactly.
 */
#define RESOLVE_REQUIRED(field, symbol)                                    \
    do {                                                                   \
        g_pcap.field = (void*)GetProcAddress(g_handle, symbol);            \
        if (g_pcap.field == NULL) {                                        \
            snprintf(g_detail, sizeof(g_detail),                           \
                     "wpcap.dll 缺少函数 %s", (symbol));                   \
            g_result = PCAP_DYN_ERR_INCOMPATIBLE;                          \
            return g_result;                                               \
        }                                                                  \
    } while (0)

#define RESOLVE_OPTIONAL(field, symbol)                                    \
    g_pcap.field = (void*)GetProcAddress(g_handle, symbol)

PCAP_DYN_RESULT pcap_dyn_init(void) {
    if (g_initialised) {
        return g_result;
    }
    g_initialised = 1;
    g_result = PCAP_DYN_ERR_NOT_INSTALLED;
    g_detail[0] = 0;

    /* Npcap installs wpcap.dll into System32, so the plain name suffices */
    g_handle = LoadLibraryA("wpcap.dll");
    if (g_handle == NULL) {
        snprintf(g_detail, sizeof(g_detail),
                 "无法加载 wpcap.dll，请先安装 Npcap");
        return g_result;
    }

    g_result = PCAP_DYN_OK;

    /* Required by the capture modules */
    RESOLVE_REQUIRED(open_live, "pcap_open_live");
    RESOLVE_REQUIRED(next_ex, "pcap_next_ex");
    RESOLVE_REQUIRED(sendpacket, "pcap_sendpacket");
    RESOLVE_REQUIRED(geterr, "pcap_geterr");
    RESOLVE_REQUIRED(datalink, "pcap_datalink");
    RESOLVE_REQUIRED(close, "pcap_close");
    RESOLVE_REQUIRED(compile, "pcap_compile");
    RESOLVE_REQUIRED(freecode, "pcap_freecode");
    RESOLVE_REQUIRED(setfilter, "pcap_setfilter");

    /* Optional: unused by our modules, handy for diagnostics */
    RESOLVE_OPTIONAL(setnonblock, "pcap_setnonblock");
    RESOLVE_OPTIONAL(stats, "pcap_stats");
    RESOLVE_OPTIONAL(breakloop, "pcap_breakloop");
    RESOLVE_OPTIONAL(loop, "pcap_loop");
    RESOLVE_OPTIONAL(dispatch, "pcap_dispatch");
    RESOLVE_OPTIONAL(lib_version, "pcap_lib_version");

    snprintf(g_detail, sizeof(g_detail), "%s",
             g_pcap.lib_version != NULL ? g_pcap.lib_version() : "Npcap");
    g_result = PCAP_DYN_OK;
    return g_result;
}

const char* pcap_dyn_status_text(void) {
    if (!g_initialised) {
        pcap_dyn_init();
    }
    return g_detail;
}

/*
 * Npcap reports "The system cannot find the path specified" or an access
 * denied message when a non-admin process has no capture right.
 */
int pcap_dyn_error_is_permission(const char* errbuf) {
    if (errbuf == NULL) {
        return 0;
    }
    if (strstr(errbuf, "access") != NULL || strstr(errbuf, "denied") != NULL ||
        strstr(errbuf, "拒绝访问") != NULL || strstr(errbuf, "权限") != NULL) {
        return 1;
    }
    return 0;
}

#endif /* _WIN32 */
