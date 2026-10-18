#ifndef MINIEAP_GUI_PROFILE_H
#define MINIEAP_GUI_PROFILE_H

#include <QList>
#include <QJsonObject>
#include <QString>
#include <QStringList>

/*
 * A named set of authentication settings.
 *
 * Mirrors the command line of the console client one to one: every field maps
 * to an option, so anything the GUI can do is reproducible on the command line
 * (and vice versa).
 */
struct Profile {
    QString name = QStringLiteral("默认配置");

    /* Required: -u / -p / -n */
    QString username;
    QString password;
    QString interface;
    /* Network module: --if-impl (empty = the first available one) */
    QString ifImpl;

    /* Packet plugin(s): --module, in order. Usually just "rjv3" */
    QStringList modules = { QStringLiteral("rjv3") };

    /* Behaviour: same defaults as the engine */
    int stageTimeout = 5;      /* -t */
    int waitAfterFail = 30;    /* -r */
    int maxFailures = 3;       /* -l */
    int maxRetries = 3;        /* --max-retries */
    bool autoReauth = true;    /* -x clears it */
    int authRound = 1;         /* -j */

    /* Ruijie plugin options (rjv3) */
    int heartbeat = 30;        /* -e */
    int dhcpType = 0;          /* -d: 0 none, 1 double auth, 2 after auth, 3 before auth */
    QString dhcpScript;        /* -c */
    QString service;           /* --service */
    QString versionStr;        /* --version-str */
    QString fakeDns1;          /* --fake-dns1 */
    QString fakeDns2;          /* --fake-dns2 */
    QString fakeSerial;        /* --fake-serial */

    /* Anything the GUI does not model, appended verbatim (advanced users) */
    QString extraArguments;

    /*
     * Build the engine command line for this profile.
     * Paths that the engine treats as files are passed through as given.
     */
    QStringList toArguments() const;

    /* A short human readable summary for the profile list */
    QString summary() const;

    bool isValid(QString* error = nullptr) const;

    /* JSON (de)serialisation for the settings store */
    QJsonObject toJson() const;
    static Profile fromJson(const QJsonObject& obj);
};

/*
 * All profiles plus the selected one, persisted as JSON in the user's
 * configuration directory.
 */
class ProfileStore {
public:
    /* Load from the standard location (creates an empty store when absent) */
    void load();
    bool save() const;

    QList<Profile> profiles() const { return m_profiles; }
    void setProfiles(const QList<Profile>& profiles) { m_profiles = profiles; }

    /* Index of the active profile, clamped to the list */
    int currentIndex() const { return m_current; }
    void setCurrentIndex(int index) { m_current = index; }

    /* Path of the JSON file, for the UI / diagnostics */
    static QString filePath();

private:
    QList<Profile> m_profiles;
    int m_current = 0;
};

#endif
