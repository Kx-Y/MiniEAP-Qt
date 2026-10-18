#ifndef MINIEAP_GUI_NETINTERFACES_H
#define MINIEAP_GUI_NETINTERFACES_H

#include <QString>
#include <QStringList>
#include <QVector>

struct InterfaceInfo {
    /* Name to pass as --nic: the friendly name shown in Windows settings */
    QString name;
    QString description;
    QString macAddress;
    QStringList addresses;
    bool isUp = false;
    bool isLoopback = false;
};

/*
 * Enumerate the network adapters the user can authenticate on.
 *
 * This mirrors what the engine's --nic accepts: on Windows the friendly name
 * ("以太网", "WLAN"), which is what interface_exists() verifies through the IP
 * Helper API. Adapters are returned roughly in preference order (connected
 * Ethernet/Wi-Fi first, virtual adapters last).
 */
class NetInterfaces {
public:
    static QVector<InterfaceInfo> enumerate();
    /* Does an adapter with this exact name exist? */
    static bool exists(const QString& name);
    /* Interface module names the engine was built with (for --if-impl) */
    static QStringList captureModules();
};

#endif
