#ifndef _MINIEAP_IF_IMPL_NPCAP_H
#define _MINIEAP_IF_IMPL_NPCAP_H

#include "if_impl.h"

/*
 * Npcap implementation: interoperates with the WinPcap/Npcap API.
 *
 * libpcap.dll is not shipped on Windows; Npcap installs wpcap.dll instead.
 * The functions used here only share the WinPcap API surface, so
 * if_impl/libpcap can already bind against wpcap when given the headers.
 *
 * A GUI process that has not been started as administrator can rely on
 * Npcap's "WinPcap API-compatible Mode", which allows non-admin packet
 * capture (and transmission) when installed with that option.
 */
IF_IMPL* npcap_new();

#endif
