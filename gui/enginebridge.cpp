#include "enginebridge.h"

#include "bridge.h"
#include "minieap_runtime.h"

#include <QByteArray>
#include <vector>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

/*
 * Decode one engine log line.
 *
 * The engine formats its messages as ordinary "char" literals, so on POSIX
 * they are UTF-8; on Windows the literals follow the compiler's execution
 * character set (UTF-8 for the llvm-mingw build) while strings coming from
 * the system (error texts, server fields) can be GBK.
 *
 * Strategy, cheapest first:
 *   1. pure ASCII -> Latin-1 (identical bytes, no conversion cost)
 *   2. valid UTF-8 (the normal case in both builds)
 *   3. the system ANSI code page, which is GBK on a Chinese Windows
 *   4. the byte-preserving Latin-1 view, so nothing is silently dropped
 *
 * Interface modules avoid passing driver buffers through (pcap's error buffer
 * is not guaranteed to be text), so this only has to cope with engine text.
 */
static QString decodeEngineLine(const char* line, int len)
{
    if (line == nullptr || len <= 0) {
        return QString();
    }
    const QByteArray raw(line, len);

    bool ascii = true;
    for (char c : raw) {
        if (static_cast<unsigned char>(c) >= 0x80) {
            ascii = false;
            break;
        }
    }
    if (ascii) {
        return QString::fromLatin1(raw);
    }

    const QString utf8Text = QString::fromUtf8(raw.constData(), raw.size());
    if (!utf8Text.contains(QChar::ReplacementCharacter)) {
        return utf8Text;
    }

    const QString ansiText = QString::fromLocal8Bit(raw);
    if (!ansiText.contains(QChar::ReplacementCharacter)) {
        return ansiText;
    }
    return QString::fromLatin1(raw);
}

EngineBridge::EngineBridge(QObject* parent)
    : QObject(parent)
{
    /*
     * Log lines and the exit callback are emitted from the bridge thread.
     * Connecting them (as signals) guarantees delivery on the GUI thread.
     */
}

EngineBridge::~EngineBridge()
{
    /*
     * The worker thread must not outlive the object it calls back into.
     * shutdown() waits for the current run to finish and stops the thread.
     */
    minieap_bridge_shutdown();
}

void EngineBridge::logThunk(int tag, const char* line, int len, void* user)
{
    auto* self = static_cast<EngineBridge*>(user);
    if (self != nullptr) {
        self->deliverLog(tag, decodeEngineLine(line, len));
    }
}

void EngineBridge::exitThunk(int reason, int exitCode, int stoppedByHost, void* user)
{
    Q_UNUSED(exitCode)
    auto* self = static_cast<EngineBridge*>(user);
    if (self != nullptr) {
        self->deliverFinished(reason, stoppedByHost != 0);
    }
}

/*
 * Encode one QString for the engine's argv.
 *
 * The engine's command line parser uses plain char* strings, and every name
 * it compares against (adapter names come from the Win32 ANSI APIs) is in
 * the system ANSI code page - GBK (CP936) on a Chinese Windows install. So the
 * conversion must happen here, with the very same code page:
 *
 *   Windows: WideCharToMultiByte(CP_ACP)  -> bytes the engine can compare
 *   POSIX:   UTF-8                        -> what the C locale and libc expect
 *
 * Using QString::toLocal8Bit() is not equivalent: Qt's "local 8 bit" follows
 * the C locale (nowdays UTF-8), which produced UTF-8 bytes that the engine
 * then compared against GBK strings - the cause of "interface not found"
 * with non-ASCII adapter names.
 */
static QByteArray encodeForEngine(const QString& text)
{
#ifdef Q_OS_WIN
    const int size = WideCharToMultiByte(CP_ACP, 0,
                                         reinterpret_cast<const wchar_t*>(text.utf16()),
                                         text.size(), nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return text.toUtf8();
    }
    QByteArray out(size, Qt::Uninitialized);
    WideCharToMultiByte(CP_ACP, 0,
                        reinterpret_cast<const wchar_t*>(text.utf16()),
                        text.size(), out.data(), size, nullptr, nullptr);
    return out;
#else
    return text.toUtf8();
#endif
}

bool EngineBridge::start(const QStringList& arguments)
{
    if (m_running || minieap_bridge_is_running()) {
        return false;
    }

    /* argv for the engine: a program name first, as main() would see */
    QList<QByteArray> encoded;
    encoded.reserve(arguments.size() + 1);
    encoded << QByteArrayLiteral("minieap");
    for (const QString& arg : arguments) {
        encoded << encodeForEngine(arg);
    }

    std::vector<char*> argv;
    argv.reserve(encoded.size() + 1);
    for (QByteArray& arg : encoded) {
        argv.push_back(arg.data());
    }
    argv.push_back(nullptr);

    if (minieap_bridge_prepare(static_cast<int>(encoded.size()), argv.data()) != MINIEAP_BRIDGE_OK) {
        return false;
    }

    if (minieap_bridge_start(&EngineBridge::logThunk, &EngineBridge::exitThunk, this)
        != MINIEAP_BRIDGE_OK) {
        return false;
    }

    m_running = true;
    return true;
}

void EngineBridge::stop()
{
    if (!m_running) {
        return;
    }
    /*
     * The engine stops when its capture loop notices the flag. If nothing is
     * capturing yet (still initializing), the flag is honoured once it gets
     * there; the finished() signal always arrives.
     */
    minieap_bridge_request_stop();
}

bool EngineBridge::isRunning() const
{
    return m_running;
}

void EngineBridge::shutdown()
{
    if (m_running) {
        minieap_bridge_request_stop();
    }
    minieap_bridge_wait_previous();
    m_running = false;
}

void EngineBridge::deliverLog(int level, const QString& line)
{
    emit logLine(level, line);
}

void EngineBridge::deliverFinished(int reason, bool stopRequested)
{
    m_running = false;
    emit finished(reason, stopRequested);
}
