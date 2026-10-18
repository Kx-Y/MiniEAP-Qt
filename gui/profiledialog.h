#ifndef MINIEAP_GUI_PROFILEDIALOG_H
#define MINIEAP_GUI_PROFILEDIALOG_H

#include <QDialog>

#include "profile.h"

class QComboBox;
class QLineEdit;
class QPlainTextEdit;
class QSpinBox;
class QCheckBox;
class QLabel;
class QTabWidget;

/*
 * Editor for one Profile.
 *
 * Tabs follow the engine's own grouping: what the user must fill in, how the
 * engine behaves, and the Ruijie extension. The command line preview at the
 * bottom shows exactly what will be handed to the engine, which keeps the GUI
 * checkable against `minieap -h`.
 */
class ProfileDialog : public QDialog {
    Q_OBJECT
public:
    ProfileDialog(const Profile& profile, QWidget* parent = nullptr);

    Profile profile() const { return m_profile; }

private slots:
    void gatherToProfile();
    void updatePreview();
    void refreshInterfaces();
    void browseDhcpScript();

private:
    QWidget* buildConnectionTab();
    QWidget* buildBehaviourTab();
    QWidget* buildRuijieTab();
    QWidget* buildAdvancedTab();
    void loadFromProfile();
    bool validate();

    Profile m_profile;

    QLineEdit* m_name = nullptr;
    QLineEdit* m_username = nullptr;
    QLineEdit* m_password = nullptr;
    QComboBox* m_interface = nullptr;
    QComboBox* m_ifImpl = nullptr;
    QCheckBox* m_useRjv3 = nullptr;

    QSpinBox* m_stageTimeout = nullptr;
    QSpinBox* m_waitAfterFail = nullptr;
    QSpinBox* m_maxFailures = nullptr;
    QSpinBox* m_maxRetries = nullptr;
    QSpinBox* m_authRound = nullptr;
    QCheckBox* m_autoReauth = nullptr;

    QSpinBox* m_heartbeat = nullptr;
    QComboBox* m_dhcpType = nullptr;
    QLineEdit* m_dhcpScript = nullptr;
    QLineEdit* m_service = nullptr;
    QLineEdit* m_versionStr = nullptr;
    QLineEdit* m_fakeDns1 = nullptr;
    QLineEdit* m_fakeDns2 = nullptr;
    QLineEdit* m_fakeSerial = nullptr;

    QPlainTextEdit* m_extraArgs = nullptr;
    QLineEdit* m_preview = nullptr;

    QTabWidget* m_tabs = nullptr;
};

#endif
