#ifndef MINIEAP_GUI_MAINWINDOW_H
#define MINIEAP_GUI_MAINWINDOW_H

#include <QMainWindow>
#include <QStringList>

#include "profile.h"

class EngineBridge;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QPlainTextEdit;
class QProgressBar;
class QCheckBox;
class QAction;
class QTimer;

/*
 * Main window: choose a profile, connect or disconnect, watch the log.
 *
 * The window owns the engine (through EngineBridge) for the whole application
 * lifetime, so reconnecting many times in one session does not churn threads
 * or leak the engine's global state.
 *
 * The authentication state shown at the top is derived from the engine's own
 * log lines (it has no structured progress API), which keeps the engine
 * untouched and the mapping easy to audit.
 */
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    /* Stop the engine and persist profiles; called before the app quits */
    void shutdown();

    /*
     * A dialog editing the current profile, for --profile-screenshot.
     * Ownership passes to the caller.
     */
    QWidget* createProfileDialogForTesting();

    /*
     * Start authenticating with `profile` (used by --connect-test to let the
     * UI exercise a real engine run without a stored profile).
     */
    void startWithProfile(const Profile& profile);

protected:
    void closeEvent(QCloseEvent* event) override;

private slots:
    void connectOrDisconnect();
    void onEngineLog(int level, const QString& text);
    void onEngineFinished(int reason, bool stopRequested);
    void editProfile();
    void addProfile();
    void duplicateProfile();
    void removeProfile();
    void selectProfile(int index);
    void saveProfiles();
    void refreshInterfaces();
    void saveLogToFile();
    void copyLog();
    void clearLog();
    void showAbout();
    void flushLogView();

private:
    enum class State {
        Idle,           /* not authenticating */
        Starting,       /* engine initialising */
        Authenticating, /* searching for the server / exchanging frames */
        Connected,      /* authentication succeeded */
        Stopping        /* the user asked to disconnect */
    };

    void buildUi();
    void buildActions();
    void reloadProfileCombo(int selectIndex);
    Profile currentProfile() const;
    void setState(State state, const QString& detail = QString());
    void setBusy(bool busy);
    void updateWarnings();
    void appendSeparator(const QString& title);
    void queueLog(int level, const QString& text);
    void restoreSettings();
    void saveSettings();

    EngineBridge* m_engine = nullptr;
    ProfileStore m_store;
    State m_state = State::Idle;

    QComboBox* m_profileCombo = nullptr;
    QLabel* m_summaryLabel = nullptr;
    QLabel* m_stateLabel = nullptr;
    QLabel* m_detailLabel = nullptr;
    QLabel* m_warningLabel = nullptr;
    QProgressBar* m_busyBar = nullptr;
    QPushButton* m_connectButton = nullptr;
    QPushButton* m_editButton = nullptr;
    QAction* m_connectAction = nullptr;
    QAction* m_editAction = nullptr;
    QPlainTextEdit* m_logView = nullptr;
    QCheckBox* m_autoScroll = nullptr;
    QLineEdit* m_search = nullptr;

    /* Log lines are buffered and appended in batches to keep the UI smooth */
    QTimer* m_logFlushTimer = nullptr;
    QStringList m_pendingTexts;
    QList<int> m_pendingLevels;

    static constexpr int MaxLogBlocks = 20000;
};

#endif
