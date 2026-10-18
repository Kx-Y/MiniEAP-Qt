/*
 * Windows implementation of the network information helpers.
 *
 * The POSIX version (util/net_util.c) reads getifaddrs(), /etc/resolv.conf
 * and the routing table through netlink/sysctl. The equivalents here are
 * GetAdaptersAddresses(), GetNetworkParams() and GetBestRoute2().
 *
 * Interface names are the ones the user picks in the GUI and passes as
 * --nic, i.e. what GetAdaptersAddresses() calls FriendlyName ("以太网",
 * "WLAN"); the adapter GUID is accepted as well.
 */
#include "net_util.h"
#include "logging.h"
#include "misc.h"
#include "minieap_common.h"
#include "platform.h" /* Sets _WIN32_WINNT before windows.h */

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <netioapi.h> /* if_nametoindex(), GetIpForwardTable2() */
#include <stdlib.h>
#include <string.h>

/*
 * GetAdaptersAddresses() returns WCHAR* for FriendlyName and Description on
 * Windows, so names are compared as wide strings.
 *
 * The engine's char* names are in the system ANSI code page (GBK on a Chinese
 * Windows), which is what the GUI hands over and what configs contain. CP_ACP
 * therefore has to be tried first: UTF-8 would mis-decode some GBK sequences.
 */
static int win32_wide_equals(const WCHAR* wide, const char* ansi) {
    WCHAR _buf[IFNAMSIZ];
    int _len;

    if (wide == NULL || ansi == NULL) {
        return FALSE;
    }
    _len = MultiByteToWideChar(CP_ACP, 0, ansi, -1, _buf, IFNAMSIZ);
    if (_len <= 0) {
        _len = MultiByteToWideChar(CP_UTF8, 0, ansi, -1, _buf, IFNAMSIZ);
        if (_len <= 0) {
            return FALSE;
        }
    }
    return _wcsicmp(wide, _buf) == 0;
}

static int win32_str_equals_ci(const char* a, const char* b) {
    return (a != NULL && b != NULL) ? (_stricmp(a, b) == 0) : FALSE;
}

static int g_wsa_ready = -1; /* -1 = unknown, 0 = failed, 1 = ready */

static RESULT win32_net_ready() {
    if (g_wsa_ready >= 0) {
        return g_wsa_ready ? SUCCESS : FAILURE;
    }
    {
        WSADATA _wsa;
        if (WSAStartup(MAKEWORD(2, 2), &_wsa) != 0) {
            PR_ERR("Winsock 初始化失败");
            g_wsa_ready = 0;
            return FAILURE;
        }
    }
    g_wsa_ready = 1;
    return SUCCESS;
}

/*
 * Locate an adapter by FriendlyName ("以太网"), Description, AdapterName (GUID)
 * or Npcap's \Device\NPF_{GUID} name.
 *
 * On success *out points to an independent copy of the matching adapter, owned
 * by the caller (free it with win32_free_adapter_copy()). Copying is required
 * because GetAdaptersAddresses() returns one block for the whole list, and the
 * matching adapter is usually not its head (Hyper-V's vEthernet adapter often
 * comes first). Returning the list head made every caller read the wrong
 * adapter - which is how frames were sent with vEthernet's source MAC.
 */
static void win32_copy_wide(wchar_t* dst, const wchar_t* src) {
    if (src == NULL) {
        dst[0] = L'\0';
        return;
    }
    wcsncpy(dst, src, IFNAMSIZ - 1);
    dst[IFNAMSIZ - 1] = L'\0';
}

