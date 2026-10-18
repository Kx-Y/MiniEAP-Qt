/*
 * MiniEAP Qt front end.
 *
 * The main window is created before any engine work, so a failure to start
 * the engine is always reported inside the GUI rather than as a stray console.
 */
#include "mainwindow.h"
#include "appversion.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QFile>
#include <QProcess>
#include <QStyleFactory>
#include <QTextStream>
#include <QTimer>

#ifdef Q_OS_WIN
#include <objbase.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <windows.h>
#endif

namespace {

/*
 * IsUserAnAdmin() lives in shell32 but is not declared by every MinGW header
 * set, and it is deprecated in the SDK. Ask the token directly instead.
 */
#ifdef Q_OS_WIN
bool isUserAdmin()
{
    BOOL isAdmin = FALSE;
    PSID adminGroup = NULL;
    SID_IDENTIFIER_AUTHORITY ntAuthority = SECURITY_NT_AUTHORITY;

    if (AllocateAndInitializeSid(&ntAuthority, 2, SECURITY_BUILTIN_DOMAIN_RID,
                                 DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &adminGroup)) {
        CheckTokenMembership(NULL, adminGroup, &isAdmin);
        FreeSid(adminGroup);
    }
    return isAdmin != FALSE;
}
#else
bool isUserAdmin()
{
    return false;
}
#endif

/*
 * Read "--connect-test-*" inputs from a UTF-8 file of "key=value" lines.
 *
 * Why a file: PowerShell 5.1 cannot pass non-ASCII arguments to a native
 * program (they arrive as "?"), and nested command lines mangle them further.
 * Reading the values here avoids the whole shell/code page problem, which
 * matters because adapter names are often Chinese ("以太网").
 *
 * Recognised keys: interface (or nic), username, password.
 */
bool readTestInputs(const QString& path, QString* iface, QString* user, QString* pass)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return false;
    }
    QTextStream stream(&file);
    stream.setEncoding(QStringConverter::Utf8);
    while (!stream.atEnd()) {
        const QString line = stream.readLine().trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#'))) {
            continue;
        }
        const int eq = line.indexOf(QLatin1Char('='));
        const QString key = (eq > 0 ? line.left(eq) : line).trimmed();
        const QString value = (eq > 0 ? line.mid(eq + 1) : QString()).trimmed();
        if (key == QLatin1String("interface") || key == QLatin1String("nic")) {
            *iface = value;
        } else if (key == QLatin1String("username") || key == QLatin1String("user")) {
            *user = value;
        } else if (key == QLatin1String("password") || key == QLatin1String("pass")) {
            *pass = value;
        }
    }
    return !iface->isEmpty();
}

#ifdef Q_OS_WIN
/*
 * Re-launch this executable elevated with the same arguments (UAC prompt),
 * used by --connect-test because the Npcap driver on this machine is
 * installed in "Admin-only Mode".
 *
 * Returns true when a new elevated process was started; the caller must exit
 * immediately in that case.
 */
bool relaunchElevated(int argc, char* argv[])
{
    if (isUserAdmin()) {
        return false;
    }

    QString params;
    for (int i = 1; i < argc; ++i) {
        QString arg = QString::fromLocal8Bit(argv[i]);
        if (arg.contains(QLatin1Char(' ')) || arg.contains(QLatin1Char('"'))) {
            arg = QLatin1Char('"') + arg.replace(QLatin1Char('"'), QString()) + QLatin1Char('"');
        }
        params += arg;
        if (i + 1 < argc) {
            params += QLatin1Char(' ');
        }
    }

    const std::wstring exe = QCoreApplication::applicationFilePath().toStdWString();
    const std::wstring args = params.toStdWString();
    SHELLEXECUTEINFOW sei;
    ZeroMemory(&sei, sizeof(sei));
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas";
    sei.lpFile = exe.c_str();
    sei.lpParameters = args.c_str();
    sei.nShow = SW_SHOWNORMAL;

    if (ShellExecuteExW(&sei)) {
        if (sei.hProcess != NULL) {
            CloseHandle(sei.hProcess);
        }
        return true;
    }
    return false;
}
#else
bool relaunchElevated(int, char**)
{
    return false;
}
#endif

/* Admin check used by the status line, mirrors what Npcap requires */
bool isRunningAsAdmin()
{
    return isUserAdmin();
}

} // namespace

