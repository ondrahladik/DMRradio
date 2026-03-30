#include "MainWindow.h"
#include "HotspotManager.h"
#include "AudioEngine.h"
#include "AmbeDecoder.h"
#include "ConfigManager.h"
#include "Hotspot.h"

#include <QGroupBox>
#include <QFrame>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QScrollArea>
#include <QStyle>
#include <QIntValidator>
#include <QCoreApplication>
#include <QFile>
#include <QMessageBox>
#include <QRegularExpression>
#include <QTextStream>
#include <QTime>
#include <QDebug>
#include <QAudioDevice>

namespace {

QHash<quint32, QPair<QString, QString>> loadDmrLookup(const QString &filePath)
{
    QHash<quint32, QPair<QString, QString>> lookup;

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return lookup;

    QTextStream in(&file);
    while (!in.atEnd()) {
        const QString line = in.readLine().trimmed();
        if (line.isEmpty())
            continue;

        QStringList parts = line.split('\t', Qt::KeepEmptyParts);
        if (parts.size() < 3)
            parts = line.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);

        if (parts.size() < 3)
            continue;

        bool ok = false;
        const quint32 dmrId = parts.value(0).toUInt(&ok);
        if (!ok)
            continue;

        const QString callsign = parts.value(1).trimmed();
        const QString name = parts.mid(2).join(" ").trimmed();
        if (callsign.isEmpty() || name.isEmpty())
            continue;

        lookup.insert(dmrId, qMakePair(callsign, name));
    }

    return lookup;
}

} // namespace

// ──────────────────────────────────────────────
//  Constructor / Destructor
// ──────────────────────────────────────────────

MainWindow::MainWindow(HotspotManager *manager, AudioEngine *audio,
                       ConfigManager *config, QWidget *parent)
    : QMainWindow(parent)
    , m_manager(manager)
    , m_audio(audio)
    , m_configMgr(config)
    , m_decoder(new AmbeDecoder())
{
    setWindowTitle("DMR radio");
    setFixedWidth(340);
    setFixedHeight(480);
    resize(340, 480);
    buildUi();
    loadDmrIds();

    // Wire manager signals
    connect(m_manager, &HotspotManager::logMessage, this, &MainWindow::addLog);
    connect(m_manager, &HotspotManager::pttChanged, this, &MainWindow::onPttChanged);
    connect(m_audio, &AudioEngine::logMessage, this, &MainWindow::addLog);

    // Forward captured audio to the active transmitting hotspot
    connect(m_audio, &AudioEngine::pcmCaptured, this, [this](const QByteArray &data) {
        int txIdx = m_manager->activeTxIndex();
        if (txIdx >= 0) {
            Hotspot *hs = m_manager->hotspot(txIdx);
            if (hs)
                hs->sendAudioData(data);
        }
    });

    // RX audio chain + caller display wiring
    for (int i = 0; i < m_manager->count(); ++i) {
        Hotspot *hs = m_manager->hotspot(i);
        connect(hs, &Hotspot::audioDataReceived, this, [this](const QByteArray &ambe) {
            QByteArray pcm = m_decoder->decode(ambe);
            if (!pcm.isEmpty())
                m_audio->playPCM(pcm);
        });
        connect(hs, &Hotspot::voiceStreamEnded, this, [this, i, hs]() {
            m_decoder->reset();
            m_audio->resetPlayback();
            addLog(QString("[%1] Decoder reset (stream boundary)").arg(hs->name()));
            onVoiceCallEnded(i);
        });
        connect(hs, &Hotspot::voiceStreamStarted, this, [this, i](quint32 srcId, quint32 dstId) {
            onVoiceCallStarted(i, srcId);
            if (m_targetLabel)
                m_targetLabel->setText(QString("TG %1").arg(dstId));
        });

        // Initialize TX TG from config
        int cfgIdx = hs->configIndex();
        int txTg = m_configMgr->hotspotTxTg(cfgIdx);
        if (txTg > 0)
            hs->setTxTalkgroup(txTg);
    }

    loadSettingsToUi();
}

MainWindow::~MainWindow()
{
    delete m_decoder;
}

// ──────────────────────────────────────────────
//  UI construction
// ──────────────────────────────────────────────

