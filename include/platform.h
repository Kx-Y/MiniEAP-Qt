#ifndef _MINIEAP_PLATFORM_H
#define _MINIEAP_PLATFORM_H

/*
 * Portability layer for the small POSIX/Windows differences that show up all
 * over the engine. Include this from the C sources that need it.
 */

/* ------------------------------------------------------------------ Windows */
#ifdef _WIN32

/*
 * Winsock and the IP Helper API need to know the oldest Windows version we
 * target before windows.h/winsock2.h are expanded:
 *   _WIN32_WINNT_VISTA       MIB_IPFORWARD_TABLE2, GetIpForwardTable2
 *   _WIN32_WINNT_WIN10       OnLinkPrefixLength in GetAdaptersAddresses()
 *                             (also present since Vista with a smaller layout)
 * Windows 10 / Server 2016 is a reasonable floor for a desktop GUI.
 */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef WINVER
#define WINVER 0x0A00
#endif
/* Keep the winsock2.h contract explicit; windows.h without it is an error */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

/*
 * Interface name length.
 *
 * POSIX net/if.h defines IFNAMSIZ (16). MinGW's headers do not, and Windows
 * interface names are longer anyway ("以太网", "WLAN", adapter GUIDs), so the
 * engine's buffers use this generous size. IF_NAMESIZE follows the Windows
 * convention (IF_MAX_STRING_SIZE + 1).
 */
#ifndef IF_MAX_STRING_SIZE
#define IF_MAX_STRING_SIZE 256
#endif
#ifndef IFNAMSIZ
#define IFNAMSIZ IF_MAX_STRING_SIZE
#endif

#ifndef ETH_P_PAE
#define ETH_P_PAE 0x888e /* 802.1X EAP over Ethernet */
#endif

#include <stdio.h>

/* MinGW spells sprintf without the security relocation that MSVC warns about */
#ifndef MINIEAP_SPRINTF
#define MINIEAP_SPRINTF snprintf
#endif

/*
 * strndup() is POSIX, and MinGW's runtime does not provide it. The engine uses
 * it for every copied command line value, so supply a compatible version.
 */
#ifndef MINIEAP_HAVE_STRNDUP
#include <stdlib.h>
#include <string.h>
static __inline char* minieap_strndup(const char* s, size_t n) {
    size_t _len = 0;
    char* _out;
    if (s == NULL) {
        return NULL;
    }
    while (_len < n && s[_len] != '\0') {
        ++_len;
    }
    _out = (char*)malloc(_len + 1);
    if (_out == NULL) {
        return NULL;
    }
    memcpy(_out, s, _len);
    _out[_len] = '\0';
    return _out;
}
#define strndup minieap_strndup
#endif

#endif /* _WIN32 */

/* ------------------------------------------------------------------- POSIX */
#ifndef _WIN32

#include <net/if.h>

#ifndef ETH_P_PAE
#define ETH_P_PAE 0x888e
#endif

#ifndef MINIEAP_SPRINTF
#define MINIEAP_SPRINTF snprintf
#endif

#endif /* !_WIN32 */

#endif
