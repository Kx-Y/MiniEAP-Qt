#include "mainwindow.h"

#include "appversion.h"
#include "enginebridge.h"
#include "netinterfaces.h"
#include "profiledialog.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QClipboard>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFontDatabase>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollBar>
#include <QSettings>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QTimer>
#include <QToolBar>
#include <QVBoxLayout>

namespace {

/*
 * Line colours per engine log level (INFO/WARN/ERROR/DEBUG).
 *
 * Fusion keeps the palette's mid grey, which is too faint on a dark system
 * background (the engine's own "[time][I]" prefix is already coloured). Use a
 * fixed, readable orange that works on light and dark themes.
 */
QColor levelColor(int level, bool dark)
{
    switch (level) {
    case 2: /* warning */
        return dark ? QColor(0xe0, 0xa0, 0x50) : QColor(0xb2, 0x5b, 0x00);
    case 3: /* error */
        return dark ? QColor(0xf0, 0x70, 0x70) : QColor(0xc0, 0x2b, 0x2b);
    case 4: /* debug */
        return QColor(0x80, 0x80, 0x80);
    default: /* info and raw output */
        return QColor();
    }
}

/*
 * Does `text` contain any of the given tokens?
 *
 * The tokens are narrow string literals holding UTF-8 (this file's encoding),
 * so they must be decoded as UTF-8. QLatin1String would reinterpret each byte
 * as a Latin-1 character, producing mojibake that never matches the decoded
 * engine output - which is why the UI stayed at "connecting" after a
 * successful login until this was fixed.
 */
bool containsAny(const QString& text, std::initializer_list<const char*> needles)
{
    for (const char* needle : needles) {
        if (text.contains(QString::fromUtf8(needle))) {
            return true;
        }
    }
    return false;
}

/* Command line preview that does not leak the password into screenshots */
QString maskedCommand(const QStringList& args)
{
    QStringList copy = args;
    for (int i = 0; i + 1 < copy.size(); ++i) {
        if (copy.at(i) == QLatin1String("-p") || copy.at(i) == QLatin1String("--password")) {
            copy[i + 1] = QStringLiteral("******");
        }
    }
    for (QString& arg : copy) {
        if (arg.contains(QLatin1Char(' '))) {
            arg = QLatin1Char('"') + arg + QLatin1Char('"');
        }
    }
    return QStringLiteral("minieap ") + copy.join(QLatin1Char(' '));
}

} // namespace

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    m_engine = new EngineBridge(this);
    connect(m_engine, &EngineBridge::logLine, this, &MainWindow::onEngineLog);
    connect(m_engine, &EngineBridge::finished, this, &MainWindow::onEngineFinished);

    /* Batch log appends so a chatty session cannot starve the UI thread */
    m_logFlushTimer = new QTimer(this);
    m_logFlushTimer->setInterval(120);
    connect(m_logFlushTimer, &QTimer::timeout, this, &MainWindow::flushLogView);

    buildUi();
    buildActions();
    restoreSettings();

    m_store.load();
    reloadProfileCombo(m_store.currentIndex());
    setState(State::Idle);
    updateWarnings();

    m_logFlushTimer->start();
}

MainWindow::~MainWindow()
{
    /* EngineBridge's destructor joins the worker thread; the engine itself
     * must already be stopped by then, see closeEvent()/shutdown(). */
}

/* -------------------------------------------------------------------- UI */

