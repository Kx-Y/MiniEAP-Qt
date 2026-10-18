#ifndef _MINIEAP_PCAP_DYN_BIND_H
#define _MINIEAP_PCAP_DYN_BIND_H

/*
 * Points the pcap_* names at the entry points resolved from wpcap.dll.
 *
 * Include this AFTER the pcap declarations of the translation unit (i.e. after
 * <pcap.h> / win32_pcap_compat.h and after pcap_dyn.h), otherwise the
 * prototypes themselves are rewritten into calls to the pointer table.
 *
 * Definitions live in src/bridge/pcap_dyn.c.
 */

#ifdef _WIN32

#include <stdint.h>
#include "win32_pcap_compat.h"

/* One entry point: the resolved address, or NULL before pcap_dyn_init() */
typedef struct pcap_dyn_bindings {
    pcap_t*      (*open_live)(const char*, int, int, int, char*);
    int          (*next_ex)(pcap_t*, struct pcap_pkthdr**, const unsigned char**);
    int          (*sendpacket)(pcap_t*, const unsigned char*, int);
    const char*  (*geterr)(pcap_t*);
    int          (*setnonblock)(pcap_t*, int, char*);
    int          (*datalink)(pcap_t*);
    int          (*stats)(pcap_t*, struct pcap_stat*);
    void         (*breakloop)(pcap_t*);
    void         (*close)(pcap_t*);
    int          (*compile)(pcap_t*, struct bpf_program*, const char*, int, uint32_t);
    void         (*freecode)(struct bpf_program*);
    int          (*setfilter)(pcap_t*, struct bpf_program*);
    int          (*loop)(pcap_t*, int, pcap_handler, unsigned char*);
    int          (*dispatch)(pcap_t*, int, pcap_handler, unsigned char*);
    const char*  (*lib_version)(void);
} PCAP_DYN_BINDINGS;

/* Filled by pcap_dyn_init() */
extern PCAP_DYN_BINDINGS g_pcap;

#define pcap_open_live   g_pcap.open_live
#define pcap_next_ex     g_pcap.next_ex
#define pcap_sendpacket  g_pcap.sendpacket
#define pcap_geterr      g_pcap.geterr
#define pcap_setnonblock g_pcap.setnonblock
#define pcap_datalink    g_pcap.datalink
#define pcap_stats       g_pcap.stats
#define pcap_breakloop   g_pcap.breakloop
#define pcap_close       g_pcap.close
#define pcap_compile     g_pcap.compile
#define pcap_freecode    g_pcap.freecode
#define pcap_setfilter   g_pcap.setfilter
#define pcap_loop        g_pcap.loop
#define pcap_dispatch    g_pcap.dispatch
#define pcap_lib_version g_pcap.lib_version

#endif /* _WIN32 */

#endif /* _MINIEAP_PCAP_DYN_BIND_H */
