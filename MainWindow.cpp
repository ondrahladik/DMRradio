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
#include <QDateTime>
#include <QFileInfo>
#include <QDir>
#include <QStandardPaths>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QDebug>
#include <QAudioDevice>
#include <QSlider>
#ifdef Q_OS_ANDROID
#include <QScroller>
#endif

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
#ifdef Q_OS_ANDROID
    // On Android let the window fill the screen; layout stays compact.
    setMinimumWidth(340);
#else
    setFixedWidth(340);
    setFixedHeight(480);
    resize(340, 480);
#endif
    buildUi();
    loadDmrIds();
    checkAndUpdateDmrIds();

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

    wireHotspotConnections();
    loadSettingsToUi();

    m_callerSyncTimer = new QTimer(this);
    m_callerSyncTimer->setInterval(2000);
    connect(m_callerSyncTimer, &QTimer::timeout, this, &MainWindow::syncCallerDisplay);
    m_callerSyncTimer->start();
}

MainWindow::~MainWindow()
{
    delete m_decoder;
}

void MainWindow::wireHotspotConnections()
{
    if (!m_manager)
        return;

    for (int i = 0; i < m_manager->count(); ++i) {
        Hotspot *hs = m_manager->hotspot(i);
        if (!hs)
            continue;

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

        const int cfgIdx = hs->configIndex();
        const int txTg = m_configMgr ? m_configMgr->hotspotTxTg(cfgIdx) : 0;
        hs->setTxTalkgroup(txTg);
        hs->setRxEnabled(m_configMgr ? m_configMgr->hotspotRxEnabled(cfgIdx) : true);
        updateRowState(i);
    }

    updateMainPttState();
}