void MainWindow::buildUi()
{
    setWindowTitle(tr("MiniEAP - 802.1x 认证客户端"));
    resize(900, 660);
    setMinimumSize(620, 480);

    auto* central = new QWidget(this);
    auto* outer = new QVBoxLayout(central);
    outer->setContentsMargins(12, 12, 12, 8);
    outer->setSpacing(10);

    /* Profile selection */
    auto* profileRow = new QHBoxLayout();
    profileRow->addWidget(new QLabel(tr("配置："), central));

    m_profileCombo = new QComboBox(central);
    m_profileCombo->setMinimumWidth(200);
    connect(m_profileCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &MainWindow::selectProfile);
    profileRow->addWidget(m_profileCombo, 1);

    m_editButton = new QPushButton(tr("编辑…"), central);
    connect(m_editButton, &QPushButton::clicked, this, &MainWindow::editProfile);
    profileRow->addWidget(m_editButton);

    auto* addButton = new QPushButton(tr("新建"), central);
    connect(addButton, &QPushButton::clicked, this, &MainWindow::addProfile);
    profileRow->addWidget(addButton);

    auto* moreButton = new QPushButton(tr("更多"), central);
    auto* moreMenu = new QMenu(moreButton);
    moreMenu->addAction(tr("复制当前配置"), this, &MainWindow::duplicateProfile);
    moreMenu->addAction(tr("删除当前配置"), this, &MainWindow::removeProfile);
    moreMenu->addSeparator();
    moreMenu->addAction(tr("显示配置文件位置"), this, [this]() {
        QMessageBox::information(this, tr("配置文件位置"), ProfileStore::filePath());
    });
    moreButton->setMenu(moreMenu);
    profileRow->addWidget(moreButton);

    outer->addLayout(profileRow);

    /* Status card */
    auto* card = new QFrame(central);
    card->setFrameShape(QFrame::StyledPanel);
    auto* cardLayout = new QVBoxLayout(card);
    cardLayout->setSpacing(6);

    auto* statusRow = new QHBoxLayout();
    m_stateLabel = new QLabel(tr("未连接"), card);
    QFont stateFont = m_stateLabel->font();
    stateFont.setPointSizeF(stateFont.pointSizeF() * 1.45);
    stateFont.setBold(true);
    m_stateLabel->setFont(stateFont);
    statusRow->addWidget(m_stateLabel);
    statusRow->addStretch(1);

    m_connectButton = new QPushButton(tr("连接"), card);
    m_connectButton->setMinimumSize(118, 34);
    connect(m_connectButton, &QPushButton::clicked, this, &MainWindow::connectOrDisconnect);
    statusRow->addWidget(m_connectButton);
    cardLayout->addLayout(statusRow);

    m_busyBar = new QProgressBar(card);
    m_busyBar->setRange(0, 0); /* indeterminate */
    m_busyBar->setTextVisible(false);
    m_busyBar->setMaximumHeight(4);
    m_busyBar->hide();
    cardLayout->addWidget(m_busyBar);

    m_summaryLabel = new QLabel(card);
    m_summaryLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    cardLayout->addWidget(m_summaryLabel);

    m_detailLabel = new QLabel(card);
    m_detailLabel->setWordWrap(true);
    m_detailLabel->setStyleSheet(QStringLiteral("color: palette(text);"));
    m_detailLabel->hide();
    cardLayout->addWidget(m_detailLabel);

    m_warningLabel = new QLabel(card);
    m_warningLabel->setWordWrap(true);
    m_warningLabel->setStyleSheet(QStringLiteral("color: #b26a00;"));
    m_warningLabel->hide();
    cardLayout->addWidget(m_warningLabel);

    outer->addWidget(card);

    /* Log header */
    auto* logHeader = new QHBoxLayout();
    logHeader->addWidget(new QLabel(tr("日志"), central));
    logHeader->addStretch(1);

    m_search = new QLineEdit(central);
    m_search->setPlaceholderText(tr("筛选日志…"));
    m_search->setClearButtonEnabled(true);
    m_search->setMaximumWidth(200);
    connect(m_search, &QLineEdit::textChanged, this, [this](const QString& text) {
        /* Simple case-insensitive search: jump to the next match */
        if (text.isEmpty()) {
            return;
        }
        m_logView->moveCursor(QTextCursor::Start);
        if (!m_logView->find(text)) {
            statusBar()->showMessage(tr("未找到：%1").arg(text), 3000);
        } else {
            statusBar()->showMessage(tr("正在筛选：%1").arg(text), 3000);
        }
    });
    logHeader->addWidget(m_search);

    m_autoScroll = new QCheckBox(tr("自动滚动"), central);
    m_autoScroll->setChecked(true);
    logHeader->addWidget(m_autoScroll);
    outer->addLayout(logHeader);

    m_logView = new QPlainTextEdit(central);
    m_logView->setReadOnly(true);
    m_logView->setMaximumBlockCount(MaxLogBlocks);
    m_logView->setLineWrapMode(QPlainTextEdit::NoWrap);
    QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    mono.setPointSizeF(mono.pointSizeF() > 0 ? mono.pointSizeF() : 9.0);
    m_logView->setFont(mono);
    outer->addWidget(m_logView, 1);

    setCentralWidget(central);
    statusBar()->showMessage(tr("就绪"));
}