void MainWindow::buildUi()
{
    auto *central = new QWidget(this);
    auto *mainLayout = new QVBoxLayout(central);
    mainLayout->setContentsMargins(6, 6, 6, 6);
    mainLayout->setSpacing(4);

    // Minimal top tab bar
    auto *navLayout = new QHBoxLayout();
    navLayout->setSpacing(2);

    m_navHotspots = new QPushButton("Hotspots");
    m_navSettings = new QPushButton("Settings");
    m_navLog      = new QPushButton("Log");
    m_navAbout    = new QPushButton("About");

    const QString navStyle =
        "QPushButton { background: transparent; color: #bdbdbd; border: none; "
        "padding: 4px 2px; font-size: 9pt; }"
        "QPushButton:hover { color: #ffffff; }"
        "QPushButton[navActive=\"true\"] { color: #4fc3f7; border-bottom: 2px solid #4fc3f7; }";

    for (auto *btn : {m_navHotspots, m_navSettings, m_navLog, m_navAbout}) {
        btn->setFixedHeight(26);
        btn->setFlat(true);
        btn->setCursor(Qt::PointingHandCursor);
        btn->setStyleSheet(navStyle);
        navLayout->addWidget(btn, 1);
    }
    mainLayout->addLayout(navLayout);

    // Stacked pages: 0=Hotspots, 1=Settings, 2=Log, 3=About
    m_stack = new QStackedWidget(central);
    m_stack->addWidget(createHotspotsPage());
    m_stack->addWidget(createSettingsPage());
    m_stack->addWidget(createLogPage());
    m_stack->addWidget(createAboutPage());
    mainLayout->addWidget(m_stack);

    setCentralWidget(central);

    connect(m_navHotspots, &QPushButton::clicked, this, [this]() { switchPage(0); });
    connect(m_navSettings, &QPushButton::clicked, this, [this]() { switchPage(1); });
    connect(m_navLog,      &QPushButton::clicked, this, [this]() { switchPage(2); });
    connect(m_navAbout,    &QPushButton::clicked, this, [this]() { switchPage(3); });

    switchPage(0);
}

void MainWindow::switchPage(int index)
{
    m_stack->setCurrentIndex(index);

    m_navHotspots->setProperty("navActive",  index == 0);
    m_navSettings->setProperty("navActive",  index == 1);
    m_navLog->setProperty("navActive",       index == 2);
    m_navAbout->setProperty("navActive",     index == 3);

    for (auto *btn : {m_navHotspots, m_navSettings, m_navLog, m_navAbout}) {
        btn->style()->unpolish(btn);
        btn->style()->polish(btn);
    }
}

// ── Page 0: Hotspots ──

