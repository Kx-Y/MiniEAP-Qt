#include "profile.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>

/* ---------------------------------------------------------------- Profile */

bool Profile::isValid(QString* error) const
{
    if (username.trimmed().isEmpty()) {
        if (error) {
            *error = QObject::tr("用户名不能为空");
        }
        return false;
    }
    if (password.isEmpty()) {
        if (error) {
            *error = QObject::tr("密码不能为空");
        }
        return false;
    }
    if (interface.trimmed().isEmpty()) {
        if (error) {
            *error = QObject::tr("请选择网络接口");
        }
        return false;
    }
    return true;
}

QStringList Profile::toArguments() const
{
    QStringList args;

    args << QStringLiteral("-u") << username
         << QStringLiteral("-p") << password
         << QStringLiteral("-n") << interface;

    /* Restarting after a forced logout is the default, -x turns it off */
    if (!autoReauth) {
        args << QStringLiteral("--no-auto-reauth");
    }

    args << QStringLiteral("-t") << QString::number(stageTimeout)
         << QStringLiteral("-r") << QString::number(waitAfterFail)
         << QStringLiteral("-l") << QString::number(maxFailures)
         << QStringLiteral("--max-retries") << QString::number(maxRetries)
         << QStringLiteral("-j") << QString::number(authRound);

    if (!ifImpl.isEmpty()) {
        args << QStringLiteral("--if-impl") << ifImpl;
    }

    /*
     * The GUI runs the engine inside a desktop application: keep it in the
     * foreground so logs and state transitions stay visible, and avoid the
     * system PID file location (/var/run is not writable for a normal user).
     */
    args << QStringLiteral("-b") << QStringLiteral("0");
    args << QStringLiteral("--pid-file") << QStringLiteral("none");

    const bool rjv3 = modules.contains(QStringLiteral("rjv3"));
    if (rjv3) {
        args << QStringLiteral("-e") << QString::number(heartbeat)
             << QStringLiteral("-d") << QString::number(dhcpType);
        if (!dhcpScript.isEmpty()) {
            args << QStringLiteral("-c") << dhcpScript;
        }
        if (!service.isEmpty()) {
            args << QStringLiteral("--service") << service;
        }
        if (!versionStr.isEmpty()) {
            args << QStringLiteral("--version-str") << versionStr;
        }
        if (!fakeDns1.isEmpty()) {
            args << QStringLiteral("--fake-dns1") << fakeDns1;
        }
        if (!fakeDns2.isEmpty()) {
            args << QStringLiteral("--fake-dns2") << fakeDns2;
        }
        if (!fakeSerial.isEmpty()) {
            args << QStringLiteral("--fake-serial") << fakeSerial;
        }
    }

    for (const QString& module : modules) {
        args << QStringLiteral("--module") << module;
    }

    /* Split on whitespace: quoted strings are kept whole */
    if (!extraArguments.trimmed().isEmpty()) {
        args += QProcess::splitCommand(extraArguments.trimmed());
    }

    return args;
}

QString Profile::summary() const
{
    if (username.isEmpty() && interface.isEmpty()) {
        return QObject::tr("尚未配置");
    }
    return QStringLiteral("%1 @ %2").arg(username.isEmpty() ? QObject::tr("(无用户名)") : username,
                                         interface.isEmpty() ? QObject::tr("(未选接口)") : interface);
}

QJsonObject Profile::toJson() const
{
    QJsonObject obj;
    obj[QStringLiteral("name")] = name;
    obj[QStringLiteral("username")] = username;
    /*
     * Stored in the user's configuration directory with owner-only access,
     * which is also what Npcap/802.1x tools elsewhere do. Documented in the
     * README: this is not encryption.
     */
    obj[QStringLiteral("password")] = password;
    obj[QStringLiteral("interface")] = interface;
    obj[QStringLiteral("ifImpl")] = ifImpl;
    obj[QStringLiteral("modules")] = QJsonArray::fromStringList(modules);
    obj[QStringLiteral("stageTimeout")] = stageTimeout;
    obj[QStringLiteral("waitAfterFail")] = waitAfterFail;
    obj[QStringLiteral("maxFailures")] = maxFailures;
    obj[QStringLiteral("maxRetries")] = maxRetries;
    obj[QStringLiteral("autoReauth")] = autoReauth;
    obj[QStringLiteral("authRound")] = authRound;
    obj[QStringLiteral("heartbeat")] = heartbeat;
    obj[QStringLiteral("dhcpType")] = dhcpType;
    obj[QStringLiteral("dhcpScript")] = dhcpScript;
    obj[QStringLiteral("service")] = service;
    obj[QStringLiteral("versionStr")] = versionStr;
    obj[QStringLiteral("fakeDns1")] = fakeDns1;
    obj[QStringLiteral("fakeDns2")] = fakeDns2;
    obj[QStringLiteral("fakeSerial")] = fakeSerial;
    obj[QStringLiteral("extraArguments")] = extraArguments;
    return obj;
}

