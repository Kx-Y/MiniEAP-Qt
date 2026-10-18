#include "profiledialog.h"

#include "netinterfaces.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QVBoxLayout>

namespace {

QLabel* hintLabel(const QString& text)
{
    auto* label = new QLabel(text);
    label->setWordWrap(true);
    label->setStyleSheet(QStringLiteral("color: palette(mid);"));
    return label;
}

} // namespace

ProfileDialog::ProfileDialog(const Profile& profile, QWidget* parent)
    : QDialog(parent)
    , m_profile(profile)
{
    setWindowTitle(tr("编辑配置"));
    resize(560, 560);

    m_tabs = new QTabWidget(this);
    m_tabs->addTab(buildConnectionTab(), tr("连接"));
    m_tabs->addTab(buildBehaviourTab(), tr("认证行为"));
    m_tabs->addTab(buildRuijieTab(), tr("锐捷扩展"));
    m_tabs->addTab(buildAdvancedTab(), tr("高级"));

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, [this]() {
        gatherToProfile();
        if (validate()) {
            accept();
        }
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(m_tabs);
    layout->addWidget(buttons);

    loadFromProfile();
    updatePreview();

    /* Keep the command line preview in sync with any edit */
    const QList<QLineEdit*> lines = findChildren<QLineEdit*>();
    for (QLineEdit* line : lines) {
        connect(line, &QLineEdit::textChanged, this, &ProfileDialog::updatePreview);
    }
    const QList<QSpinBox*> spins = findChildren<QSpinBox*>();
    for (QSpinBox* spin : spins) {
        connect(spin, QOverload<int>::of(&QSpinBox::valueChanged), this,
                &ProfileDialog::updatePreview);
    }
    const QList<QComboBox*> combos = findChildren<QComboBox*>();
    for (QComboBox* combo : combos) {
        connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                &ProfileDialog::updatePreview);
    }
    const QList<QCheckBox*> checks = findChildren<QCheckBox*>();
    for (QCheckBox* check : checks) {
        connect(check, &QCheckBox::toggled, this, &ProfileDialog::updatePreview);
    }
    connect(m_extraArgs, &QPlainTextEdit::textChanged, this, &ProfileDialog::updatePreview);
}

QWidget* ProfileDialog::buildConnectionTab()
{
    auto* page = new QWidget(this);
    auto* form = new QFormLayout(page);

    m_name = new QLineEdit(page);
    m_name->setPlaceholderText(tr("例如：宿舍、实验室"));
    form->addRow(tr("配置名称"), m_name);

    m_username = new QLineEdit(page);
    m_username->setPlaceholderText(tr("学号 / 上网账号"));
    form->addRow(tr("用户名"), m_username);

    m_password = new QLineEdit(page);
    m_password->setEchoMode(QLineEdit::Password);
    form->addRow(tr("密码"), m_password);

    auto* ifRow = new QHBoxLayout();
    m_interface = new QComboBox(page);
    m_interface->setEditable(true); /* Allow adapters the API does not list */
    m_interface->setMinimumWidth(260);
    auto* refresh = new QPushButton(tr("刷新"), page);
    connect(refresh, &QPushButton::clicked, this, &ProfileDialog::refreshInterfaces);
    ifRow->addWidget(m_interface, 1);
    ifRow->addWidget(refresh);
    form->addRow(tr("网络接口"), ifRow);
    form->addRow(QString(), hintLabel(tr("选择连接校园网的网卡。无线认证请选择 WLAN。")));

    m_ifImpl = new QComboBox(page);
    m_ifImpl->addItem(tr("自动（第一个可用模块）"), QString());
    for (const QString& module : NetInterfaces::captureModules()) {
        m_ifImpl->addItem(module, module);
    }
    form->addRow(tr("抓包模块"), m_ifImpl);
    form->addRow(QString(), hintLabel(tr("Windows 下使用 Npcap；若未安装 Npcap，程序会在日志中提示。")));

    m_useRjv3 = new QCheckBox(tr("使用锐捷认证扩展（rjv3）"), page);
    m_useRjv3->setChecked(true);
    form->addRow(QString(), m_useRjv3);
    form->addRow(QString(),
                 hintLabel(tr("锐捷认证服务器需要此选项；标准 802.1x (MD5-Challenge) 服务器可关闭。")));

    return page;
}