void MainWindow::buildActions()
{
    QMenu* fileMenu = menuBar()->addMenu(tr("文件(&F)"));
    QAction* saveLog = fileMenu->addAction(tr("保存日志…"), this, &MainWindow::saveLogToFile);
    saveLog->setShortcut(QKeySequence::Save);
    fileMenu->addAction(tr("复制日志"), this, &MainWindow::copyLog);
    fileMenu->addAction(tr("清空日志"), this, &MainWindow::clearLog);
    fileMenu->addSeparator();
    QAction* quit = fileMenu->addAction(tr("退出"), this, &QWidget::close);
    quit->setShortcut(QKeySequence::Quit);

    QMenu* profileMenu = menuBar()->addMenu(tr("配置(&P)"));
    m_editAction = profileMenu->addAction(tr("编辑当前配置…"), this, &MainWindow::editProfile);
    profileMenu->addAction(tr("新建配置…"), this, &MainWindow::addProfile);
    profileMenu->addSeparator();
    profileMenu->addAction(tr("刷新网络接口"), this, &MainWindow::refreshInterfaces);

    QMenu* helpMenu = menuBar()->addMenu(tr("帮助(&H)"));
    helpMenu->addAction(tr("关于 MiniEAP"), this, &MainWindow::showAbout);

    /*
     * No toolbar: the connect button lives on the status card, and a toolbar
     * duplicate of it was confusing (two buttons doing the same thing).
     * Keyboard shortcuts cover the common actions instead.
     */
    QAction* connectAction = new QAction(tr("连接 / 断开"), this);
    connectAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Return")));
    connectAction->setShortcutContext(Qt::ApplicationShortcut);
    connect(connectAction, &QAction::triggered, this, &MainWindow::connectOrDisconnect);
    addAction(connectAction);
    m_connectAction = connectAction;

    QAction* refreshAction = new QAction(tr("刷新网络接口"), this);
    refreshAction->setShortcut(QKeySequence(QStringLiteral("F5")));
    connect(refreshAction, &QAction::triggered, this, &MainWindow::refreshInterfaces);
    addAction(refreshAction);
}/* --------------------------------------------------------------- state */

