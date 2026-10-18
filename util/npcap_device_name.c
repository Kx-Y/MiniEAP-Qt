/*
 * Translate a user-facing interface name into the device name that Npcap's
 * pcap_open_live() expects.
 *
 * The engine addresses interfaces by their system name ("以太网", "WLAN"),
 * which is what users see in Windows settings and what configs store. Npcap
 * does not accept that name: it only understands its own device path,
 * "\\Device\\NPF_{GUID}", where the GUID is the adapter's AdapterName from
 * GetAdaptersAddresses(). Passing the friendly name fails with "Error
 * opening adapter ... (123)" (verified against Npcap 1.82).
 *
 * Accepted inputs, in order:
 *   - an already valid device path (returned unchanged)
 *   - the friendly name shown in Windows settings
 *   - the adapter GUID, with or without braces
 *   - the adapter description
 *
 * Everything involving the comparison goes through UTF-16 because the names
 * from GetAdaptersAddresses() are wide strings.
 */
#ifdef _WIN32

#include "platform.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <stdlib.h>
#include <string.h>

/*
 * Convert the name the engine gives us into UTF-16 for comparison with the
 * wide strings from GetAdaptersAddresses().
 *
 * The engine's char* strings are in the system ANSI code page (GBK on a
 * Chinese Windows): that is what GUI->engine argv conversion now produces,
 * and what the adapter lookups in net_util_win32.c expect. CP_ACP must come
 * first; trying UTF-8 first would silently mis-decode a GBK byte sequence
 * whose length happens to look like valid UTF-8.
 */
static void to_wide(const char* text, WCHAR* out, int out_count)
{
    if (text == NULL || out == NULL || out_count <= 0) {
        return;
    }
    out[0] = L'\0';
    if (MultiByteToWideChar(CP_ACP, 0, text, -1, out, out_count) > 0) {
        return;
    }
    /* Fall back to UTF-8 for sources that already convert (e.g. tests) */
    if (MultiByteToWideChar(CP_UTF8, 0, text, -1, out, out_count) <= 0) {
        out[0] = L'\0';
    }
}

/*
 * Fill `out` with the Npcap device name for `ifname`.
 * Returns TRUE on success; on failure `out` is untouched.
 */
int minieap_win32_npcap_device_name(const char* ifname, char* out, int out_len)
{
    IP_ADAPTER_ADDRESSES* _list = NULL;
    IP_ADAPTER_ADDRESSES* _curr;
    ULONG _size = 16 * 1024;
    ULONG _rc = 0;
    int _tries;
    int _found = 0;
    WCHAR _want[IFNAMSIZ];

    if (ifname == NULL || ifname[0] == '\0' || out == NULL || out_len <= 0) {
        return 0;
    }

    /* Already a device path: nothing to translate */
    if (strncmp(ifname, "\\Device\\NPF_", 12) == 0) {
        return 0;
    }

    for (_tries = 0; _tries < 3; ++_tries) {
        _list = (IP_ADAPTER_ADDRESSES*)malloc(_size);
        if (_list == NULL) {
            return 0;
        }
        _rc = GetAdaptersAddresses(AF_UNSPEC,
                                   GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST,
                                   NULL, _list, &_size);
        if (_rc == ERROR_SUCCESS) {
            break;
        }
        free(_list);
        _list = NULL;
        if (_rc != ERROR_BUFFER_OVERFLOW) {
            return 0;
        }
    }
    if (_list == NULL) {
        return 0;
    }

    to_wide(ifname, _want, IFNAMSIZ);

    for (_curr = _list; _curr != NULL; _curr = _curr->Next) {
        int _match = 0;

        if (_curr->AdapterName == NULL) {
            continue;
        }

        /*
         * GUID, with or without the surrounding braces. AdapterName is a
         * narrow PCHAR, so it is compared with strcmp; the wide _want buffer
         * only applies to the two wide name fields below.
         */
        if (strcmp(ifname, _curr->AdapterName) == 0) {
            _match = 1;
        }

        /* Friendly name ("以太网") */
        if (!_match && _curr->FriendlyName != NULL && _want[0] != L'\0' &&
            _wcsicmp(_want, _curr->FriendlyName) == 0) {
            _match = 1;
        }

        /* Driver description ("Realtek PCIe GbE Family Controller") */
        if (!_match && _curr->Description != NULL && _want[0] != L'\0' &&
            _wcsicmp(_want, _curr->Description) == 0) {
            _match = 1;
        }

        if (_match) {
            snprintf(out, (size_t)out_len, "\\Device\\NPF_%s", _curr->AdapterName);
            _found = 1;
            break;
        }
    }

    free(_list);
    return _found;
}

#endif /* _WIN32 */