static RESULT win32_find_adapter(const char* ifname, IP_ADAPTER_ADDRESSES** out) {
    IP_ADAPTER_ADDRESSES* _addrs = NULL;
    IP_ADAPTER_ADDRESSES* _curr;
    ULONG _buf_len = 16 * 1024;
    ULONG _ret;
    int _tries;

    *out = NULL;
    if (ifname == NULL || ifname[0] == 0) {
        return FAILURE;
    }
    if (IS_FAIL(win32_net_ready())) {
        return FAILURE;
    }

    for (_tries = 0; _tries < 3; ++_tries) {
        _addrs = (IP_ADAPTER_ADDRESSES*)malloc(_buf_len);
        if (_addrs == NULL) {
            return FAILURE;
        }
        _ret = GetAdaptersAddresses(AF_UNSPEC,
                                    GAA_FLAG_INCLUDE_PREFIX | GAA_FLAG_SKIP_ANYCAST |
                                        GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                    NULL, _addrs, &_buf_len);
        if (_ret == ERROR_SUCCESS) {
            break;
        }
        free(_addrs);
        _addrs = NULL;
        if (_ret != ERROR_BUFFER_OVERFLOW) {
            PR_WARN("无法枚举网络适配器 (错误码 %lu)", (unsigned long)_ret);
            return FAILURE;
        }
    }

    if (_addrs == NULL) {
        return FAILURE;
    }

    for (_curr = _addrs; _curr != NULL; _curr = _curr->Next) {
        int _match = FALSE;

        if (win32_wide_equals(_curr->FriendlyName, ifname)) {
            _match = TRUE;
        } else if (win32_str_equals_ci(_curr->AdapterName, ifname)) {
            _match = TRUE;
        } else if (win32_wide_equals(_curr->Description, ifname)) {
            _match = TRUE;
        }

        /* Npcap names devices as \Device\NPF_{GUID} */
        if (!_match && _curr->AdapterName != NULL) {
            char _npf_name[160];
            snprintf(_npf_name, sizeof(_npf_name), "\\Device\\NPF_%s", _curr->AdapterName);
            if (_stricmp(_npf_name, ifname) == 0) {
                _match = TRUE;
            }
        }

        if (_match) {
            /* Deep copy: the caller frees this, not the original block */
            IP_ADAPTER_ADDRESSES* _copy =
                (IP_ADAPTER_ADDRESSES*)calloc(1, sizeof(IP_ADAPTER_ADDRESSES));
            if (_copy == NULL) {
                free(_addrs);
                return FAILURE;
            }
            _copy->IfType = _curr->IfType;
            _copy->OperStatus = _curr->OperStatus;
            _copy->Luid = _curr->Luid;
            _copy->PhysicalAddressLength = _curr->PhysicalAddressLength;
            memmove(_copy->PhysicalAddress, _curr->PhysicalAddress,
                    sizeof(_copy->PhysicalAddress));
            if (_curr->AdapterName != NULL) {
                _copy->AdapterName = _strdup(_curr->AdapterName);
            }
            _copy->FriendlyName =
                _curr->FriendlyName != NULL ? _wcsdup(_curr->FriendlyName) : NULL;
            _copy->Description =
                _curr->Description != NULL ? _wcsdup(_curr->Description) : NULL;
            _copy->FirstUnicastAddress = _curr->FirstUnicastAddress;
            _copy->Next = NULL;

            free(_addrs);
            *out = _copy;
            return SUCCESS;
        }
    }

    free(_addrs);
    return FAILURE;
}

/* Release a copy produced by win32_find_adapter() */
static void win32_free_adapter_copy(IP_ADAPTER_ADDRESSES* copy) {
    if (copy == NULL) {
        return;
    }
    free(copy->AdapterName);
    free(copy->FriendlyName);
    free(copy->Description);
    free(copy);
}

RESULT obtain_iface_mac(const char* ifname, uint8_t* adr_buf) {
    IP_ADAPTER_ADDRESSES* _addrs = NULL;
    RESULT _ret = FAILURE;

    if (IS_FAIL(win32_find_adapter(ifname, &_addrs))) {
        PR_ERR("找不到网络适配器 %s", ifname ? ifname : "(null)");
        return FAILURE;
    }

    /* win32_find_adapter() returns a copy of the matching adapter */
    if (_addrs->PhysicalAddressLength >= 6) {
        memmove(adr_buf, _addrs->PhysicalAddress, 6);
        _ret = SUCCESS;
    } else {
        PR_ERR("网络适配器 %s 没有可用的 MAC 地址", ifname);
    }

    win32_free_adapter_copy(_addrs);
    return _ret;
}

RESULT obtain_iface_ip_mask(const char* ifname, LIST_ELEMENT** list) {
    IP_ADAPTER_ADDRESSES* _addrs = NULL;
    IP_ADAPTER_UNICAST_ADDRESS* _ua;

    if (IS_FAIL(win32_find_adapter(ifname, &_addrs))) {
        PR_ERR("找不到网络适配器 %s", ifname ? ifname : "(null)");
        return FAILURE;
    }

    for (_ua = _addrs->FirstUnicastAddress; _ua != NULL; _ua = _ua->Next) {
        IP_ADDR* _addr;
        int _prefix;

        if (_ua->Address.lpSockaddr == NULL) {
            continue;
        }
        _addr = (IP_ADDR*)malloc(sizeof(IP_ADDR));
        if (_addr == NULL) {
            continue;
        }
        memset(_addr, 0, sizeof(IP_ADDR));
        _addr->family = _ua->Address.lpSockaddr->sa_family;
        _prefix = (int)_ua->OnLinkPrefixLength;

        if (_addr->family == AF_INET) {
            struct sockaddr_in* _sin = (struct sockaddr_in*)_ua->Address.lpSockaddr;
            memmove(_addr->ip, &_sin->sin_addr, 4);
            if (_prefix <= 0 || _prefix > 32) {
                _prefix = 32;
            }
            if (_prefix > 0) {
                uint32_t _mask = htonl(0xFFFFFFFFu << (32 - _prefix));
                memmove(_addr->mask, &_mask, 4);
            }
        } else if (_addr->family == AF_INET6) {
            struct sockaddr_in6* _sin6 = (struct sockaddr_in6*)_ua->Address.lpSockaddr;
            memmove(_addr->ip, &_sin6->sin6_addr, 16);
            if (_prefix > 128) {
                _prefix = 128;
            }
            if (_prefix > 0) {
                _addr->mask[(_prefix - 1) / 8] = (uint8_t)(0xFF << (8 - (_prefix % 8)));
            }
        } else {
            free(_addr);
            continue;
        }

        insert_data(list, _addr);
    }

    win32_free_adapter_copy(_addrs);
    return SUCCESS;
}