QWidget* ProfileDialog::buildBehaviourTab()
{
    auto* page = new QWidget(this);
    auto* form = new QFormLayout(page);

    /*
     * 0 is a meaningful value for both of these and is what the console
     * client's documented working configuration uses (stage-timeout=0,
     * max-retries=0): the engine then waits forever instead of giving up.
     * The spin boxes must therefore allow 0 - a lower bound of 1 made those
     * settings impossible to enter from the GUI.
     */
    m_stageTimeout = new QSpinBox(page);
    m_stageTimeout->setRange(0, 120);
    m_stageTimeout->setSuffix(tr(" 秒"));
    m_stageTimeout->setSpecialValueText(tr("0（不超时）"));
    form->addRow(tr("阶段超时"), m_stageTimeout);
    form->addRow(QString(), hintLabel(tr("等待服务器响应的最长时间，超时后会重发认证包。"
                                         "设为 0 表示不超时，一直等待——校园网认证通常需要如此。")));

    m_waitAfterFail = new QSpinBox(page);
    m_waitAfterFail->setRange(0, 3600);
    m_waitAfterFail->setSuffix(tr(" 秒"));
    form->addRow(tr("失败后等待"), m_waitAfterFail);

    m_maxFailures = new QSpinBox(page);
    m_maxFailures->setRange(1, 100);
    m_maxFailures->setSuffix(tr(" 次"));
    form->addRow(tr("最大失败次数"), m_maxFailures);
    form->addRow(QString(), hintLabel(tr("连续失败达到此次数后停止认证。")));

    m_maxRetries = new QSpinBox(page);
    m_maxRetries->setRange(0, 100);
    m_maxRetries->setSuffix(tr(" 次"));
    m_maxRetries->setSpecialValueText(tr("0（不限制）"));
    form->addRow(tr("超时重试次数"), m_maxRetries);
    form->addRow(QString(), hintLabel(tr("在同一阶段超时重试达到此次数后判定失败。"
                                         "设为 0 表示不限制重试次数。")));

    m_authRound = new QSpinBox(page);
    m_authRound->setRange(1, 10);
    m_authRound->setSuffix(tr(" 轮"));
    form->addRow(tr("认证轮数"), m_authRound);

    m_autoReauth = new QCheckBox(tr("被服务器强制下线后自动重新认证"), page);
    form->addRow(QString(), m_autoReauth);

    return page;
}

QWidget* ProfileDialog::buildRuijieTab()
{
    auto* page = new QWidget(this);
    auto* form = new QFormLayout(page);

    m_heartbeat = new QSpinBox(page);
    m_heartbeat->setRange(1, 3600);
    m_heartbeat->setSuffix(tr(" 秒"));
    form->addRow(tr("心跳间隔"), m_heartbeat);

    m_dhcpType = new QComboBox(page);
    m_dhcpType->addItem(tr("不使用 DHCP"), 0);
    m_dhcpType->addItem(tr("二次认证"), 1);
    m_dhcpType->addItem(tr("认证后 DHCP"), 2);
    m_dhcpType->addItem(tr("认证前 DHCP"), 3);
    form->addRow(tr("DHCP 方式"), m_dhcpType);
    form->addRow(QString(), hintLabel(tr("多数锐捷校园网使用“二次认证”。不确定时保持默认。")));

    auto* scriptRow = new QHBoxLayout();
    m_dhcpScript = new QLineEdit(page);
    m_dhcpScript->setPlaceholderText(tr("可选：在 DHCP 阶段执行的命令"));
    auto* browse = new QPushButton(tr("浏览…"), page);
    connect(browse, &QPushButton::clicked, this, &ProfileDialog::browseDhcpScript);
    scriptRow->addWidget(m_dhcpScript, 1);
    scriptRow->addWidget(browse);
    form->addRow(tr("DHCP 脚本"), scriptRow);

    m_service = new QLineEdit(page);
    m_service->setPlaceholderText(tr("留空使用默认值"));
    form->addRow(tr("服务名"), m_service);

    m_versionStr = new QLineEdit(page);
    m_versionStr->setPlaceholderText(tr("留空使用默认值"));
    form->addRow(tr("版本字符串"), m_versionStr);

    m_fakeDns1 = new QLineEdit(page);
    m_fakeDns1->setPlaceholderText(tr("自动获取"));
    form->addRow(tr("主 DNS"), m_fakeDns1);

    m_fakeDns2 = new QLineEdit(page);
    m_fakeDns2->setPlaceholderText(tr("自动获取"));
    form->addRow(tr("次 DNS"), m_fakeDns2);

    m_fakeSerial = new QLineEdit(page);
    m_fakeSerial->setPlaceholderText(tr("自动获取"));
    form->addRow(tr("硬盘序列号"), m_fakeSerial);
    form->addRow(QString(), hintLabel(tr("以上字段通常无需修改，仅当认证被服务器拒绝时才需要手动指定。")));

    return page;
}

QWidget* ProfileDialog::buildAdvancedTab()
{
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);

    layout->addWidget(new QLabel(tr("附加命令行参数（原样传给认证引擎，每行一个或按空格分隔）："), page));
    m_extraArgs = new QPlainTextEdit(page);
    m_extraArgs->setPlaceholderText(QStringLiteral("--rj-option 6a:000102"));
    layout->addWidget(m_extraArgs, 1);

    layout->addWidget(new QLabel(tr("完整命令行（自动生成）："), page));
    m_preview = new QLineEdit(page);
    m_preview->setReadOnly(true);
    /* Do not leak the password into screenshots / logs by default */
    m_preview->setEchoMode(QLineEdit::PasswordEchoOnEdit);
    layout->addWidget(m_preview);

    layout->addWidget(hintLabel(tr("这些参数与 minieap 命令行完全一致，可在终端中用相同参数复现。")));
    return page;
}