void MainWindow::setState(State state, const QString& detail)
{
    m_state = state;

    /*
     * Text colour for the state headline. "palette(mid)" (used for idle and
     * stopping) is too faint on the dark Fusion theme, so pick a readable grey
     * that still stands apart from the coloured active/connected states.
     */
    const bool dark = palette().color(QPalette::Window).lightness() < 128;
    const QString neutral = dark ? QStringLiteral("#b0b0b0") : QStringLiteral("#606060");
    const QString accent = dark ? QStringLiteral("#6ea8ff") : QStringLiteral("#1667c9");
    const QString success = dark ? QStringLiteral("#5cc46a") : QStringLiteral("#1b7f3b");

    QString text;
    QString color;
    switch (state) {
    case State::Idle:
        text = tr("未连接");
        color = neutral;
        break;
    case State::Starting:
        text = tr("正在启动…");
        color = accent;
        break;
    case State::Authenticating:
        text = tr("正在认证…");
        color = accent;
        break;
    case State::Connected:
        text = tr("已连接");
        color = success;
        break;
    case State::Stopping:
        text = tr("正在断开…");
        color = neutral;
        break;
    }

    m_stateLabel->setText(text);
    m_stateLabel->setStyleSheet(QStringLiteral("color: %1;").arg(color));

    if (detail.isEmpty()) {
        m_detailLabel->clear();
        m_detailLabel->hide();
    } else {
        m_detailLabel->setText(detail);
        m_detailLabel->show();
    }

    const bool busy = (state == State::Starting || state == State::Authenticating
                       || state == State::Stopping);
    setBusy(busy);

    const bool active = (state != State::Idle);
    m_connectButton->setText(active ? tr("断开") : tr("连接"));
    m_connectButton->setEnabled(state != State::Stopping);
    if (m_connectAction != nullptr) {
        m_connectAction->setText(active ? tr("断开") : tr("连接"));
    }

    /* The engine holds the profile while it runs: do not edit underneath it */
    const bool editable = (state == State::Idle);
    m_editButton->setEnabled(editable);
    if (m_editAction != nullptr) {
        m_editAction->setEnabled(editable);
    }
}

void MainWindow::setBusy(bool busy)
{
    m_busyBar->setVisible(busy);
}

/* ------------------------------------------------------------ profiles */

void MainWindow::reloadProfileCombo(int selectIndex)
{
    const QSignalBlocker blocker(m_profileCombo);
    m_profileCombo->clear();

    const QList<Profile> profiles = m_store.profiles();
    for (const Profile& profile : profiles) {
        m_profileCombo->addItem(profile.name);
    }
    if (selectIndex >= 0 && selectIndex < m_profileCombo->count()) {
        m_profileCombo->setCurrentIndex(selectIndex);
    }
}

Profile MainWindow::currentProfile() const
{
    const QList<Profile> profiles = m_store.profiles();
    const int index = m_profileCombo->currentIndex();
    if (index >= 0 && index < profiles.size()) {
        return profiles.at(index);
    }
    return Profile();
}

void MainWindow::selectProfile(int index)
{
    if (index < 0) {
        return;
    }
    m_store.setCurrentIndex(index);
    updateWarnings();
    saveProfiles();
}

void MainWindow::addProfile()
{
    if (m_state != State::Idle) {
        return;
    }
    Profile profile;
    profile.name = tr("新配置 %1").arg(m_store.profiles().size() + 1);

    ProfileDialog dialog(profile, this);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    QList<Profile> profiles = m_store.profiles();
    profiles << dialog.profile();
    m_store.setProfiles(profiles);
    m_store.setCurrentIndex(static_cast<int>(profiles.size()) - 1);
    reloadProfileCombo(m_store.currentIndex());
    selectProfile(m_store.currentIndex());
}

void MainWindow::editProfile()
{
    if (m_state != State::Idle) {
        return;
    }
    const int index = m_profileCombo->currentIndex();

    ProfileDialog dialog(currentProfile(), this);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    QList<Profile> profiles = m_store.profiles();
    if (index < 0 || index >= profiles.size()) {
        return;
    }
    profiles[index] = dialog.profile();
    m_store.setProfiles(profiles);
    reloadProfileCombo(index);
    selectProfile(index);
}

void MainWindow::duplicateProfile()
{
    QList<Profile> profiles = m_store.profiles();
    const int index = m_profileCombo->currentIndex();
    if (index < 0 || index >= profiles.size()) {
        return;
    }

    Profile copy = profiles.at(index);
    copy.name = tr("%1 副本").arg(copy.name);
    profiles << copy;
    m_store.setProfiles(profiles);
    m_store.setCurrentIndex(static_cast<int>(profiles.size()) - 1);
    reloadProfileCombo(m_store.currentIndex());
    selectProfile(m_store.currentIndex());
}