QWidget *MainWindow::createHotspotsPage()
{
    auto *page = new QWidget();
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(6);

    // Hotspot rows: [● Name] [TG:spin] [CONNECT] [PTT]
    for (int i = 0; i < m_manager->count(); ++i) {
        Hotspot *hs = m_manager->hotspot(i);
        HotspotRow row = createHotspotRow(i, hs);
        m_rows.append(row);

        auto *rowLayout = new QHBoxLayout();
        rowLayout->setSpacing(4);
        rowLayout->addWidget(row.dotLabel);
        rowLayout->addWidget(row.nameLabel, 1);
        rowLayout->addWidget(row.txTgSpin);
        rowLayout->addWidget(row.connectBtn);
        rowLayout->addWidget(row.pttBtn);
        layout->addLayout(rowLayout);

        if (i < m_manager->count() - 1) {
            auto *line = new QFrame();
            line->setFrameShape(QFrame::HLine);
            line->setFrameShadow(QFrame::Sunken);
            line->setObjectName("separator");
            layout->addWidget(line);
        }
    }

    layout->addStretch();

    // Caller / target panel (always visible, above PTT)
    auto *panel = new QFrame();
    panel->setFrameShape(QFrame::StyledPanel);
    panel->setStyleSheet(
        "QFrame { background-color: #1b1b1b; border: none; }");

    auto *panelLayout = new QGridLayout(panel);
    panelLayout->setContentsMargins(8, 6, 8, 6);
    panelLayout->setHorizontalSpacing(8);
    panelLayout->setVerticalSpacing(4);

    auto addInfoRow = [panelLayout](int row, const QString &titleText, QLabel *&valueLabel,
                                    const QString &valueColor, int fontSize) {
        auto *title = new QLabel(titleText);
        title->setStyleSheet("QLabel { color: #9e9e9e; font-size: 8pt; font-weight: bold; }");
        panelLayout->addWidget(title, row, 0);

        valueLabel = new QLabel("");
        valueLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        valueLabel->setStyleSheet(QString("QLabel { color: %1; font-size: %2pt; font-weight: bold; }")
                                      .arg(valueColor)
                                      .arg(fontSize));
        valueLabel->setMinimumHeight(22);
        panelLayout->addWidget(valueLabel, row, 1);
    };

    addInfoRow(0, "Callsign:", m_callerCallsignLabel, "#3fc3f7", 11);
    addInfoRow(1, "Name:", m_callerNameLabel, "#3fc3f7", 10);
    addInfoRow(2, "DMR ID:", m_callerLabel, "#3fc3f7", 12);

    auto *targetTitle = new QLabel("Target:");
    targetTitle->setStyleSheet("QLabel { color: #9e9e9e; font-size: 8pt; font-weight: bold; }");
    panelLayout->addWidget(targetTitle, 3, 0);

    m_targetLabel = new QLabel("");
    m_targetLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_targetLabel->setStyleSheet(
        "QLabel { color: #3fc3f7; font-size: 10pt; font-weight: bold; }");
    m_targetLabel->setMinimumHeight(22);
    panelLayout->addWidget(m_targetLabel, 3, 1);

    panelLayout->setColumnStretch(0, 0);
    panelLayout->setColumnStretch(1, 1);

    layout->addWidget(panel);

    // Main PTT button at the bottom
    m_mainPttBtn = new QPushButton("PTT");
    m_mainPttBtn->setMinimumHeight(60);
    m_mainPttBtn->setEnabled(false);
    m_mainPttBtn->setStyleSheet(
        "QPushButton { background-color: #2d2d2d; color: #d4d4d4; font-size: 16pt; "
        "font-weight: bold; border: 2px solid #3d3d3d; border-radius: 8px; }"
        "QPushButton:hover { background-color: #3a3a3a; border-color: #569cd6; }"
        "QPushButton:pressed { background-color: #b71c1c; border-color: #ef5350; color: #ffffff; }"
        "QPushButton:disabled { color: #555555; background-color: #252525; border-color: #303030; }");

    connect(m_mainPttBtn, &QPushButton::pressed, this, &MainWindow::onMainPttPressed);
    connect(m_mainPttBtn, &QPushButton::released, this, &MainWindow::onMainPttReleased);

    layout->addWidget(m_mainPttBtn);

    return page;
}

QWidget *MainWindow::createAboutPage()
{
    auto *page = new QWidget();
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(10, 10, 10, 10);
    layout->setSpacing(10);

    auto *title = new QLabel("DMR radio");
    QFont titleFont = title->font();
    titleFont.setPointSize(18);
    titleFont.setBold(true);
    title->setFont(titleFont);
    title->setAlignment(Qt::AlignCenter);
    title->setStyleSheet("QLabel { color: #4fc3f7; }");
    layout->addWidget(title);

    const QString versionText = QCoreApplication::applicationVersion().isEmpty()
        ? QStringLiteral("1.0.2")
        : QCoreApplication::applicationVersion();
    auto *version = new QLabel(QString("Version: %1").arg(versionText));
    version->setAlignment(Qt::AlignCenter);
    version->setStyleSheet("QLabel { color: #bdbdbd; }");
    layout->addWidget(version);

    auto *copyright = new QLabel(QString::fromUtf8("© 2026 OK1KKY"));
    copyright->setAlignment(Qt::AlignCenter);
    copyright->setStyleSheet("QLabel { color: #cfcfcf; }");
    layout->addWidget(copyright);

    auto *projectPage = new QLabel("<a href=\"https://github.com/ondrahladik/DMRradio\">github.com/ondrahladik/DMRradio</a>");
    projectPage->setAlignment(Qt::AlignCenter);
    projectPage->setTextFormat(Qt::RichText);
    projectPage->setTextInteractionFlags(Qt::TextBrowserInteraction);
    projectPage->setOpenExternalLinks(true);
    projectPage->setStyleSheet("QLabel { color: #4fc3f7; }");
    layout->addWidget(projectPage);

    auto *web = new QLabel("<a href=\"https://www.ok1kky.cz\">www.ok1kky.cz</a>");
    web->setAlignment(Qt::AlignCenter);
    web->setTextFormat(Qt::RichText);
    web->setTextInteractionFlags(Qt::TextBrowserInteraction);
    web->setOpenExternalLinks(true);
    web->setStyleSheet("QLabel { color: #4fc3f7; }");
    layout->addWidget(web);

    layout->addStretch();
    return page;
}