void ProfileDialog::loadFromProfile()
{
    m_name->setText(m_profile.name);
    m_username->setText(m_profile.username);
    m_password->setText(m_profile.password);
    refreshInterfaces();
    if (!m_profile.interface.isEmpty()) {
        m_interface->setEditText(m_profile.interface);
    }

    const int implIndex = m_ifImpl->findData(m_profile.ifImpl);
    m_ifImpl->setCurrentIndex(implIndex >= 0 ? implIndex : 0);

    m_useRjv3->setChecked(m_profile.modules.contains(QStringLiteral("rjv3")));

    m_stageTimeout->setValue(m_profile.stageTimeout);
    m_waitAfterFail->setValue(m_profile.waitAfterFail);
    m_maxFailures->setValue(m_profile.maxFailures);
    m_maxRetries->setValue(m_profile.maxRetries);
    m_authRound->setValue(m_profile.authRound);
    m_autoReauth->setChecked(m_profile.autoReauth);

    m_heartbeat->setValue(m_profile.heartbeat);
    const int dhcpIndex = m_dhcpType->findData(m_profile.dhcpType);
    m_dhcpType->setCurrentIndex(dhcpIndex >= 0 ? dhcpIndex : 0);
    m_dhcpScript->setText(m_profile.dhcpScript);
    m_service->setText(m_profile.service);
    m_versionStr->setText(m_profile.versionStr);
    m_fakeDns1->setText(m_profile.fakeDns1);
    m_fakeDns2->setText(m_profile.fakeDns2);
    m_fakeSerial->setText(m_profile.fakeSerial);

    m_extraArgs->setPlainText(m_profile.extraArguments);
}

void ProfileDialog::refreshInterfaces()
{
    const QString current = m_interface->currentText();

    m_interface->clear();
    for (const InterfaceInfo& info : NetInterfaces::enumerate()) {
        QString label = info.name;
        QStringList details;
        if (!info.addresses.isEmpty()) {
            details << info.addresses.first();
        }
        if (!info.macAddress.isEmpty()) {
            details << info.macAddress;
        }
        if (!details.isEmpty()) {
            label += QStringLiteral("  (%1)").arg(details.join(QStringLiteral(", ")));
        }
        m_interface->addItem(label, info.name);
    }

    /* Restore the previous selection, even if it is no longer present */
    const int index = m_interface->findData(current);
    if (index >= 0) {
        m_interface->setCurrentIndex(index);
    } else if (!current.isEmpty()) {
        m_interface->setEditText(current);
    }
}

void ProfileDialog::browseDhcpScript()
{
    const QString path = QFileDialog::getOpenFileName(this, tr("选择 DHCP 脚本"));
    if (!path.isEmpty()) {
        m_dhcpScript->setText(path);
    }
}

void ProfileDialog::gatherToProfile()
{
    m_profile.name = m_name->text().trimmed();
    if (m_profile.name.isEmpty()) {
        m_profile.name = tr("未命名配置");
    }
    m_profile.username = m_username->text().trimmed();
    m_profile.password = m_password->text();
    /* The combo stores the plain adapter name as data */
    m_profile.interface = m_interface->currentData().toString();
    if (m_profile.interface.isEmpty()) {
        m_profile.interface = m_interface->currentText().section(QLatin1Char(' '), 0, 0).trimmed();
    }
    m_profile.ifImpl = m_ifImpl->currentData().toString();

    m_profile.modules.clear();
    if (m_useRjv3->isChecked()) {
        m_profile.modules << QStringLiteral("rjv3");
    }

    m_profile.stageTimeout = m_stageTimeout->value();
    m_profile.waitAfterFail = m_waitAfterFail->value();
    m_profile.maxFailures = m_maxFailures->value();
    m_profile.maxRetries = m_maxRetries->value();
    m_profile.authRound = m_authRound->value();
    m_profile.autoReauth = m_autoReauth->isChecked();

    m_profile.heartbeat = m_heartbeat->value();
    m_profile.dhcpType = m_dhcpType->currentData().toInt();
    m_profile.dhcpScript = m_dhcpScript->text();
    m_profile.service = m_service->text();
    m_profile.versionStr = m_versionStr->text();
    m_profile.fakeDns1 = m_fakeDns1->text();
    m_profile.fakeDns2 = m_fakeDns2->text();
    m_profile.fakeSerial = m_fakeSerial->text();

    m_profile.extraArguments = m_extraArgs->toPlainText();
}

void ProfileDialog::updatePreview()
{
    if (m_preview == nullptr) {
        return;
    }
    gatherToProfile();
    QStringList args = m_profile.toArguments();
    /* Quote arguments that contain spaces, like a shell would */
    for (QString& arg : args) {
        if (arg.contains(QLatin1Char(' '))) {
            arg = QStringLiteral("\"%1\"").arg(arg);
        }
    }
    m_preview->setText(QStringLiteral("minieap ") + args.join(QLatin1Char(' ')));
}

bool ProfileDialog::validate()
{
    QString error;
    if (!m_profile.isValid(&error)) {
        QMessageBox::warning(this, tr("配置不完整"), error);
        m_tabs->setCurrentIndex(0);
        return false;
    }
    return true;
}