void MainWindow::removeProfile()
{
    QList<Profile> profiles = m_store.profiles();
    if (profiles.size() <= 1) {
        QMessageBox::information(this, tr("无法删除"), tr("至少需要保留一个配置。"));
        return;
    }
    const int index = m_profileCombo->currentIndex();
    if (index < 0 || index >= profiles.size()) {
        return;
    }
    if (QMessageBox::question(this, tr("删除配置"),
                              tr("确定删除配置“%1”吗？").arg(profiles.at(index).name))
        != QMessageBox::Yes) {
        return;
    }

    profiles.removeAt(index);
    m_store.setProfiles(profiles);
    const int next = qBound(0, index - 1, static_cast<int>(profiles.size()) - 1);
    m_store.setCurrentIndex(next);
    reloadProfileCombo(next);
    selectProfile(next);
}

void MainWindow::saveProfiles()
{
    if (!m_store.save()) {
        statusBar()->showMessage(tr("配置保存失败：%1").arg(ProfileStore::filePath()), 8000);
    }
}

void MainWindow::refreshInterfaces()
{
    updateWarnings();
    statusBar()->showMessage(tr("已刷新网络接口列表"), 3000);
}

void MainWindow::updateWarnings()
{
    const Profile profile = currentProfile();
    QStringList warnings;

    m_summaryLabel->setText(profile.summary());

    if (!profile.interface.isEmpty() && !NetInterfaces::exists(profile.interface)) {
        warnings << tr("找不到网络接口“%1”。请点“配置 → 刷新网络接口”，"
                       "或在配置中重新选择网卡。")
                        .arg(profile.interface);
    }

    m_warningLabel->setText(warnings.join(QLatin1Char('\n')));
    m_warningLabel->setVisible(!warnings.isEmpty());
}

/* ---------------------------------------------------------- connection */

void MainWindow::connectOrDisconnect()
{
    if (m_engine->isRunning()) {
        queueLog(1, tr(">>> 正在断开连接…"));
        m_engine->stop();
        setState(State::Stopping);
        return;
    }
    if (m_state == State::Stopping) {
        return;
    }

    const Profile profile = currentProfile();
    QString error;
    if (!profile.isValid(&error)) {
        QMessageBox::warning(this, tr("配置不完整"),
                             tr("%1\n\n请先点“编辑…”补全配置。").arg(error));
        editProfile();
        return;
    }

    /* Warn, but always let the user try: the adapter list can be stale */
    updateWarnings();

    appendSeparator(tr("开始认证：%1").arg(profile.summary()));

    const QStringList args = profile.toArguments();
    queueLog(1, tr(">>> 启动认证引擎：%1").arg(maskedCommand(args)));

    setState(State::Starting, tr("正在初始化认证引擎…"));
    if (!m_engine->start(args)) {
        setState(State::Idle);
        QMessageBox::critical(this, tr("无法启动"), tr("认证引擎启动失败，请查看日志。"));
        return;
    }
    statusBar()->showMessage(tr("认证引擎已启动"), 4000);
}

