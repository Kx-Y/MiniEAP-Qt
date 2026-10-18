#ifndef MINIEAP_GUI_ENGINEBRIDGE_H
#define MINIEAP_GUI_ENGINEBRIDGE_H

#include <QObject>
#include <QString>
#include <QStringList>

/*
 * Qt wrapper around the C bridge (src/bridge/bridge.c).
 *
 * The engine runs on the bridge's worker thread. Its callbacks fire there, so
 * this object republishes them as Qt signals: Qt queues them onto the GUI
 * thread automatically when the connection is between threads.
 *
 * Typical lifecycle:
 *
 *     bridge.start(args);
 *     ...
 *     bridge.stop();                    // request stop, non blocking
 *     ... engineFinished() arrives later
 *
 * Only one run at a time; start() refuses while a run is in progress.
 */
class EngineBridge : public QObject {
    Q_OBJECT
public:
    explicit EngineBridge(QObject* parent = nullptr);
    ~EngineBridge() override;

    /* Why the engine stopped, mirroring ENGINE_EXIT_REASON in minieap_runtime.h */
    enum ExitReason {
        ExitOk = 0,
        ExitAuthFailed,
        ExitLoggedOff,
        ExitStateTimeout,
        ExitTransportError
    };
    Q_ENUM(ExitReason)

    enum LogLevel {
        LogNone = 0,
        LogInfo,
        LogWarning,
        LogError,
        LogDebug
    };
    Q_ENUM(LogLevel)

    /* Start a run with the given argument list (without argv[0]) */
    bool start(const QStringList& arguments);
    /* Ask the running engine to stop; the result arrives via finished() */
    void stop();
    /* TRUE while a run is queued or in progress */
    bool isRunning() const;

    /* Block until the current run has fully cleaned up (used when quitting) */
    void shutdown();

signals:
    /* One line of engine output. Emitted on the GUI thread. */
    void logLine(int level, const QString& text);
    /*
     * The run ended. `stopRequested` is true when the user asked for it, so
     * the UI can avoid showing an error for a deliberate stop.
     */
    void finished(int reason, bool stopRequested);

private:
    /* Called from the bridge thread; only emits signals */
    void deliverLog(int level, const QString& line);
    void deliverFinished(int reason, bool stopRequested);

    /* Friends of the C ABI: plain functions that forward to the instance */
    static void logThunk(int tag, const char* line, int len, void* user);
    static void exitThunk(int reason, int exitCode, int stoppedByHost, void* user);

    bool m_running = false;
};

#endif