Profile Profile::fromJson(const QJsonObject& obj)
{
    Profile p;
    p.name = obj.value(QStringLiteral("name")).toString(p.name);
    p.username = obj.value(QStringLiteral("username")).toString();
    p.password = obj.value(QStringLiteral("password")).toString();
    p.interface = obj.value(QStringLiteral("interface")).toString();
    p.ifImpl = obj.value(QStringLiteral("ifImpl")).toString();

    const QJsonArray modules = obj.value(QStringLiteral("modules")).toArray();
    p.modules.clear();
    for (const QJsonValue& value : modules) {
        p.modules << value.toString();
    }
    if (p.modules.isEmpty()) {
        p.modules << QStringLiteral("rjv3");
    }

    p.stageTimeout = obj.value(QStringLiteral("stageTimeout")).toInt(p.stageTimeout);
    p.waitAfterFail = obj.value(QStringLiteral("waitAfterFail")).toInt(p.waitAfterFail);
    p.maxFailures = obj.value(QStringLiteral("maxFailures")).toInt(p.maxFailures);
    p.maxRetries = obj.value(QStringLiteral("maxRetries")).toInt(p.maxRetries);
    p.autoReauth = obj.value(QStringLiteral("autoReauth")).toBool(p.autoReauth);
    p.authRound = obj.value(QStringLiteral("authRound")).toInt(p.authRound);
    p.heartbeat = obj.value(QStringLiteral("heartbeat")).toInt(p.heartbeat);
    p.dhcpType = obj.value(QStringLiteral("dhcpType")).toInt(p.dhcpType);
    p.dhcpScript = obj.value(QStringLiteral("dhcpScript")).toString();
    p.service = obj.value(QStringLiteral("service")).toString();
    p.versionStr = obj.value(QStringLiteral("versionStr")).toString();
    p.fakeDns1 = obj.value(QStringLiteral("fakeDns1")).toString();
    p.fakeDns2 = obj.value(QStringLiteral("fakeDns2")).toString();
    p.fakeSerial = obj.value(QStringLiteral("fakeSerial")).toString();
    p.extraArguments = obj.value(QStringLiteral("extraArguments")).toString();
    return p;
}

/* ----------------------------------------------------------- ProfileStore */

QString ProfileStore::filePath()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    return dir + QStringLiteral("/profiles.json");
}

void ProfileStore::load()
{
    m_profiles.clear();
    m_current = 0;

    QFile file(filePath());
    if (file.open(QIODevice::ReadOnly)) {
        const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
        if (doc.isObject()) {
            const QJsonObject root = doc.object();
            const QJsonArray profiles = root.value(QStringLiteral("profiles")).toArray();
            for (const QJsonValue& value : profiles) {
                m_profiles << Profile::fromJson(value.toObject());
            }
            m_current = root.value(QStringLiteral("current")).toInt(0);
        }
    } else if (file.error() != QFile::NoError && file.exists()) {
        /* A broken file is reported by the caller through save() failures */
        m_profiles.clear();
    }

    if (m_profiles.isEmpty()) {
        m_profiles << Profile();
    }
    if (m_current < 0 || m_current >= m_profiles.size()) {
        m_current = 0;
    }
}

bool ProfileStore::save() const
{
    QJsonArray profiles;
    for (const Profile& profile : m_profiles) {
        profiles.append(profile.toJson());
    }

    QJsonObject root;
    root[QStringLiteral("version")] = 1;
    root[QStringLiteral("current")] = m_current;
    root[QStringLiteral("profiles")] = profiles;

    const QString path = filePath();
    QDir().mkpath(QFileInfo(path).absolutePath());

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));

    /*
     * The file holds the password: restrict it to the current user on POSIX.
     * Windows inherits the ACL of the user's profile directory.
     */
#ifndef Q_OS_WIN
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
#endif
    file.close();
    return true;
}
