#ifndef _MINIEAP_PCAP_DYN_H
#define _MINIEAP_PCAP_DYN_H

/*
 * Runtime binding to wpcap.dll.
 *
 * Building against Npcap needs its SDK, a separate download. The shipping
 * Windows build resolves wpcap.dll at runtime instead: the program starts
 * (and the GUI can explain what is missing) even without Npcap, and no import
 * library is needed to compile.
 *
 * Export table check: every function used here is exported by name from the
 * Npcap wpcap.dll (119 named exports), so LoadLibrary + GetProcAddress is
 * enough.
 *
 * Layout of this trio:
 *   win32_pcap_compat.h  types and prototypes (no macros)
 *   pcap_dyn.h           this file: loader API (no macros)
 *   pcap_dyn_bind.h      macros that redirect pcap_* to the loader
 *
 * A translation unit includes the compat header first and then binds, see
 * if_impl/npcap/if_impl_npcap.c.
 */

#ifdef _WIN32

#include "win32_pcap_compat.h"

typedef enum _pcap_dyn_result {
    PCAP_DYN_OK = 0,
    PCAP_DYN_ERR_NOT_INSTALLED,   /* wpcap.dll could not be loaded */
    PCAP_DYN_ERR_INCOMPATIBLE,    /* loaded, but functions are missing */
    PCAP_DYN_ERR_NOT_PERMITTED    /* loaded, but no capture right */
} PCAP_DYN_RESULT;

/*
 * Load wpcap.dll (once) and resolve the exported entry points. Safe to call
 * repeatedly; returns PCAP_DYN_OK when the bound pcap_* macros are usable.
 */
PCAP_DYN_RESULT pcap_dyn_init(void);

/* Human readable state for the GUI / log */
const char* pcap_dyn_status_text(void);

/* Does this pcap error message look like a missing capture privilege? */
int pcap_dyn_error_is_permission(const char* errbuf);

#endif /* _WIN32 */

#endif /* _MINIEAP_PCAP_DYN_H */
