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

class HotspotManager;
class AudioEngine;
class Hotspot;
class AmbeDecoder;
class ConfigManager;

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

private slots:
    void onConnectClicked(int index);
    void onHsPttPressed(int index);
    void onHsPttReleased(int index);
    void onMainPttPressed();
    void onMainPttReleased();
    void onHotspotStateChanged(int index);
    void onPttChanged(int index, bool active);
    void onVoiceCallStarted(int index, quint32 srcId);
    void onVoiceCallEnded(int index);
    void addLog(const QString &msg);
    void saveSettings();

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
    void loadSettingsToUi();
    void loadDmrIds();

    HotspotManager *m_manager = nullptr;
    AudioEngine *m_audio = nullptr;
    AmbeDecoder *m_decoder = nullptr;
    ConfigManager *m_configMgr = nullptr;

    QVector<HotspotRow> m_rows;
    QStackedWidget *m_stack = nullptr;
    QPlainTextEdit *m_logView = nullptr;
    QPushButton *m_mainPttBtn = nullptr;
    QLabel *m_callerLabel = nullptr;
    QLabel *m_callerCallsignLabel = nullptr;
    QLabel *m_callerNameLabel = nullptr;
    QLabel *m_targetLabel = nullptr;
    QSlider *m_volumeSlider = nullptr;
    QLabel *m_volumeValueLabel = nullptr;

    // Navigation buttons
    QPushButton *m_navHotspots = nullptr;
    QPushButton *m_navSettings = nullptr;
    QPushButton *m_navLog = nullptr;
    QPushButton *m_navAbout = nullptr;

    // Settings page fields
    QLineEdit *m_settHost = nullptr;
    QSpinBox *m_settPort = nullptr;
    QLineEdit *m_settCallsign = nullptr;
    QLineEdit *m_settPassword = nullptr;
    QLineEdit *m_settDmrId = nullptr;
    QComboBox *m_settInputDevice = nullptr;
    QComboBox *m_settOutputDevice = nullptr;

    struct TgRow {
        QLineEdit *name = nullptr;
        QSpinBox *suffix = nullptr;
        QLineEdit *options = nullptr;
        QCheckBox *enabled = nullptr;
        QRadioButton *isMain = nullptr;
        QCheckBox *rxEnabled = nullptr;
    };
    QVector<TgRow> m_settTgRows;
    QButtonGroup *m_mainGroup = nullptr;

    QHash<quint32, QPair<QString, QString>> m_dmrLookup;
};

#endif // MAINWINDOW_H