// ── Page 1: Settings ──

QWidget *MainWindow::createSettingsPage()
{
    auto *page = new QWidget();
    auto *outerLayout = new QVBoxLayout(page);
    outerLayout->setContentsMargins(0, 0, 0, 0);

    auto *scrollArea = new QScrollArea();
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);
    scrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scrollArea->setMinimumWidth(340);
    scrollArea->setMaximumWidth(340);

    auto *scrollContent = new QWidget();
    scrollContent->setMinimumWidth(332);
    scrollContent->setMaximumWidth(332);
    auto *layout = new QVBoxLayout(scrollContent);
    layout->setSpacing(10);
    layout->setContentsMargins(4, 4, 4, 4);

    const QString sectionStyle =
        "QGroupBox {"
        "  border: 1px solid #4a4a4a;"
        "  border-radius: 6px;"
        "  margin-top: 10px;"
        "  padding-top: 12px;"
        "}"
        "QGroupBox::title {"
        "  subcontrol-origin: margin;"
        "  left: 8px;"
        "  padding: 0 3px;"
        "}";

    const int minH = 28;

    // ── Server group ──
    auto *serverGroup = new QGroupBox("Server", scrollContent);
    serverGroup->setStyleSheet(sectionStyle);
    auto *serverForm = new QFormLayout(serverGroup);
    serverForm->setSpacing(6);

    m_settHost = new QLineEdit();
    m_settHost->setMinimumHeight(minH);
    m_settHost->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_settPort = new QSpinBox();
    m_settPort->setRange(1, 65535);
    m_settPort->setMinimumHeight(minH);
    m_settPassword = new QLineEdit();
    m_settPassword->setEchoMode(QLineEdit::Password);
    m_settPassword->setMinimumHeight(minH);
    m_settPassword->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    serverForm->addRow("Host:", m_settHost);
    serverForm->addRow("Port:", m_settPort);
    serverForm->addRow("Password:", m_settPassword);
    layout->addWidget(serverGroup);

    // ── Station group ──
    auto *stationGroup = new QGroupBox("Station", scrollContent);
    stationGroup->setStyleSheet(sectionStyle);
    auto *stationForm = new QFormLayout(stationGroup);
    stationForm->setSpacing(6);

    m_settCallsign = new QLineEdit();
    m_settCallsign->setMinimumHeight(minH);
    m_settCallsign->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_settDmrId = new QLineEdit();
    m_settDmrId->setValidator(new QIntValidator(1, 999999999, m_settDmrId));
    m_settDmrId->setMinimumHeight(minH);
    m_settDmrId->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    stationForm->addRow("Callsign:", m_settCallsign);
    stationForm->addRow("DMR ID:", m_settDmrId);
    layout->addWidget(stationGroup);

    // ── Audio group ──
    auto *audioGroup = new QGroupBox("Audio", scrollContent);
    audioGroup->setStyleSheet(sectionStyle);
    auto *audioGrid = new QGridLayout(audioGroup);
    audioGrid->setHorizontalSpacing(6);
    audioGrid->setVerticalSpacing(6);

    m_settInputDevice = new QComboBox();
    m_settInputDevice->setMinimumHeight(minH);
    m_settInputDevice->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_settInputDevice->addItem("(Default)");
    for (const QAudioDevice &dev : AudioEngine::availableInputDevices())
        m_settInputDevice->addItem(dev.description());

    m_settOutputDevice = new QComboBox();
    m_settOutputDevice->setMinimumHeight(minH);
    m_settOutputDevice->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_settOutputDevice->addItem("(Default)");
    for (const QAudioDevice &dev : AudioEngine::availableOutputDevices())
        m_settOutputDevice->addItem(dev.description());

    auto *inputLabel = new QLabel("Input:");
    inputLabel->setMinimumHeight(minH);
    inputLabel->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
    audioGrid->addWidget(inputLabel, 0, 0);
    audioGrid->addWidget(m_settInputDevice, 0, 1);

    auto *outputLabel = new QLabel("Output:");
    outputLabel->setMinimumHeight(minH);
    outputLabel->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
    audioGrid->addWidget(outputLabel, 1, 0);
    audioGrid->addWidget(m_settOutputDevice, 1, 1);
    audioGrid->setColumnStretch(1, 1);
    layout->addWidget(audioGroup);

    // ── Hotspots group ──
    auto *tgGroup = new QGroupBox("Hotspots", scrollContent);
    tgGroup->setStyleSheet(sectionStyle);
    auto *tgLayout = new QVBoxLayout(tgGroup);
    tgLayout->setSpacing(8);

    m_mainGroup = new QButtonGroup(this);

    int hsCount = m_configMgr ? m_configMgr->hotspotCount() : 0;
    for (int i = 0; i < hsCount; ++i) {
        TgRow tr;

        // Row 1: ON + RX checkboxes
        auto *flagsRow = new QHBoxLayout();
        flagsRow->setSpacing(6);

        tr.enabled = new QCheckBox("ON");
        tr.rxEnabled = new QCheckBox("RX");
        tr.isMain = new QRadioButton("Main");
        m_mainGroup->addButton(tr.isMain, i);

        flagsRow->addWidget(tr.enabled);
        flagsRow->addWidget(tr.rxEnabled);
        flagsRow->addWidget(tr.isMain);
        flagsRow->addStretch();

        // Row 2: Name + Suffix on one line
        auto *nameRow = new QHBoxLayout();
        nameRow->setSpacing(4);

        tr.name = new QLineEdit();
        tr.name->setMinimumHeight(minH);
        tr.name->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        tr.name->setPlaceholderText("Hotspot Name");

        tr.suffix = new QSpinBox();
        tr.suffix->setRange(1, 99);
        tr.suffix->setMinimumHeight(minH);
        tr.suffix->setMinimumWidth(48);
        tr.suffix->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);

        nameRow->addWidget(tr.name, 1);
        nameRow->addWidget(tr.suffix, 0);

        // Row 3: Options on its own line
        auto *optForm = new QFormLayout();
        optForm->setSpacing(4);
        tr.options = new QLineEdit();
        tr.options->setMinimumHeight(minH);
        tr.options->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        tr.options->setPlaceholderText("DMR+ Options");
        optForm->addRow(tr.options);

        auto *hsBox = new QVBoxLayout();
        hsBox->setSpacing(4);
        hsBox->addLayout(flagsRow);
        hsBox->addLayout(nameRow);
        hsBox->addLayout(optForm);

        tgLayout->addLayout(hsBox);

        if (i < hsCount - 1) {
            auto *sep = new QFrame();
            sep->setFrameShape(QFrame::HLine);
            sep->setFrameShadow(QFrame::Sunken);
            tgLayout->addWidget(sep);
        }

        m_settTgRows.append(tr);
    }

    layout->addWidget(tgGroup);

    // Save button
    auto *saveBtn = new QPushButton("Save");
    saveBtn->setMinimumWidth(100);
    saveBtn->setFixedHeight(34);
    saveBtn->setStyleSheet(
        "QPushButton { background-color: #2e7d32; color: white; font-weight: bold; }"
        "QPushButton:hover { background-color: #388e3c; }"
        "QPushButton:pressed { background-color: #1b5e20; }");
    connect(saveBtn, &QPushButton::clicked, this, &MainWindow::saveSettings);

    auto *btnLayout = new QHBoxLayout();
    btnLayout->addStretch();
    btnLayout->addWidget(saveBtn);
    btnLayout->addStretch();
    layout->addLayout(btnLayout);

    layout->addStretch();
    scrollArea->setWidget(scrollContent);
    outerLayout->addWidget(scrollArea);

    return page;
}