void MainWindow::onEngineLog(int level, const QString& text)
{
    queueLog(level, text);

    /* Only INFO lines describe progress; errors keep the current stage but
     * put the message on screen, so the user sees what went wrong. */
    if (level == 3) {
        if (containsAny(text, { "打开设备失败", "捕获准备失败", "设置接口名称失败",
                                "认证失败", "已经停留" })) {
            setState(m_state == State::Connected ? State::Authenticating : m_state, text);
        }
        return;
    }
    if (level == 2) {
        /* Warnings carry the retry/session messages; surface them too */
        if (containsAny(text, { "认证失败", "认证掉线" })) {
            setState(State::Authenticating, text);
        }
        return;
    }
    if (level != 1) {
        return;
    }

    /*
     * State transitions are derived from the engine's log lines because the
     * engine has no structured progress API. Keep the matching tolerant: the
     * lines carry a timestamp prefix and the exact wording differs between
     * stages, so match on the distinctive phrases only.
     */
    if (containsAny(text, { "认证成功", "二次认证成功" })) {
        setState(State::Connected, tr("网络已通过 802.1x 认证，保持心跳中"));
        statusBar()->showMessage(tr("认证成功"), 5000);
        return;
    }

    if (containsAny(text, { "认证掉线" })) {
        setState(State::Authenticating, text);
        return;
    }

    if (m_state == State::Connected) {
        /* Keep-alive traffic on an established session: stay connected */
        return;
    }

    if (containsAny(text, { "正在查找认证服务器", "正在回应用户名请求",
                            "正在回应密码请求", "首次认证成功", "正在执行" })) {
        setState(State::Authenticating, text);
    }
}

void MainWindow::onEngineFinished(int reason, bool stopRequested)
{
    if (stopRequested) {
        setState(State::Idle, tr("已断开"));
        appendSeparator(tr("已断开连接"));
        return;
    }

    QString title;
    QString detail;
    switch (reason) {
    case EngineBridge::ExitAuthFailed:
        title = tr("认证失败");
        detail = tr("连续认证失败次数已达上限。请检查用户名、密码、所选网卡，"
                    "以及是否需要启用锐捷扩展。");
        break;
    case EngineBridge::ExitLoggedOff:
        title = tr("已被服务器强制下线");
        detail = tr("同一账号可能在其他设备上登录，或账号访问受到限制。");
        break;
    case EngineBridge::ExitStateTimeout:
        title = tr("认证超时");
        detail = tr("在某个认证阶段多次超时，一直没收到服务器响应。请确认网线/无线已连接，"
                    "并确认认证方式（锐捷扩展）选择正确。");
        break;
    case EngineBridge::ExitTransportError:
        title = tr("网络操作失败");
        detail = tr("无法通过所选网卡收发认证数据包。Windows 上常见原因是未安装 Npcap，"
                    "或当前用户没有抓包权限（请右键以管理员身份运行）。");
        break;
    default:
        title = tr("认证已结束");
        break;
    }

    setState(State::Idle, title);
    appendSeparator(title);
    if (!detail.isEmpty()) {
        queueLog(3, detail);
    }
    statusBar()->showMessage(title, 8000);
}

/* ------------------------------------------------------------------ log */

void MainWindow::queueLog(int level, const QString& text)
{
    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss"));
    m_pendingTexts << QStringLiteral("[%1] %2").arg(stamp, text);
    m_pendingLevels << level;

    /* Keep the separator/state responsive even if the timer has not fired */
    if (m_pendingTexts.size() >= 200) {
        flushLogView();
    }
}

void MainWindow::appendSeparator(const QString& title)
{
    flushLogView();
    m_pendingTexts << QStringLiteral("=== %1 ===").arg(title);
    m_pendingLevels << 0;
    flushLogView();
}

void MainWindow::flushLogView()
{
    if (m_pendingTexts.isEmpty()) {
        return;
    }

    const bool follow = m_autoScroll->isChecked();
    QScrollBar* scrollBar = m_logView->verticalScrollBar();
    const bool wasAtBottom = (scrollBar->value() >= scrollBar->maximum() - 4);

    QTextCursor cursor(m_logView->document());
    cursor.movePosition(QTextCursor::End);

    /* Repainting once per batch instead of once per line */
    const bool dark = m_logView->palette().color(QPalette::Base).lightness() < 128;
    m_logView->setUpdatesEnabled(false);
    for (int i = 0; i < m_pendingTexts.size(); ++i) {
        QTextCharFormat format;
        const QColor color = levelColor(m_pendingLevels.at(i), dark);
        if (color.isValid()) {
            format.setForeground(color);
        }
        cursor.insertText(m_pendingTexts.at(i) + QLatin1Char('\n'), format);
    }
    m_logView->setUpdatesEnabled(true);

    m_pendingTexts.clear();
    m_pendingLevels.clear();

    /* Follow new output only when the user was already at the end */
    if (follow || wasAtBottom) {
        m_logView->moveCursor(QTextCursor::End);
        m_logView->ensureCursorVisible();
    }
}