int main(int argc, char* argv[])
{
#ifdef Q_OS_WIN
    /*
     * GetAdaptersAddresses() (the interface picker) wants a COM capable
     * apartment. Qt 6 uses STA on Windows; initialise it explicitly so this
     * does not silently depend on Qt's default.
     */
    const HRESULT comResult = OleInitialize(nullptr);
    (void)comResult;
#endif

    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("MiniEAP"));
    QApplication::setApplicationDisplayName(QStringLiteral("MiniEAP"));
    QApplication::setOrganizationName(QStringLiteral("MiniEAP"));
    QApplication::setApplicationVersion(QStringLiteral(MINIEAP_GUI_VERSION_STR));

    /*
     * Fusion renders the same way on every platform and follows the palette,
     * which keeps the colours of the log view readable in light and dark mode.
     */
    if (QStyleFactory::keys().contains(QStringLiteral("Fusion"))) {
        QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    }

    QCommandLineParser parser;
    parser.setApplicationDescription(QObject::tr("MiniEAP：可扩展的 802.1x 认证客户端"));
    parser.addHelpOption();
    parser.addVersionOption();

    /*
     * --screenshot <file>: render the window to a PNG and exit. Useful for
     * documentation, CI checks of the layout, and debugging UI changes
     * without a display capture.
     */
    QCommandLineOption screenshotOption(
        QStringLiteral("screenshot"),
        QObject::tr("将主窗口渲染为 PNG 图片后退出（用于文档与测试）"),
        QStringLiteral("file"));
    parser.addOption(screenshotOption);

    QCommandLineOption profileShotOption(
        QStringLiteral("profile-screenshot"),
        QObject::tr("将配置编辑对话框渲染为 PNG 图片后退出（用于文档与测试）"),
        QStringLiteral("file"));
    parser.addOption(profileShotOption);

    /*
     * --connect-test: start a real engine run, wait, render the window and
     * exit. Driven by a UTF-8 key=value file so non-ASCII adapter names and
     * credentials reach the engine intact (see readTestInputs()).
     */
    QCommandLineOption connectTestOption(
        QStringLiteral("connect-test"),
        QObject::tr("用指定网卡做一次真实的连接测试，结束后保存截图并退出"),
        QStringLiteral("interface"));
    parser.addOption(connectTestOption);

    QCommandLineOption connectTestInputOption(
        QStringLiteral("connect-test-file"),
        QObject::tr("连接测试的输入文件（UTF-8，格式为 interface=/username=/password=）"),
        QStringLiteral("file"));
    parser.addOption(connectTestInputOption);

    QCommandLineOption connectTestUserOption(
        QStringLiteral("connect-test-user"),
        QObject::tr("连接测试使用的用户名"),
        QStringLiteral("username"));
    parser.addOption(connectTestUserOption);

    QCommandLineOption connectTestPassOption(
        QStringLiteral("connect-test-password"),
        QObject::tr("连接测试使用的密码"),
        QStringLiteral("password"));
    parser.addOption(connectTestPassOption);

    QCommandLineOption connectTestShotOption(
        QStringLiteral("connect-test-screenshot"),
        QObject::tr("连接测试结束时保存的 PNG 路径"),
        QStringLiteral("file"));
    parser.addOption(connectTestShotOption);

    QCommandLineOption testDurationOption(
        QStringLiteral("connect-test-duration"),
        QObject::tr("连接测试运行时长（毫秒，默认 12000）"),
        QStringLiteral("ms"));
    parser.addOption(testDurationOption);

    QCommandLineOption elevateOption(
        QStringLiteral("elevate"),
        QObject::tr("以管理员身份重新启动（Windows，用于需要管理员权限的抓包）"));
    parser.addOption(elevateOption);

    QCommandLineOption noElevateOption(
        QStringLiteral("no-elevate"),
        QObject::tr("连接测试时不要自动请求管理员权限"));
    parser.addOption(noElevateOption);

    parser.process(app);

    /*
     * Elevation is opt-in only (--elevate).
     *
     * It must NOT happen implicitly: on machines where UAC prompts are
     * disabled by policy, ShellExecuteEx(runas) can block or return without
     * spawning anything, leaving the caller hung with no window and no log -
     * which is exactly what made --connect-test (and therefore the GUI's
     * connect button in test mode) appear to fail with lost parameters.
     * Ordinary runs start the engine in-process and let Npcap report a
     * permission problem, which the UI shows.
     */
    if (parser.isSet(elevateOption) && !isRunningAsAdmin()) {
        if (relaunchElevated(argc, argv)) {
            return 0;
        }
    }

    MainWindow window;
    window.show();

    if (parser.isSet(connectTestOption)) {
        /*
         * `interface` is also a macro from windows.h; the name itself is fine
         * here because we only touch Profile members.
         */
        QString iface = parser.value(connectTestOption);
        QString user = parser.isSet(connectTestUserOption) ? parser.value(connectTestUserOption)
                                                           : QString();
        QString pass = parser.isSet(connectTestPassOption) ? parser.value(connectTestPassOption)
                                                           : QString();
        if (parser.isSet(connectTestInputOption)) {
            /* The file takes precedence: it carries exact UTF-8 bytes */
            readTestInputs(parser.value(connectTestInputOption), &iface, &user, &pass);
        }
        if (user.isEmpty()) {
            user = QStringLiteral("test-user");
        }
        if (pass.isEmpty()) {
            pass = QStringLiteral("test-password");
        }

        Profile profile;
        profile.name = QStringLiteral("集成测试");
        profile.username = user;
        profile.password = pass;
        /*
         * Profile::interface clashes with the `interface` macro from
         * combaseapi.h (`#define interface struct`), so suspend it for these
         * lines only.
         */
#ifdef interface
#pragma push_macro("interface")
#undef interface
#define MINIEAP_RESTORE_INTERFACE_MACRO
#endif
        profile.interface = iface;
#ifdef MINIEAP_RESTORE_INTERFACE_MACRO
#pragma pop_macro("interface")
#undef MINIEAP_RESTORE_INTERFACE_MACRO
#endif
        /* extra=<args> in the input file lets a test try engine options */
        QString extra;
        QString fakeDns2;
        QString fakeSerial;
        int stageTimeout = 5;
        int maxRetries = 3;
        if (parser.isSet(connectTestInputOption)) {
            QFile f(parser.value(connectTestInputOption));
            if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
                QTextStream ts(&f);
                ts.setEncoding(QStringConverter::Utf8);
                while (!ts.atEnd()) {
                    const QString line = ts.readLine().trimmed();
                    const int eq = line.indexOf(QLatin1Char('='));
                    if (eq <= 0) {
                        continue;
                    }
                    const QString key = line.left(eq).trimmed();
                    const QString value = line.mid(eq + 1).trimmed();
                    if (key == QLatin1String("extra")) {
                        extra = value;
                    } else if (key == QLatin1String("stage-timeout")) {
                        stageTimeout = value.toInt();
                    } else if (key == QLatin1String("max-retries")) {
                        maxRetries = value.toInt();
                    } else if (key == QLatin1String("fake-dns2")) {
                        fakeDns2 = value;
                    } else if (key == QLatin1String("fake-serial")) {
                        fakeSerial = value;
                    }
                }
            }
        }
        profile.extraArguments = extra;
        profile.stageTimeout = stageTimeout;
        profile.maxRetries = maxRetries;
        if (!fakeDns2.isEmpty()) {
            profile.fakeDns2 = fakeDns2;
        }
        if (!fakeSerial.isEmpty()) {
            profile.fakeSerial = fakeSerial;
        }

        /*
         * Defaults match a normal GUI profile; the input file can override
         * stage-timeout / max-retries to reproduce another platform's working
         * configuration (e.g. stage-timeout=0, max-retries=0 keeps retrying
         * forever instead of giving up).
         */
        profile.maxFailures = 3;
        profile.waitAfterFail = 5;

        QTimer::singleShot(500, &window, [&window, profile]() {
            window.startWithProfile(profile);
        });
        const int duration = parser.isSet(testDurationOption)
                                 ? parser.value(testDurationOption).toInt()
                                 : 12000;
        QTimer::singleShot(duration, &window,
                           [&window, file = parser.value(connectTestShotOption)]() {
                               if (!file.isEmpty()) {
                                   window.grab().save(file, "PNG");
                               }
                               QApplication::quit();
                           });
    } else if (parser.isSet(screenshotOption)) {
        const QString path = parser.value(screenshotOption);
        QTimer::singleShot(600, &window, [&window, path]() {
            window.grab().save(path, "PNG");
            QApplication::quit();
        });
    } else if (parser.isSet(profileShotOption)) {
        const QString path = parser.value(profileShotOption);
        QTimer::singleShot(600, &window, [&window, path]() {
            auto* dialog = window.createProfileDialogForTesting();
            dialog->show();
            QTimer::singleShot(400, [dialog, path]() {
                dialog->grab().save(path, "PNG");
                dialog->close();
                QApplication::quit();
            });
        });
    }

    const int result = QApplication::exec();

    /*
     * Persist profiles and join the engine's worker thread before Qt tears
     * down the application object.
     */
    window.shutdown();
    return result;
}