// ── Page 2: Log ──

QWidget *MainWindow::createLogPage()
{
    auto *page = new QWidget();
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(4, 4, 4, 4);

    m_logView = new QPlainTextEdit(page);
    m_logView->setReadOnly(true);
    m_logView->setMaximumBlockCount(5000);
    layout->addWidget(m_logView);

    return page;
}

// ──────────────────────────────────────────────
//  Hotspot row widget factory
// ──────────────────────────────────────────────

HotspotRow MainWindow::createHotspotRow(int index, Hotspot *hs)
{
    HotspotRow row;
    row.configIndex = hs->configIndex();

    // Status dot
    row.dotLabel = new QLabel();
    row.dotLabel->setFixedSize(14, 14);
    row.dotLabel->setStyleSheet(
        "background-color: #cc3333; border-radius: 7px; border: none;");

    // Name
    row.nameLabel = new QLabel(hs->name());
    row.nameLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    QFont boldFont = row.nameLabel->font();
    boldFont.setBold(true);
    row.nameLabel->setFont(boldFont);

    // TX TG
    row.txTgSpin = new QSpinBox();
    row.txTgSpin->setRange(1, 999999);
    row.txTgSpin->setMinimumWidth(65);
    row.txTgSpin->setFixedHeight(28);
    row.txTgSpin->setPrefix("TG ");
    row.txTgSpin->setValue(hs->txTalkgroup());

    connect(row.txTgSpin, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [hs](int val) { hs->setTxTalkgroup(val); });

    // 🔥 CONNECT BUTTON (SVG)
    row.connectBtn = new QPushButton();
    row.connectBtn->setFixedSize(36, 28);
    row.connectBtn->setIcon(QIcon(":/icons/connect.png"));
    row.connectBtn->setIconSize(QSize(24, 24));
    row.connectBtn->setCursor(Qt::PointingHandCursor);
    row.connectBtn->setToolTip("Connect / Disconnect");

    row.connectBtn->setStyleSheet(
        "QPushButton { background-color: #2d2d2d; border: 1px solid #3d3d3d; border-radius: 4px; }"
        "QPushButton:hover { background-color: #3a3a3a; border-color: #569cd6; }"
        "QPushButton:pressed { background-color: #1f1f1f; }"
        );

    connect(row.connectBtn, &QPushButton::clicked, this, [this, index]() {
        onConnectClicked(index);
    });

    // 🔴 PTT BUTTON
    row.pttBtn = new QPushButton("PTT");
    row.pttBtn->setFixedWidth(50);
    row.pttBtn->setFixedHeight(28);
    row.pttBtn->setEnabled(false);

    row.pttBtn->setStyleSheet(
        "QPushButton { background-color: #2d2d2d; color: #d4d4d4; font-weight: bold; "
        "border: 1px solid #3d3d3d; border-radius: 4px; }"
        "QPushButton:hover { background-color: #3a3a3a; border-color: #569cd6; }"
        "QPushButton:pressed { background-color: #b71c1c; border-color: #ef5350; color: #fff; }"
        "QPushButton:disabled { color: #555555; background-color: #252525; border-color: #303030; }"
        );

    connect(row.pttBtn, &QPushButton::pressed, this, [this, index]() {
        onHsPttPressed(index);
    });

    connect(row.pttBtn, &QPushButton::released, this, [this, index]() {
        onHsPttReleased(index);
    });

    connect(hs, &Hotspot::stateChanged, this, [this, index]() {
        onHotspotStateChanged(index);
    });

    return row;
}

