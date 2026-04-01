#include <QApplication>
#include <QDir>
#include <QDebug>
#include <QIcon>

#include "ConfigManager.h"
#include "HotspotManager.h"
#include "AudioEngine.h"
#include "MainWindow.h"

// Dark theme applied at application level so all dialogs inherit it
static const char *APP_DARK_STYLE = R"(
QWidget {
    background-color: #1e1e1e;
    color: #d4d4d4;
    font-size: 10pt;
}
QGroupBox {
    border: 1px solid #3d3d3d;
    border-radius: 6px;
    margin-top: 14px;
    padding: 14px 8px 8px 8px;
    font-weight: bold;
}
QGroupBox::title {
    subcontrol-origin: margin;
    left: 12px;
    padding: 0 6px;
    color: #569cd6;
}
QPushButton {
    background-color: #2d2d2d;
    color: #d4d4d4;
    border: 1px solid #3d3d3d;
    padding: 6px 14px;
    border-radius: 4px;
}
QPushButton:hover { background-color: #3a3a3a; border-color: #505050; }
QPushButton:pressed { background-color: #4a4a4a; }
QPushButton:disabled { color: #555555; background-color: #252525; border-color: #303030; }
QPushButton[navActive="true"] {
    background-color: #264f78;
    border-color: #569cd6;
    color: #ffffff;
}
QLineEdit, QSpinBox {
    background-color: #2d2d2d;
    color: #d4d4d4;
    border: 1px solid #3d3d3d;
    padding: 5px;
    border-radius: 3px;
    selection-background-color: #264f78;
}
QLineEdit:focus, QSpinBox:focus { border-color: #569cd6; }
QComboBox {
    background-color: #2d2d2d;
    color: #d4d4d4;
    border: 1px solid #3d3d3d;
    padding: 5px;
    border-radius: 3px;
    selection-background-color: #264f78;
}
QComboBox:focus { border-color: #569cd6; }
QComboBox::drop-down {
    border: none;
    width: 20px;
}
QComboBox QAbstractItemView {
    background-color: #2d2d2d;
    color: #d4d4d4;
    border: 1px solid #3d3d3d;
    selection-background-color: #264f78;
}
QCheckBox { color: #d4d4d4; spacing: 6px; }
QRadioButton { color: #d4d4d4; spacing: 6px; }
QPlainTextEdit {
    background-color: #1a1a1a;
    color: #cccccc;
    border: 1px solid #3d3d3d;
    font-family: Consolas, monospace;
    font-size: 9pt;
}
QLabel { color: #d4d4d4; }
QFrame#separator { color: #3d3d3d; }
QScrollArea { background-color: transparent; border: none; }
QMessageBox { background-color: #2d2d2d; }
QMessageBox QLabel { color: #d4d4d4; }
QMessageBox QPushButton { min-width: 80px; }
)";

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.setApplicationName("DMR radio");
    app.setApplicationVersion("1.0.3");
    app.setWindowIcon(QIcon(":/icons/logo.png"));
    app.setStyleSheet(APP_DARK_STYLE);

    // Locate config.json — resolves platform-specific path automatically.
    // On Windows: next to exe, then AppData fallback.
    // On Android: AppDataLocation (seeded from :/config.json on first run).
    const QString configPath = ConfigManager::resolveConfigPath();

    ConfigManager config;
    if (!config.load(configPath)) {
        qCritical() << "Failed to load config from:" << configPath;
        return 1;
    }

    HotspotManager manager;
    if (!manager.loadFromConfig(&config)) {
        qCritical() << "Failed to load hotspots from config.";
        return 1;
    }

    AudioEngine audio;
    if (!audio.initialize(config.inputDevice(), config.outputDevice())) {
        qWarning() << "Audio engine initialization failed.";
    }

    MainWindow window(&manager, &audio, &config);
    window.show();

    return app.exec();
}
