#include "netinterfaces.h"

#include <QRegularExpression>

#include <algorithm>

#ifdef Q_OS_WIN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#endif

#ifdef Q_OS_WIN
/* RAII wrapper for the heap buffer returned by GetAdaptersAddresses() */
namespace {
class WinsockScope {
public:
    WinsockScope() { m_ok = (WSAStartup(MAKEWORD(2, 2), &m_data) == 0); }
    ~WinsockScope()
    {
        if (m_ok) {
            WSACleanup();
        }
    }
    bool ok() const { return m_ok; }

private:
    WSADATA m_data {};
    bool m_ok = false;
};

IP_ADAPTER_ADDRESSES* getAdapterList()
{
    ULONG size = 16 * 1024;
    for (int attempt = 0; attempt < 3; ++attempt) {
        auto* list = static_cast<IP_ADAPTER_ADDRESSES*>(malloc(size));
        if (list == nullptr) {
            return nullptr;
        }
        const ULONG rc = GetAdaptersAddresses(
            AF_UNSPEC, GAA_FLAG_INCLUDE_PREFIX | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST,
            nullptr, list, &size);
        if (rc == ERROR_SUCCESS) {
            return list;
        }
        free(list);
        if (rc != ERROR_BUFFER_OVERFLOW) {
            return nullptr;
        }
    }
    return nullptr;
}

QString wideToQString(const WCHAR* text)
{
    return text != nullptr ? QString::fromWCharArray(text) : QString();
}

bool looksVirtual(const QString& description)
{
    static const QRegularExpression re(
        QStringLiteral("virtual|vmware|virtualbox|hyper-v|vbox|tap|tun|loopback|npcap|wireshark|"
                       "bluetooth|蓝牙|伪|虚拟"),
        QRegularExpression::CaseInsensitiveOption);
    return re.match(description).hasMatch();
}
} // namespace
#endif // Q_OS_WIN

QVector<InterfaceInfo> NetInterfaces::enumerate()
{
    QVector<InterfaceInfo> result;

#ifdef Q_OS_WIN
    WinsockScope winsock;
    if (!winsock.ok()) {
        return result;
    }
    IP_ADAPTER_ADDRESSES* list = getAdapterList();
    if (list == nullptr) {
        return result;
    }

    for (IP_ADAPTER_ADDRESSES* cur = list; cur != nullptr; cur = cur->Next) {
        if (cur->IfType == IF_TYPE_SOFTWARE_LOOPBACK) {
            continue; /* Cannot carry EAPOL */
        }

        InterfaceInfo info;
        info.name = wideToQString(cur->FriendlyName);
        info.description = wideToQString(cur->Description);
        if (info.name.isEmpty()) {
            continue;
        }
        info.isUp = (cur->OperStatus == IfOperStatusUp);
        info.isLoopback = false;

        const int macLen = cur->PhysicalAddressLength;
        if (macLen >= 6) {
            QStringList parts;
            for (int i = 0; i < 6; ++i) {
                parts << QStringLiteral("%1").arg(cur->PhysicalAddress[i], 2, 16,
                                                  QLatin1Char('0')).toUpper();
            }
            info.macAddress = parts.join(QLatin1Char(':'));
        }

        for (IP_ADAPTER_UNICAST_ADDRESS* ua = cur->FirstUnicastAddress; ua != nullptr;
             ua = ua->Next) {
            if (ua->Address.lpSockaddr == nullptr) {
                continue;
            }
            char buffer[64] = {0};
            const int family = ua->Address.lpSockaddr->sa_family;
            const void* addr = nullptr;
            if (family == AF_INET) {
                addr = &reinterpret_cast<sockaddr_in*>(ua->Address.lpSockaddr)->sin_addr;
            } else if (family == AF_INET6) {
                addr = &reinterpret_cast<sockaddr_in6*>(ua->Address.lpSockaddr)->sin6_addr;
            }
            if (addr != nullptr &&
                InetNtopA(family, const_cast<void*>(addr), buffer, sizeof(buffer)) != nullptr) {
                info.addresses << QString::fromLatin1(buffer);
            }
        }

        result << info;
    }
    free(list);

    /*
     * Prefer what a user actually plugs the campus network into: connected
     * physical Ethernet/Wi-Fi adapters first, then anything else connected,
     * then the rest. Virtual adapters (VPN clients, Hyper-V) go last.
     */
    std::stable_sort(result.begin(), result.end(), [](const InterfaceInfo& a, const InterfaceInfo& b) {
        auto rank = [](const InterfaceInfo& i) {
            /* 0: physical and up, 1: other connected, 2/3: virtual */
            if (looksVirtual(i.description)) {
                return i.isUp ? 2 : 3;
            }
            return i.isUp ? 0 : 1;
        };
        return rank(a) < rank(b);
    });
#else
    /*
     * POSIX: the engine takes the kernel interface name (eth0, wlan0, en0).
     * Reading it here needs getifaddrs(); kept behind the platform guard to
     * avoid pulling POSIX headers into the GUI build.
     */
#endif

    return result;
}

/*
 * Does the engine accept this interface name?
 *
 * The engine resolves friendly name, driver description and GUID (see
 * util/net_util_win32.c), so the GUI must accept the same set: otherwise a
 * profile that works would still show "interface not found". Case-insensitive
 * to match the engine's comparison.
 */
bool NetInterfaces::exists(const QString& name)
{
    const QString wanted = name.trimmed();
    if (wanted.isEmpty()) {
        return false;
    }
    const QVector<InterfaceInfo> all = enumerate();
    for (const InterfaceInfo& info : all) {
        if (info.name.compare(wanted, Qt::CaseInsensitive) == 0 ||
            info.description.compare(wanted, Qt::CaseInsensitive) == 0) {
            return true;
        }
    }
    return false;
}

QStringList NetInterfaces::captureModules()
{
    /*
     * Modules compiled into this binary. The names match IF_IMPL::name in
     * the engine.
     */
    QStringList modules;
#ifdef Q_OS_WIN
    modules << QStringLiteral("npcap");
#elif defined(Q_OS_MACOS)
    modules << QStringLiteral("bpf");
#else
    modules << QStringLiteral("sockraw");
#endif
    return modules;
}