// ──────────────────────────────────────────────
//  Slot implementations
// ──────────────────────────────────────────────

void MainWindow::onConnectClicked(int index)
{
    Hotspot *hs = m_manager->hotspot(index);
    if (!hs) return;

    if (hs->state() != Hotspot::State::Disconnected)
        hs->disconnectFromServer();
    else
        hs->connectToServer();
}

void MainWindow::onHsPttPressed(int index)
{
    if (m_manager->requestPtt(index))
        m_audio->startCapture();
}

void MainWindow::onHsPttReleased(int index)
{
    m_audio->stopCapture();
    m_manager->releasePtt(index);
}

void MainWindow::onMainPttPressed()
{
    int mainIdx = m_manager->mainHotspotIndex();
    if (mainIdx < 0) return;

    if (m_manager->requestPtt(mainIdx))
        m_audio->startCapture();
}

void MainWindow::onMainPttReleased()
{
    int mainIdx = m_manager->mainHotspotIndex();
    if (mainIdx < 0) return;

    m_audio->stopCapture();
    m_manager->releasePtt(mainIdx);
}

void MainWindow::onHotspotStateChanged(int index)
{
    updateRowState(index);
    updateMainPttState();
}

void MainWindow::onPttChanged(int /*index*/, bool active)
{
    if (m_mainPttBtn)
        m_mainPttBtn->setProperty("txActive", active);
    updateMainPttState();
}