void free_ip_list(LIST_ELEMENT** list) {
    list_destroy(list, TRUE);
}

/* Shared with the POSIX implementation: pick the first address of a family */
static int ip_addr_family_cmpfunc(void* family, void* ip_addr) {
    if (*(short*)family == ((IP_ADDR*)ip_addr)->family) {
        return 0;
    }
    return 1;
}

IP_ADDR* find_ip_with_family(LIST_ELEMENT* list, short family) {
    return (IP_ADDR*)lookup_data(list, &family, ip_addr_family_cmpfunc);
}

/*
 * GetNetworkParams() returns the DNS servers configured on the system, in
 * the same spirit as the resolv.conf list used on POSIX.
 */
RESULT obtain_dns_list(LIST_ELEMENT** list) {
    FIXED_INFO* _fi = NULL;
    IP_ADDR_STRING* _dns;
    ULONG _buf_len = sizeof(FIXED_INFO);

    if (IS_FAIL(win32_net_ready())) {
        return FAILURE;
    }

    _fi = (FIXED_INFO*)malloc(_buf_len);
    if (_fi == NULL) {
        return FAILURE;
    }

    {
        ULONG _ret = GetNetworkParams(_fi, &_buf_len);
        if (_ret == ERROR_BUFFER_OVERFLOW) {
            free(_fi);
            _fi = (FIXED_INFO*)malloc(_buf_len);
            if (_fi == NULL) {
                return FAILURE;
            }
            _ret = GetNetworkParams(_fi, &_buf_len);
        }
        if (_ret != ERROR_SUCCESS) {
            free(_fi);
            PR_WARN("无法获取 DNS 服务器列表 (错误码 %lu)", (unsigned long)_ret);
            return FAILURE;
        }
    }

    for (_dns = &_fi->DnsServerList; _dns != NULL; _dns = _dns->Next) {
        if (_dns->IpAddress.String[0] != 0) {
            insert_data(list, strdup(_dns->IpAddress.String));
        }
    }

    free(_fi);
    return SUCCESS;
}

void free_dns_list(LIST_ELEMENT** list) {
    list_destroy(list, TRUE);
}

/*
 * Find the IPv4 default gateway of an interface.
 *
 * GetBestRoute2() answers "which route would carry a packet to this
 * destination", so it cannot be pointed at a specific adapter's default
 * route. The route table is walked instead and the default route
 * (0.0.0.0/0) whose NextHop lies on the adapter's subnet is chosen.
 */
RESULT obtain_iface_ipv4_gateway(const char* ifname, uint8_t* buf) {
    IP_ADAPTER_ADDRESSES* _addrs = NULL;
    IP_ADAPTER_UNICAST_ADDRESS* _ua;
    MIB_IPFORWARD_TABLE2* _routes = NULL;
    ULONG _i;
    RESULT _ret = FAILURE;
    uint32_t _if_ip = 0;
    uint32_t _if_mask = 0;

    if (IS_FAIL(win32_find_adapter(ifname, &_addrs))) {
        return FAILURE;
    }

    for (_ua = _addrs->FirstUnicastAddress; _ua != NULL; _ua = _ua->Next) {
        if (_ua->Address.lpSockaddr != NULL && _ua->Address.lpSockaddr->sa_family == AF_INET) {
            int _prefix = (int)_ua->OnLinkPrefixLength;
            memmove(&_if_ip, &((struct sockaddr_in*)_ua->Address.lpSockaddr)->sin_addr, 4);
            if (_prefix <= 0 || _prefix > 32) {
                _prefix = 32;
            }
            _if_mask = htonl(0xFFFFFFFFu << (32 - _prefix));
            break;
        }
    }
    win32_free_adapter_copy(_addrs);

    if (_if_ip == 0) {
        return FAILURE;
    }

    if (GetIpForwardTable2(AF_INET, &_routes) != NO_ERROR || _routes == NULL) {
        return FAILURE;
    }

    for (_i = 0; _i < _routes->NumEntries; ++_i) {
        MIB_IPFORWARD_ROW2* _row = &_routes->Table[_i];
        uint32_t _dst, _next_hop;

        if (_row->DestinationPrefix.Prefix.si_family != AF_INET ||
            _row->NextHop.si_family != AF_INET ||
            _row->DestinationPrefix.PrefixLength != 0) {
            continue; /* Only IPv4 default routes */
        }

        memmove(&_dst, &_row->DestinationPrefix.Prefix.Ipv4.sin_addr, 4);
        memmove(&_next_hop, &_row->NextHop.Ipv4.sin_addr, 4);
        if (_dst != 0) {
            continue;
        }

        /* The next hop has to belong to this adapter's subnet */
        if ((_next_hop & _if_mask) == (_if_ip & _if_mask)) {
            memmove(buf, &_next_hop, 4);
            _ret = SUCCESS;
            break;
        }
    }

    FreeMibTable(_routes);
    return _ret;
}
