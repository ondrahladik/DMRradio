#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QLabel>
#include <QPushButton>
#include <QCheckBox>
#include <QComboBox>
#include <QRadioButton>
#include <QButtonGroup>
#include <QStackedWidget>
#include <QPlainTextEdit>
#include <QLineEdit>
#include <QSpinBox>
#include <QSlider>
#include <QHash>
#include <QVector>

#include "AudioLevelBar.h"

class QEvent;
class HotspotManager;
class AudioEngine;
class Hotspot;
class ConfigManager;
class QWidget;

struct HotspotRow {
    int configIndex = -1;
    QLabel *dotLabel = nullptr;
    QLabel *nameLabel = nullptr;
    QPushButton *connectBtn = nullptr;
    QPushButton *pttBtn = nullptr;
    QSpinBox *txTgSpin = nullptr;
};

// Main application window with multi-page layout (Hotspots / Settings / Log).
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(HotspotManager *manager, AudioEngine *audio,
                        ConfigManager *config, QWidget *parent = nullptr);
    ~MainWindow() override;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private slots:
    void onConnectClicked(int index);
    void onHsPttPressed(int index);
    void onHsPttReleased(int index);
    void onMainPttPressed();
    void onMainPttReleased();
    void onMuteToggled();
    void onCallTypeToggled();
    void onHotspotStateChanged(int index);
    void onPttChanged(int index, bool active);
    void onVoiceCallStarted(int index, quint32 srcId);
    void onVoiceCallEnded(int index);
    void addLog(const QString &msg);
    void saveSettings();
    void syncCallerDisplay();

private:
    void buildUi();
    void switchPage(int index);
    QWidget *createHotspotsPage();
    QWidget *createLogPage();
    QWidget *createSettingsPage();
    QWidget *createAboutPage();
    HotspotRow createHotspotRow(int index, Hotspot *hs);
    void updateRowState(int index);
    void updateMainPttState();
    void wireHotspotConnections();
    void rebuildHotspotsPage();
    void loadSettingsToUi();
    void updateServerModeUi();
    QString currentServerModeUi() const;
    void loadDmrIds();
    void checkAndUpdateDmrIds();
    void startDmrIdsDownload(const QString &url, const QString &savePath);
    static QString dmrIdsWritablePath();

    HotspotManager *m_manager = nullptr;
    AudioEngine *m_audio = nullptr;
    ConfigManager *m_configMgr = nullptr;

    QVector<HotspotRow> m_rows;
    QStackedWidget *m_stack = nullptr;
    QPlainTextEdit *m_logView = nullptr;
    QPushButton *m_mainPttBtn = nullptr;
    bool m_mainPttKeyDown = false;
    QPushButton *m_extraBtn = nullptr;
    QPushButton *m_muteBtn = nullptr;
    bool m_isMuted = false;
    bool m_isPrivateCall = false;
    int  m_savedVolume = 100;
    QLabel *m_callerLabel = nullptr;
    QLabel *m_callerCallsignLabel = nullptr;
    QLabel *m_callerNameLabel = nullptr;
    QLabel *m_targetLabel = nullptr;
    QSlider *m_volumeSlider = nullptr;
    QLabel *m_volumeValueLabel = nullptr;
    QSlider *m_micGainSlider = nullptr;
    QLabel *m_micGainValueLabel = nullptr;
    AudioLevelBar *m_audioLevelBar = nullptr;

    // Caller info display state
    bool    m_callerActive = false;
    quint32 m_callerSrcId = 0;
    QTimer *m_callerSyncTimer = nullptr;

    // Navigation buttons
    QPushButton *m_navHotspots = nullptr;
    QPushButton *m_navSettings = nullptr;
    QPushButton *m_navLog = nullptr;
    QPushButton *m_navAbout = nullptr;

    // Settings page fields
    QPushButton *m_serverModeSingleBtn = nullptr;
    QPushButton *m_serverModeMultipleBtn = nullptr;
    QWidget *m_singleServerFields = nullptr;
    QWidget *m_multiServerFields = nullptr;
    QLineEdit *m_settHost = nullptr;
    QSpinBox *m_settPort = nullptr;
    QLineEdit *m_settCallsign = nullptr;
    QLineEdit *m_settPassword = nullptr;
    QLineEdit *m_settDmrId = nullptr;
    QComboBox *m_settInputDevice = nullptr;
    QComboBox *m_settOutputDevice = nullptr;
    QVector<QLineEdit *> m_settServerHosts;
    QVector<QSpinBox *> m_settServerPorts;
    QVector<QLineEdit *> m_settServerPasswords;

    struct TgRow {
        QSpinBox *txTg = nullptr;
        QSpinBox *suffix = nullptr;
        QLineEdit *options = nullptr;
        QCheckBox *enabled = nullptr;
        QRadioButton *isMain = nullptr;
        QCheckBox *rxEnabled = nullptr;
    };
    QVector<TgRow> m_settTgRows;
    QButtonGroup *m_mainGroup = nullptr;

    QHash<quint32, QPair<QString, QString>> m_dmrLookup;

    QLabel *m_dmrIdsDateLabel = nullptr;  // About page — updated after auto-download
};

#endif // MAINWINDOW_H