void MainWindow::onVoiceCallStarted(int /*index*/, quint32 srcId)
{
    if (m_callerLabel)
        m_callerLabel->setText(QString::number(srcId));
    if (m_callerCallsignLabel)
        m_callerCallsignLabel->setText("-");
    if (m_callerNameLabel)
        m_callerNameLabel->setText("-");

    const auto it = m_dmrLookup.constFind(srcId);
    if (it != m_dmrLookup.constEnd()) {
        if (m_callerCallsignLabel)
            m_callerCallsignLabel->setText(it.value().first);
        if (m_callerNameLabel)
            m_callerNameLabel->setText(it.value().second);
    }
}

void MainWindow::onVoiceCallEnded(int /*index*/)
{
    if (m_callerLabel)
        m_callerLabel->setText("");
    if (m_callerCallsignLabel)
        m_callerCallsignLabel->setText("");
    if (m_callerNameLabel)
        m_callerNameLabel->setText("");
    if (m_targetLabel)
        m_targetLabel->setText("");
}

void MainWindow::updateRowState(int index)
{
    if (index < 0 || index >= m_rows.size())
        return;

    Hotspot *hs = m_manager->hotspot(index);
    HotspotRow &row = m_rows[index];

    bool connected = false;

    switch (hs->state()) {

    case Hotspot::State::Disconnected:
        row.dotLabel->setStyleSheet(
            "background-color: #cc3333; border-radius: 7px;");
        row.connectBtn->setIcon(QIcon(":/icons/connect.png"));
        break;

    case Hotspot::State::Connected:
        row.dotLabel->setStyleSheet(
            "background-color: #33cc33; border-radius: 7px;");
        row.connectBtn->setIcon(QIcon(":/icons/disconnect.png"));
        connected = true;
        break;

    default:
        row.dotLabel->setStyleSheet(
            "background-color: #cccc33; border-radius: 7px;");
        row.connectBtn->setIcon(QIcon(":/icons/abort.png"));
        break;
    }

    row.pttBtn->setEnabled(connected);
}

void MainWindow::updateMainPttState()
{
    if (!m_mainPttBtn) return;

    int mainIdx = m_manager->mainHotspotIndex();
    if (mainIdx < 0) {
        m_mainPttBtn->setEnabled(false);
        return;
    }
    Hotspot *mainHs = m_manager->hotspot(mainIdx);
    m_mainPttBtn->setEnabled(mainHs && mainHs->isConnected());
}

// ──────────────────────────────────────────────
//  Logging (with timestamp)
// ──────────────────────────────────────────────

void MainWindow::addLog(const QString &msg)
{
    QString timestamp = QTime::currentTime().toString("HH:mm:ss");
    QString line = QString("[%1] %2").arg(timestamp, msg);
    qDebug().noquote() << line;
    if (m_logView)
        m_logView->appendPlainText(line);
}

// ──────────────────────────────────────────────
//  Settings page
// ──────────────────────────────────────────────

