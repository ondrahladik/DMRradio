#include <QApplication>
#include <QDir>
#include <QDebug>
#include <QIcon>
#include <QEvent>
#include <QMouseEvent>
#include <QLineEdit>
#include <QAbstractSpinBox>
#include <QGuiApplication>
#include <QInputMethod>
#include <QTimer>
#include <QThread>
#include <QWidget>
#include <QMessageBox>

#include "ConfigManager.h"
#include "HotspotManager.h"
#include "AudioEngine.h"
#include "MainWindow.h"

#ifdef Q_OS_ANDROID
#include <QPermissions>
#include <QJniObject>
#include <QtCore/qcoreapplication_platform.h>
#endif

#ifdef Q_OS_ANDROID
namespace {

void startAndroidBackgroundService()
{
    QJniObject::callStaticMethod<void>(
        "cz/dmrradio/BackgroundService",
        "start",
        "(Landroid/content/Context;)V",
        QNativeInterface::QAndroidApplication::context());
}

void stopAndroidBackgroundService()
{
    QJniObject::callStaticMethod<void>(
        "cz/dmrradio/BackgroundService",
        "stop",
        "(Landroid/content/Context;)V",
        QNativeInterface::QAndroidApplication::context());
}

void requestAndroidBatteryOptimizationExemption()
{
    QJniObject::callStaticMethod<void>(
        "cz/dmrradio/BackgroundService",
        "requestBatteryOptimizationExemption",
        "(Landroid/content/Context;)V",
        QNativeInterface::QAndroidApplication::context());
}

} // namespace
#endif

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
QSpinBox::up-button, QSpinBox::down-button { width: 0px; height: 0px; border: none; }
QSpinBox { padding-right: 0px; }
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

// Clicking outside a focused input clears focus (and hides keyboard on Android).
class ClickOutsideFilter : public QObject
{
public:
    explicit ClickOutsideFilter(QObject *parent = nullptr) : QObject(parent) {}

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (event->type() == QEvent::MouseButtonPress) {
            QWidget *focused = QApplication::focusWidget();
            if (focused && (qobject_cast<QLineEdit *>(focused) ||
                            qobject_cast<QAbstractSpinBox *>(focused))) {
                auto *me = static_cast<QMouseEvent *>(event);
                QWidget *clicked = QApplication::widgetAt(me->globalPosition().toPoint());
                // Walk up parent hierarchy — QSpinBox has an internal QLineEdit child,
                // so widgetAt() may return that child instead of the spinbox itself.
                bool isInsideFocused = false;
                QWidget *w = clicked;
                while (w) {
                    if (w == focused) { isInsideFocused = true; break; }
                    w = w->parentWidget();
                }
                if (!isInsideFocused) {
                    focused->clearFocus();
#ifdef Q_OS_ANDROID
                    QGuiApplication::inputMethod()->hide();
#endif
                }
            }
        }
        return QObject::eventFilter(watched, event);
    }
};

int main(int argc, char *argv[])
{
#ifdef Q_OS_ANDROID
    // Forward hardware volume keys to Qt so the app can use them for PTT.
    qputenv("QT_ANDROID_VOLUME_KEYS", "1");
#endif
    QApplication app(argc, argv);
#ifdef Q_OS_ANDROID
    app.setQuitOnLastWindowClosed(false);
#endif
    app.setApplicationName("DMR radio");
    app.setApplicationVersion("1.0.6");
    app.setWindowIcon(QIcon(":/icons/logo.png"));
    app.installEventFilter(new ClickOutsideFilter(&app));

#ifdef Q_OS_ANDROID
    startAndroidBackgroundService();
    requestAndroidBatteryOptimizationExemption();
    QObject::connect(&app, &QCoreApplication::aboutToQuit, []() {
        stopAndroidBackgroundService();
    });

    QTimer::singleShot(0, &app, [&app]() {
        QMicrophonePermission micPerm;
        if (app.checkPermission(micPerm) == Qt::PermissionStatus::Undetermined) {
            app.requestPermission(micPerm, &app, [](const QPermission &perm) {
                if (perm.status() == Qt::PermissionStatus::Denied)
                    qWarning() << "Android microphone permission denied at startup";
            });
        }
    });
#endif

    const QString configPath = ConfigManager::resolveConfigPath();

    ConfigManager config;
    if (configPath.isEmpty() || !config.load(configPath)) {
        QMessageBox mb;
        mb.setIcon(QMessageBox::Warning);
        mb.setWindowTitle("Error");
        mb.setText("The config.json file was not found.");
        mb.setStandardButtons(QMessageBox::Ok);
        mb.setStyleSheet(
            "QMessageBox { background-color: #2d2d2d; }"
            "QMessageBox QWidget { background-color: #2d2d2d; }"
            "QMessageBox QFrame { background-color: #2d2d2d; }"
            "QMessageBox QLabel { color: #d4d4d4; background-color: transparent; }"
            "QMessageBox QPushButton { min-width: 80px; background-color: #2d2d2d; color: #d4d4d4; "
            "border: 1px solid #3d3d3d; padding: 6px 14px; border-radius: 4px; }"
            "QMessageBox QPushButton:hover { background-color: #3a3a3a; border-color: #505050; }");
        mb.exec();
        return 1;
    }

    app.setStyleSheet(APP_DARK_STYLE);

#ifdef Q_OS_ANDROID
    app.setStyleSheet(QString(APP_DARK_STYLE) + R"(
        QWidget { font-size: 13pt; }
        QLabel { font-size: 13pt; }
        QPushButton { font-size: 13pt; }
        QLineEdit, QSpinBox, QComboBox { font-size: 13pt; }
        QCheckBox, QRadioButton { font-size: 13pt; }
        QPlainTextEdit { font-size: 11pt; }
    )");
#endif

    HotspotManager manager;
    if (!manager.loadFromConfig(&config)) {
        qCritical() << "Failed to load hotspots from config.";
        return 1;
    }

    AudioEngine audio;
    if (!audio.initialize(config.inputDevice(), config.outputDevice())) {
        qWarning() << "Audio engine initialization failed.";
    }

    // Wire audio engine before MainWindow so the decode→play pipeline
    // runs entirely on the radioThread (bypasses the suspended UI thread
    // when Android backgrounds the app).
    manager.setAudioEngine(&audio);

    MainWindow window(&manager, &audio, &config);

#ifdef Q_OS_ANDROID
    QThread radioThread;
    radioThread.setObjectName("RadioWorker");
    QObject::connect(&app, &QCoreApplication::aboutToQuit, &radioThread, &QThread::quit);
    QObject::connect(&app, &QCoreApplication::aboutToQuit, [&radioThread]() {
        radioThread.wait();
    });
    manager.moveToThread(&radioThread);
    audio.moveToThread(&radioThread);
    radioThread.start();
#endif

#ifdef Q_OS_ANDROID
    window.showMaximized();
#else
    window.show();
#endif

    return app.exec();
}