void MainWindow::saveLogToFile()
{
    flushLogView();
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    const QString suggested =
        dir + QStringLiteral("/minieap-log-%1.txt")
                  .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss")));

    const QString path = QFileDialog::getSaveFileName(this, tr("保存日志"), suggested,
                                                      tr("文本文件 (*.txt *.log)"));
    if (path.isEmpty()) {
        return;
    }
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QMessageBox::warning(this, tr("保存失败"), file.errorString());
        return;
    }
    file.write(m_logView->toPlainText().toUtf8());
    statusBar()->showMessage(tr("日志已保存到 %1").arg(path), 6000);
}

void MainWindow::copyLog()
{
    flushLogView();
    if (m_logView->textCursor().hasSelection()) {
        m_logView->copy();
        return;
    }
    QApplication::clipboard()->setText(m_logView->toPlainText());
    statusBar()->showMessage(tr("已复制全部日志"), 3000);
}

void MainWindow::clearLog()
{
    m_pendingTexts.clear();
    m_pendingLevels.clear();
    m_logView->clear();
}

/* -------------------------------------------------------------- the rest */

void MainWindow::showAbout()
{
    QMessageBox::about(this, tr("关于 MiniEAP"),
                       tr("<h3>MiniEAP %1</h3>"
                          "<p>可扩展的 802.1x 认证客户端，附带锐捷 v3 (v4) 算法插件。</p>"
                          "<p>认证引擎作者：Hamster Tian (updateing) 等<br>"
                          "图形界面：基于 Qt 6 的跨平台前端</p>"
                          "<p>配置文件保存在：<br>%2</p>")
                           .arg(QStringLiteral(MINIEAP_GUI_VERSION_STR), ProfileStore::filePath()));
}

void MainWindow::restoreSettings()
{
    QSettings settings;
    restoreGeometry(settings.value(QStringLiteral("window/geometry")).toByteArray());
    if (settings.contains(QStringLiteral("window/autoScroll"))) {
        m_autoScroll->setChecked(settings.value(QStringLiteral("window/autoScroll")).toBool());
    }
}

void MainWindow::saveSettings()
{
    QSettings settings;
    settings.setValue(QStringLiteral("window/geometry"), saveGeometry());
    settings.setValue(QStringLiteral("window/autoScroll"), m_autoScroll->isChecked());
}

void MainWindow::shutdown()
{
    saveSettings();
    saveProfiles();
    if (m_engine->isRunning()) {
        m_engine->shutdown();
    }
}

QWidget* MainWindow::createProfileDialogForTesting()
{
    return new ProfileDialog(currentProfile(), this);
}

void MainWindow::startWithProfile(const Profile& profile)
{
    /* Keep the UI in sync with what is about to run */
    QList<Profile> profiles = m_store.profiles();
    if (profiles.isEmpty()) {
        profiles << profile;
    } else {
        profiles[0] = profile;
    }
    m_store.setProfiles(profiles);
    reloadProfileCombo(0);
    selectProfile(0);

    connectOrDisconnect();
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    if (m_engine->isRunning()) {
        const auto answer = QMessageBox::question(this, tr("正在认证"),
                                                  tr("认证仍在进行，确定要断开并退出吗？"),
                                                  QMessageBox::Yes | QMessageBox::No,
                                                  QMessageBox::No);
        if (answer != QMessageBox::Yes) {
            event->ignore();
            return;
        }
    }
    flushLogView();
    shutdown();
    QMainWindow::closeEvent(event);
}