void MainWindow::loadSettingsToUi()
{
    if (!m_configMgr)
        return;

    m_settHost->setText(m_configMgr->host());
    m_settPort->setValue(m_configMgr->port());
    m_settPassword->setText(m_configMgr->password());
    m_settCallsign->setText(m_configMgr->callsign());
    m_settDmrId->setText(QString::number(m_configMgr->dmrId()));

    // Select saved input device in combo
    QString savedDev = m_configMgr->inputDevice();
    if (savedDev.isEmpty()) {
        m_settInputDevice->setCurrentIndex(0);
    } else {
        int idx = m_settInputDevice->findText(savedDev);
        m_settInputDevice->setCurrentIndex(idx >= 0 ? idx : 0);
    }

    QString savedOutDev = m_configMgr->outputDevice();
    if (savedOutDev.isEmpty()) {
        m_settOutputDevice->setCurrentIndex(0);
    } else {
        int idx = m_settOutputDevice->findText(savedOutDev);
        m_settOutputDevice->setCurrentIndex(idx >= 0 ? idx : 0);
    }

    for (int i = 0; i < m_settTgRows.size() && i < m_configMgr->hotspotCount(); ++i) {
        m_settTgRows[i].name->setText(m_configMgr->hotspotName(i));
        m_settTgRows[i].suffix->setValue(m_configMgr->hotspotSuffix(i));
        m_settTgRows[i].options->setText(m_configMgr->hotspotOptions(i));
        m_settTgRows[i].enabled->setChecked(m_configMgr->hotspotEnabled(i));
        m_settTgRows[i].isMain->setChecked(m_configMgr->hotspotIsMain(i));
        m_settTgRows[i].rxEnabled->setChecked(m_configMgr->hotspotRxEnabled(i));
    }
}

void MainWindow::loadDmrIds()
{
    const QString dmrIdsPath = QCoreApplication::applicationDirPath() + "/DMRIds.dat";
    m_dmrLookup = loadDmrLookup(dmrIdsPath);
}

void MainWindow::saveSettings()
{
    if (!m_configMgr)
        return;

    m_configMgr->setHost(m_settHost->text());
    m_configMgr->setPort(static_cast<quint16>(m_settPort->value()));
    m_configMgr->setPassword(m_settPassword->text());
    m_configMgr->setCallsign(m_settCallsign->text());

    quint32 baseDmrId = m_settDmrId->text().toUInt();
    m_configMgr->setDmrId(baseDmrId);

    // Save input device (empty string = default)
    QString devText = m_settInputDevice->currentText();
    m_configMgr->setInputDevice(devText == "(Default)" ? QString() : devText);

    QString outDevText = m_settOutputDevice->currentText();
    m_configMgr->setOutputDevice(outDevText == "(Default)" ? QString() : outDevText);

    for (int i = 0; i < m_settTgRows.size() && i < m_configMgr->hotspotCount(); ++i) {
        m_configMgr->setHotspotName(i, m_settTgRows[i].name->text());
        m_configMgr->setHotspotSuffix(i, m_settTgRows[i].suffix->value());
        m_configMgr->setHotspotOptions(i, m_settTgRows[i].options->text());
        m_configMgr->setHotspotEnabled(i, m_settTgRows[i].enabled->isChecked());
        m_configMgr->setHotspotIsMain(i, m_settTgRows[i].isMain->isChecked());
        m_configMgr->setHotspotRxEnabled(i, m_settTgRows[i].rxEnabled->isChecked());
    }

    // Also save runtime TX TG values from the Hotspots page
    for (int i = 0; i < m_rows.size(); ++i) {
        int cfgIdx = m_rows[i].configIndex;
        if (cfgIdx >= 0 && m_rows[i].txTgSpin)
            m_configMgr->setHotspotTxTg(cfgIdx, m_rows[i].txTgSpin->value());
    }

    bool saved = m_configMgr->save();
    bool audioApplied = false;
    if (saved && m_audio) {
        audioApplied = m_audio->initialize(m_configMgr->inputDevice(), m_configMgr->outputDevice());
    }

    if (saved) {
        addLog("Settings saved to " + m_configMgr->configPath());
        if (audioApplied) {
            QMessageBox::information(this, "Settings",
                "Settings saved successfully.\nAudio input and output were updated.");
        } else {
            QMessageBox::information(this, "Settings",
                "Settings saved successfully.\nAudio settings were saved, but the selected devices could not be applied right now.");
        }
    } else {
        addLog("Error: Failed to save settings!");
        QMessageBox::warning(this, "Error", "Failed to save settings.");
    }
}