void MainWindow::rebuildHotspotsPage()
{
    if (!m_stack)
        return;

    QWidget *oldPage = m_stack->widget(0);
    QWidget *currentPage = m_stack->currentWidget();

    m_rows.clear();

    QWidget *newPage = createHotspotsPage();
    m_stack->insertWidget(0, newPage);
    m_stack->removeWidget(oldPage);
    if (oldPage)
        oldPage->deleteLater();

    if (currentPage == oldPage)
        m_stack->setCurrentWidget(newPage);
    else if (currentPage)
        m_stack->setCurrentWidget(currentPage);

    updateMainPttState();
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

    QString navStyle =
        "QPushButton { background: transparent; color: #bdbdbd; border: none; "
        "padding: 4px 2px; font-size: 9pt; }";

    #ifdef Q_OS_ANDROID
        navStyle =
            "QPushButton { background: transparent; color: #bdbdbd; border: none; "
            "padding: 4px 2px; font-size: 16pt; }";
    #endif

        navStyle +=
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
#ifdef Q_OS_ANDROID
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(4);
#else
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(6);
#endif
    m_rows.clear();

    // Hotspot rows: [● Name] [TG:spin] [CONNECT] [PTT]
    for (int i = 0; i < m_manager->count(); ++i) {
        Hotspot *hs = m_manager->hotspot(i);
        HotspotRow row = createHotspotRow(i, hs);
        m_rows.append(row);

        auto *rowLayout = new QHBoxLayout();
#ifdef Q_OS_ANDROID
        rowLayout->setSpacing(6);
        rowLayout->setContentsMargins(0, 2, 0, 2);
#else
        rowLayout->setSpacing(4);
#endif
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



    // Caller / target panel (always visible, above PTT)
    auto *panel = new QFrame();
    panel->setFrameShape(QFrame::StyledPanel);
    panel->setStyleSheet(
        "QFrame { background-color: #1b1b1b; border: 1px solid #2f2f2f; border-radius: 6px; }");

    auto *panelLayout = new QGridLayout(panel);
    panelLayout->setContentsMargins(8, 6, 8, 6);
    panelLayout->setHorizontalSpacing(8);
    panelLayout->setVerticalSpacing(4);

    auto addInfoRow = [panelLayout](int row, const QString &titleText, QLabel *&valueLabel,
                                    const QString &valueColor, int fontSize) {
        auto *title = new QLabel(titleText);
#ifdef Q_OS_ANDROID
        title->setStyleSheet("QLabel { color: #9e9e9e; font-size: 18pt; font-weight: bold; border: none; background: transparent; }");
#else
        title->setStyleSheet("QLabel { color: #9e9e9e; font-size: 8pt; font-weight: bold; border: none; background: transparent; }");
#endif
        panelLayout->addWidget(title, row, 0);

        valueLabel = new QLabel("");
        valueLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
#ifdef Q_OS_ANDROID
        const int androidFontSize = fontSize + 6;
        valueLabel->setStyleSheet(QString("QLabel { color: %1; font-size: %2pt; font-weight: bold; border: none; background: transparent; }")
                                      .arg(valueColor).arg(androidFontSize));
        valueLabel->setMinimumHeight(30);
#else
        valueLabel->setStyleSheet(QString("QLabel { color: %1; font-size: %2pt; font-weight: bold; border: none; background: transparent; }")
                                      .arg(valueColor).arg(fontSize));
        valueLabel->setMinimumHeight(22);
#endif
        panelLayout->addWidget(valueLabel, row, 1);
    };

    addInfoRow(0, "Callsign:", m_callerCallsignLabel, "#3fc3f7", 11);
    addInfoRow(1, "Name:", m_callerNameLabel, "#3fc3f7", 10);
    addInfoRow(2, "DMR ID:", m_callerLabel, "#3fc3f7", 12);

    auto *targetTitle = new QLabel("Target:");
#ifdef Q_OS_ANDROID
    targetTitle->setStyleSheet("QLabel { color: #9e9e9e; font-size: 18pt; font-weight: bold; border: none; background: transparent; }");
#else
    targetTitle->setStyleSheet("QLabel { color: #9e9e9e; font-size: 8pt; font-weight: bold; border: none; background: transparent; }");
#endif
    panelLayout->addWidget(targetTitle, 3, 0);

    m_targetLabel = new QLabel("");
    m_targetLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
#ifdef Q_OS_ANDROID
    m_targetLabel->setStyleSheet(
        "QLabel { color: #3fc3f7; font-size: 16pt; font-weight: bold; border: none; background: transparent; }");
    m_targetLabel->setMinimumHeight(30);
#else
    m_targetLabel->setStyleSheet(
        "QLabel { color: #3fc3f7; font-size: 10pt; font-weight: bold; border: none; background: transparent; }");
    m_targetLabel->setMinimumHeight(22);
#endif
    panelLayout->addWidget(m_targetLabel, 3, 1);

    panelLayout->setColumnStretch(0, 0);
    panelLayout->setColumnStretch(1, 1);

    layout->addWidget(panel);

    // Audio level bargraph — full width, above volume control
    auto *levelFrame = new QFrame();
    levelFrame->setFrameShape(QFrame::StyledPanel);
    levelFrame->setStyleSheet(
        "QFrame { background-color: #1b1b1b; border: 1px solid #2f2f2f; border-radius: 6px; }");

    auto *levelLayout = new QHBoxLayout(levelFrame);
#ifdef Q_OS_ANDROID
    levelLayout->setContentsMargins(8, 2, 8, 2);
    levelLayout->setSpacing(6);
#else
    levelLayout->setContentsMargins(10, 8, 10, 8);
    levelLayout->setSpacing(8);
#endif

    auto *levelLabel = new QLabel("Audio");

    #ifdef Q_OS_ANDROID
        levelLabel->setStyleSheet(
            "QLabel { color: #9e9e9e; font-size: 12pt; font-weight: bold; border: none; background: transparent; }");
        levelLabel->setMinimumWidth(44);
    #else
        levelLabel->setStyleSheet(
        "QLabel { color: #9e9e9e; font-size: 8pt; font-weight: bold; border: none; background: transparent; }");
        levelLabel->setMinimumWidth(44);
    #endif


    m_audioLevelBar = new AudioLevelBar();

    levelLayout->addWidget(levelLabel);
    levelLayout->addWidget(m_audioLevelBar, 1);

    if (m_audio)
        connect(m_audio, &AudioEngine::audioLevelChanged, m_audioLevelBar, &AudioLevelBar::setLevel);

    layout->addWidget(levelFrame);

    // Full-width playback volume control
    auto *volumeFrame = new QFrame();
    volumeFrame->setFrameShape(QFrame::StyledPanel);
    volumeFrame->setStyleSheet(
        "QFrame { background-color: #1b1b1b; border: 1px solid #2f2f2f; border-radius: 6px; }");

    auto *volumeLayout = new QHBoxLayout(volumeFrame);
#ifdef Q_OS_ANDROID
    volumeLayout->setContentsMargins(8, 2, 8, 2);
    volumeLayout->setSpacing(6);
#else
    volumeLayout->setContentsMargins(10, 8, 10, 8);
    volumeLayout->setSpacing(8);
#endif

    auto *volumeLabel = new QLabel("Volume");

    #ifdef Q_OS_ANDROID
        volumeLabel->setStyleSheet("QLabel { color: #9e9e9e; font-size: 12pt; font-weight: bold; border: none; background: transparent; }");
    #else
        volumeLabel->setStyleSheet("QLabel { color: #9e9e9e; font-size: 8pt; font-weight: bold; border: none; background: transparent; }");
    #endif

    volumeLabel->setMinimumWidth(44);

    m_volumeSlider = new QSlider(Qt::Horizontal);
    m_volumeSlider->setRange(0, 100);
    m_volumeSlider->setSingleStep(1);
    m_volumeSlider->setPageStep(10);
    m_volumeSlider->setTickInterval(10);
    m_volumeSlider->setTickPosition(QSlider::NoTicks);
    m_volumeSlider->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
#ifdef Q_OS_ANDROID
    m_volumeSlider->setMinimumHeight(44);
    m_volumeSlider->setStyleSheet(
        "QSlider::groove:horizontal {"
        "  height: 12px;"
        "  background: #2b2b2b;"
        "  border: 1px solid #3d3d3d;"
        "  border-radius: 6px;"
        "}"
        "QSlider::sub-page:horizontal {"
        "  background: #569cd6;"
        "  border: 1px solid #569cd6;"
        "  border-radius: 6px;"
        "}"
        "QSlider::add-page:horizontal {"
        "  background: #2b2b2b;"
        "  border: 1px solid #3d3d3d;"
        "  border-radius: 6px;"
        "}"
        "QSlider::handle:horizontal {"
        "  background: #d4d4d4;"
        "  border: 1px solid #5a5a5a;"
        "  width: 32px;"
        "  height: 32px;"
        "  margin: -10px 0;"
        "  border-radius: 16px;"
        "}"
        "QSlider::handle:horizontal:hover {"
        "  background: #ffffff;"
        "  border-color: #569cd6;"
        "}");
#else
    m_volumeSlider->setMinimumHeight(24);
    m_volumeSlider->setStyleSheet(
        "QSlider::groove:horizontal {"
        "  height: 6px;"
        "  background: #2b2b2b;"
        "  border: 1px solid #3d3d3d;"
        "  border-radius: 3px;"
        "}"
        "QSlider::sub-page:horizontal {"
        "  background: #569cd6;"
        "  border: 1px solid #569cd6;"
        "  border-radius: 3px;"
        "}"
        "QSlider::add-page:horizontal {"
        "  background: #2b2b2b;"
        "  border: 1px solid #3d3d3d;"
        "  border-radius: 3px;"
        "}"
        "QSlider::handle:horizontal {"
        "  background: #d4d4d4;"
        "  border: 1px solid #5a5a5a;"
        "  width: 16px;"
        "  margin: -6px 0;"
        "  border-radius: 8px;"
        "}"
        "QSlider::handle:horizontal:hover {"
        "  background: #ffffff;"
        "  border-color: #569cd6;"
        "}");
#endif

    m_volumeValueLabel = new QLabel("100%");
    m_volumeValueLabel->setMinimumWidth(40);
    m_volumeValueLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    #ifdef Q_OS_ANDROID
        m_volumeValueLabel->setStyleSheet("QLabel { color: #3fc3f7; font-size: 12pt; font-weight: bold; border: none; background: transparent; }");
    #else
        m_volumeValueLabel->setStyleSheet("QLabel { color: #3fc3f7; font-size: 9pt; font-weight: bold; border: none; background: transparent; }");
    #endif

    connect(m_volumeSlider, &QSlider::valueChanged, this, [this](int value) {
        if (m_volumeValueLabel)
            m_volumeValueLabel->setText(QString::number(value) + "%");
        if (m_audio)
            m_audio->setPlaybackVolume(value);
    });

    m_volumeSlider->setValue(m_audio ? m_audio->playbackVolume() : 100);

    volumeLayout->addWidget(volumeLabel);
    volumeLayout->addWidget(m_volumeSlider, 1);
    volumeLayout->addWidget(m_volumeValueLabel);

    layout->addWidget(volumeFrame);

    // Main PTT button at the bottom
    m_mainPttBtn = new QPushButton("PTT");
#ifdef Q_OS_ANDROID
    m_mainPttBtn->setFixedHeight(80);
    m_mainPttBtn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
#else
    m_mainPttBtn->setMinimumHeight(60);
#endif
    m_mainPttBtn->setEnabled(false);

    QString pttStyle;

#ifdef Q_OS_ANDROID
    pttStyle = "QPushButton { font-size: 28pt; ";
#else
    pttStyle = "QPushButton { font-size: 16pt; ";
#endif

    pttStyle +=
        "background-color: #2d2d2d; color: #d4d4d4; "
        "font-weight: bold; border: 2px solid #3d3d3d; border-radius: 8px; }"
        "QPushButton:hover { background-color: #3a3a3a; border-color: #569cd6; }"
        "QPushButton:pressed { background-color: #b71c1c; border-color: #ef5350; color: #ffffff; }"
        "QPushButton:disabled { color: #555555; background-color: #252525; border-color: #303030; }";

    m_mainPttBtn->setStyleSheet(pttStyle);

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

    auto *aboutCard = new QFrame();
    aboutCard->setFrameShape(QFrame::StyledPanel);
    aboutCard->setStyleSheet(
        "QFrame { background-color: #1b1b1b; border: 1px solid #2f2f2f; border-radius: 6px; }");

    auto *aboutLayout = new QGridLayout(aboutCard);
    aboutLayout->setContentsMargins(10, 10, 10, 10);
    aboutLayout->setHorizontalSpacing(12);
    aboutLayout->setVerticalSpacing(8);

    auto *title = new QLabel("DMR radio");
    QFont titleFont = title->font();
    titleFont.setPointSize(18);
    titleFont.setBold(true);
    title->setFont(titleFont);
    title->setAlignment(Qt::AlignCenter);
    title->setStyleSheet("QLabel { color: #4fc3f7; border: none; background: transparent; }");
    aboutLayout->addWidget(title, 0, 0, 1, 2);

    const QString versionText = QCoreApplication::applicationVersion().isEmpty()
        ? QStringLiteral("1.0.3")
        : QCoreApplication::applicationVersion();

    auto addRow = [aboutLayout](int row, const QString &labelText, QLabel *valueLabel) {
        auto *label = new QLabel(labelText);
        label->setStyleSheet("QLabel { color: #9e9e9e; font-weight: bold; border: none; background: transparent; }");
        aboutLayout->addWidget(label, row, 0);

        valueLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        aboutLayout->addWidget(valueLabel, row, 1);
    };

    auto *version = new QLabel(QString("v%1").arg(versionText));
    version->setStyleSheet("QLabel { color: #bdbdbd; border: none; background: transparent; }");
    addRow(1, "Version", version);

    auto *author = new QLabel(QString::fromUtf8("© 2026 OK1KKY"));
    author->setStyleSheet("QLabel { color: #cfcfcf; border: none; background: transparent; }");
    addRow(2, "Author", author);

    auto *projectPage = new QLabel("<a href=\"https://github.com/ondrahladik/DMRradio\">github.com/ondrahladik/DMRradio</a>");
    projectPage->setTextFormat(Qt::RichText);
    projectPage->setTextInteractionFlags(Qt::TextBrowserInteraction);
    projectPage->setOpenExternalLinks(true);
    projectPage->setStyleSheet("QLabel { color: #4fc3f7; border: none; background: transparent; }");
    addRow(3, "Project", projectPage);

    auto *web = new QLabel("<a href=\"https://www.ok1kky.cz\">www.ok1kky.cz</a>");
    web->setTextFormat(Qt::RichText);
    web->setTextInteractionFlags(Qt::TextBrowserInteraction);
    web->setOpenExternalLinks(true);
    web->setStyleSheet("QLabel { color: #4fc3f7; border: none; background: transparent; }");
    addRow(4, "Website", web);

#ifndef Q_OS_ANDROID
    {
        const QString dmrPath = QCoreApplication::applicationDirPath() + "/DMRIds.dat";
        QFileInfo fi(dmrPath);
        QString dmrIdsDate = fi.exists() ? fi.lastModified().toString("yyyy-MM-dd") : QStringLiteral("(not found)");
        m_dmrIdsDateLabel = new QLabel(dmrIdsDate);
        m_dmrIdsDateLabel->setStyleSheet("QLabel { color: #bdbdbd; border: none; background: transparent; }");
        addRow(5, "ID update", m_dmrIdsDateLabel);
    }
#endif

    aboutLayout->setColumnStretch(0, 0);
    aboutLayout->setColumnStretch(1, 1);
    layout->addWidget(aboutCard);

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
#ifdef Q_OS_ANDROID
    // On Android: show vertical scrollbar and enable touch kinetic scrolling
    scrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    QScroller::grabGesture(scrollArea->viewport(), QScroller::LeftMouseButtonGesture);
    QScrollerProperties props = QScroller::scroller(scrollArea->viewport())->scrollerProperties();
    props.setScrollMetric(QScrollerProperties::DecelerationFactor, 0.3);
    QScroller::scroller(scrollArea->viewport())->setScrollerProperties(props);
#else
    scrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scrollArea->setMinimumWidth(340);
    scrollArea->setMaximumWidth(340);
#endif

    auto *scrollContent = new QWidget();
#ifdef Q_OS_ANDROID
    // Let content adapt to screen width
#else
    scrollContent->setMinimumWidth(332);
    scrollContent->setMaximumWidth(332);
#endif
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
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(0);

    auto *logCard = new QFrame();
    logCard->setFrameShape(QFrame::StyledPanel);
    logCard->setStyleSheet(
        "QFrame { background-color: #1b1b1b; border: 1px solid #2f2f2f; border-radius: 6px; }");

    auto *logLayout = new QVBoxLayout(logCard);
    logLayout->setContentsMargins(0, 0, 0, 0);

    m_logView = new QPlainTextEdit(logCard);
    m_logView->setReadOnly(true);
    m_logView->setMaximumBlockCount(5000);
    m_logView->setFrameShape(QFrame::NoFrame);
#ifdef Q_OS_ANDROID
    // Disable text selection on Android so finger drag = scroll, not text select
    m_logView->setTextInteractionFlags(Qt::NoTextInteraction);
    QScroller::grabGesture(m_logView->viewport(), QScroller::LeftMouseButtonGesture);
    QScrollerProperties logProps = QScroller::scroller(m_logView->viewport())->scrollerProperties();
    logProps.setScrollMetric(QScrollerProperties::DecelerationFactor, 0.3);
    QScroller::scroller(m_logView->viewport())->setScrollerProperties(logProps);
    m_logView->setStyleSheet(
        "QPlainTextEdit {"
        "  background-color: transparent;"
        "  color: #cccccc;"
        "  border: none;"
        "  padding: 8px;"
        "}"
        "QPlainTextEdit QScrollBar:vertical {"
        "  background: #2a2a2a;"
        "  width: 18px;"
        "  margin: 2px;"
        "  border-radius: 9px;"
        "}"
        "QPlainTextEdit QScrollBar::handle:vertical {"
        "  background: #5a5a5a;"
        "  min-height: 40px;"
        "  border-radius: 7px;"
        "  margin: 2px;"
        "}"
        "QPlainTextEdit QScrollBar::add-line:vertical,"
        "QPlainTextEdit QScrollBar::sub-line:vertical {"
        "  height: 0px; background: none; border: none;"
        "}"
        "QPlainTextEdit QScrollBar::add-page:vertical,"
        "QPlainTextEdit QScrollBar::sub-page:vertical {"
        "  background: transparent;"
        "}");
#else
    m_logView->setStyleSheet(
        "QPlainTextEdit {"
        "  background-color: transparent;"
        "  color: #cccccc;"
        "  border: none;"
        "  padding: 8px;"
        "  selection-background-color: #264f78;"
        "  selection-color: #ffffff;"
        "}"
        "QPlainTextEdit QScrollBar:vertical {"
        "  background: transparent;"
        "  width: 10px;"
        "  margin: 4px 2px 4px 2px;"
        "}"
        "QPlainTextEdit QScrollBar::handle:vertical {"
        "  background: #4a4a4a;"
        "  min-height: 24px;"
        "  border-radius: 5px;"
        "}"
        "QPlainTextEdit QScrollBar::handle:vertical:hover {"
        "  background: #569cd6;"
        "}"
        "QPlainTextEdit QScrollBar::add-line:vertical,"
        "QPlainTextEdit QScrollBar::sub-line:vertical {"
        "  height: 0px;"
        "  background: none;"
        "  border: none;"
        "}"
        "QPlainTextEdit QScrollBar::add-page:vertical,"
        "QPlainTextEdit QScrollBar::sub-page:vertical {"
        "  background: transparent;"
        "}"
        "QPlainTextEdit QScrollBar:horizontal {"
        "  background: transparent;"
        "  height: 10px;"
        "  margin: 2px 4px 2px 4px;"
        "}"
        "QPlainTextEdit QScrollBar::handle:horizontal {"
        "  background: #4a4a4a;"
        "  min-width: 24px;"
        "  border-radius: 5px;"
        "}"
        "QPlainTextEdit QScrollBar::handle:horizontal:hover {"
        "  background: #569cd6;"
        "}"
        "QPlainTextEdit QScrollBar::add-line:horizontal,"
        "QPlainTextEdit QScrollBar::sub-line:horizontal {"
        "  width: 0px;"
        "  background: none;"
        "  border: none;"
        "}"
        "QPlainTextEdit QScrollBar::add-page:horizontal,"
        "QPlainTextEdit QScrollBar::sub-page:horizontal {"
        "  background: transparent;"
        "}");
#endif // Q_OS_ANDROID
    logLayout->addWidget(m_logView);
    layout->addWidget(logCard);

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
#ifdef Q_OS_ANDROID
    row.dotLabel->setFixedSize(20, 20);
    row.dotLabel->setStyleSheet(
        "background-color: #cc3333; border-radius: 10px; border: none;");
#else
    row.dotLabel->setFixedSize(14, 14);
    row.dotLabel->setStyleSheet(
        "background-color: #cc3333; border-radius: 7px; border: none;");
#endif

    // Name
    row.nameLabel = new QLabel(hs->name());
    row.nameLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    QFont boldFont = row.nameLabel->font();
    boldFont.setBold(true);
#ifdef Q_OS_ANDROID
    boldFont.setPointSize(boldFont.pointSize() + 3);
#endif
    row.nameLabel->setFont(boldFont);

    // TX TG
    row.txTgSpin = new QSpinBox();
    row.txTgSpin->setRange(1, 999999);
    row.txTgSpin->setPrefix("TG ");
    row.txTgSpin->setValue(hs->txTalkgroup());
#ifdef Q_OS_ANDROID
    row.txTgSpin->setMinimumWidth(90);
    row.txTgSpin->setFixedHeight(39);
#else
    row.txTgSpin->setMinimumWidth(65);
    row.txTgSpin->setFixedHeight(28);
#endif

    connect(row.txTgSpin, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [hs](int val) { hs->setTxTalkgroup(val); });

    // CONNECT BUTTON
    row.connectBtn = new QPushButton();
#ifdef Q_OS_ANDROID
    row.connectBtn->setFixedSize(50, 39);
    row.connectBtn->setIconSize(QSize(32, 32));
#else
    row.connectBtn->setFixedSize(36, 28);
    row.connectBtn->setIconSize(QSize(24, 24));
#endif
    row.connectBtn->setIcon(QIcon(":/icons/connect.png"));
    row.connectBtn->setCursor(Qt::PointingHandCursor);
    row.connectBtn->setToolTip("Connect");

    row.connectBtn->setStyleSheet(
        "QPushButton { background-color: #2d2d2d; border: 1px solid #3d3d3d; border-radius: 4px; }"
        "QPushButton:hover { background-color: #3a3a3a; border-color: #569cd6; }"
        "QPushButton:pressed { background-color: #1f1f1f; }"
        );

    connect(row.connectBtn, &QPushButton::clicked, this, [this, index]() {
        onConnectClicked(index);
    });

    // PTT BUTTON
    row.pttBtn = new QPushButton("PTT");
#ifdef Q_OS_ANDROID
    row.pttBtn->setMinimumWidth(81);
    row.pttBtn->setFixedHeight(39);
#else
    row.pttBtn->setMinimumWidth(58);
    row.pttBtn->setFixedHeight(28);
#endif
    row.pttBtn->setEnabled(false);

    row.pttBtn->setStyleSheet(
        "QPushButton { background-color: #2d2d2d; color: #d4d4d4; font-weight: bold; "
        "padding: 0 8px; border: 1px solid #3d3d3d; border-radius: 4px; }"
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
    m_callerActive = true;
    m_callerSrcId  = srcId;

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
    m_callerActive = false;
    m_callerSrcId  = 0;

    if (m_callerLabel)
        m_callerLabel->setText("");
    if (m_callerCallsignLabel)
        m_callerCallsignLabel->setText("");
    if (m_callerNameLabel)
        m_callerNameLabel->setText("");
    if (m_targetLabel)
        m_targetLabel->setText("");
}

void MainWindow::syncCallerDisplay()
{
    bool anyStreamActive = false;
    if (m_manager) {
        for (int i = 0; i < m_manager->count(); ++i) {
            const Hotspot *hs = m_manager->hotspot(i);
            if (hs && hs->isRxStreamActive()) {
                anyStreamActive = true;
                break;
            }
        }
    }

    if (anyStreamActive == m_callerActive)
        return;

    if (!anyStreamActive && m_callerActive) {
        addLog("[UI] Caller info out of sync — clearing stale display");
        onVoiceCallEnded(0);
    }
}

void MainWindow::updateRowState(int index)
{
    if (index < 0 || index >= m_rows.size())
        return;

    Hotspot *hs = m_manager->hotspot(index);
    HotspotRow &row = m_rows[index];

    bool connected = false;

#ifdef Q_OS_ANDROID
    const QString dotStyle = QStringLiteral("border-radius: 10px;");
#else
    const QString dotStyle = QStringLiteral("border-radius: 7px;");
#endif

    switch (hs->state()) {

    case Hotspot::State::Disconnected:
        row.dotLabel->setStyleSheet("background-color: #cc3333; " + dotStyle);
        row.connectBtn->setIcon(QIcon(":/icons/connect.png"));
        row.connectBtn->setToolTip("Connect");
        break;

    case Hotspot::State::Connected:
        row.dotLabel->setStyleSheet("background-color: #33cc33; " + dotStyle);
        row.connectBtn->setIcon(QIcon(":/icons/disconnect.png"));
        row.connectBtn->setToolTip("Disconnect");
        connected = true;
        break;

    default:
        row.dotLabel->setStyleSheet("background-color: #cccc33; " + dotStyle);
        row.connectBtn->setIcon(QIcon(":/icons/abort.png"));
        row.connectBtn->setToolTip("Abort");
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

QString MainWindow::dmrIdsWritablePath()
{
#ifdef Q_OS_ANDROID
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return QDir(dir).filePath("DMRIds.dat");
#else
    return QCoreApplication::applicationDirPath() + "/DMRIds.dat";
#endif
}

void MainWindow::loadDmrIds()
{
    const QString writablePath = dmrIdsWritablePath();

#ifdef Q_OS_ANDROID
    // Prefer a previously downloaded file; fall back to embedded resource.
    if (QFile::exists(writablePath))
        m_dmrLookup = loadDmrLookup(writablePath);
    else
        m_dmrLookup = loadDmrLookup(QStringLiteral(":/DMRIds.dat"));
#else
    m_dmrLookup = loadDmrLookup(writablePath);
#endif
    addLog(QString("DMR ID lookup loaded: %1 entries").arg(m_dmrLookup.size()));
}

void MainWindow::checkAndUpdateDmrIds()
{
    const QString path = dmrIdsWritablePath();
    const QFileInfo fi(path);

    // Skip download if the file exists and is less than 24 hours old.
    if (fi.exists() &&
        fi.lastModified().secsTo(QDateTime::currentDateTime()) < 86400) {
        addLog("DMRIds.dat is up to date.");
        return;
    }

    addLog("DMRIds.dat is outdated or missing — downloading update...");

    auto *nam = new QNetworkAccessManager(this);
    QNetworkRequest req(QUrl("https://odska.cz/DMRIds.dat"));
    req.setTransferTimeout(30000);
    QNetworkReply *reply = nam->get(req);

    connect(reply, &QNetworkReply::finished, this, [this, reply, path, nam]() {
        reply->deleteLater();
        nam->deleteLater();

        if (reply->error() != QNetworkReply::NoError) {
            addLog("DMRIds.dat download failed: " + reply->errorString());
            return;
        }

        const QByteArray data = reply->readAll();
        if (data.isEmpty()) {
            addLog("DMRIds.dat download returned empty file.");
            return;
        }

        // Ensure directory exists (important on Android)
        QDir().mkpath(QFileInfo(path).absolutePath());

        QFile file(path);
        if (!file.open(QIODevice::WriteOnly)) {
            addLog("DMRIds.dat: cannot write to " + path);
            return;
        }
        file.write(data);
        file.close();

        // Reload lookup with fresh data
        m_dmrLookup = loadDmrLookup(path);
        addLog(QString("DMRIds.dat updated: %1 entries").arg(m_dmrLookup.size()));

        // Update About page date label
        if (m_dmrIdsDateLabel) {
            QFileInfo fiNew(path);
            m_dmrIdsDateLabel->setText(fiNew.lastModified().toString("yyyy-MM-dd"));
        }
    });
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

    QHash<int, bool> reconnectStates;
    if (m_manager) {
        for (int i = 0; i < m_manager->count(); ++i) {
            Hotspot *hs = m_manager->hotspot(i);
            if (!hs)
                continue;
            reconnectStates.insert(hs->configIndex(), hs->state() != Hotspot::State::Disconnected);
        }
    }

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
    if (saved) {
        if (m_audio) {
            m_audio->stopCapture();
            m_audio->resetPlayback();
            audioApplied = m_audio->initialize(m_configMgr->inputDevice(), m_configMgr->outputDevice());
        }

        if (m_manager) {
            m_manager->loadFromConfig(m_configMgr);
            rebuildHotspotsPage();
            wireHotspotConnections();

            for (int i = 0; i < m_manager->count(); ++i) {
                Hotspot *hs = m_manager->hotspot(i);
                if (!hs)
                    continue;

                if (reconnectStates.value(hs->configIndex(), false))
                    hs->connectToServer();
            }
        }

        updateMainPttState();
    }

    if (saved) {
        addLog("Settings saved to " + m_configMgr->configPath());
        if (!audioApplied)
            addLog("Audio device settings could not be applied right now.");

        QMessageBox box(this);
        box.setIcon(QMessageBox::Information);
        box.setWindowTitle("Settings");
        box.setText("Settings saved successfully.");
        box.setStandardButtons(QMessageBox::Ok);
        box.setStyleSheet(
            "QMessageBox { background-color: #2d2d2d; }"
            "QMessageBox QWidget { background-color: #2d2d2d; }"
            "QMessageBox QFrame { background-color: #2d2d2d; }"
            "QMessageBox QLabel { color: #d4d4d4; background-color: transparent; }"
            "QMessageBox QPushButton { min-width: 80px; background-color: #2d2d2d; color: #d4d4d4; "
            "border: 1px solid #3d3d3d; padding: 6px 14px; border-radius: 4px; }"
            "QMessageBox QPushButton:hover { background-color: #3a3a3a; border-color: #505050; }");
        box.exec();
    } else {
        addLog("Error: Failed to save settings!");
        QMessageBox::warning(this, "Error", "Failed to save settings.");
    }
}
