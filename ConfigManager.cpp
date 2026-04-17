#include "ConfigManager.h"
#include "ConfigBuildInfo.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QStandardPaths>
#include <QSaveFile>
#ifdef Q_OS_ANDROID
#include <QJniObject>
#include <QtCore/qcoreapplication_platform.h>
#endif

namespace {

QString normalizedServerMode(const QJsonValue &value)
{
    const QString mode = value.toString().trimmed().toLower();
    return mode == QStringLiteral("multiple")
        ? QStringLiteral("multiple")
        : QStringLiteral("single");
}

QString configMarkerPath(const QString &configPath)
{
    return configPath + QStringLiteral(".version");
}

bool writeTextFile(const QString &path, const QByteArray &data)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
        return false;
    if (file.write(data) != data.size())
        return false;
    return file.commit();
}

bool installBundledConfig(const QString &path)
{
    QFile source(QStringLiteral(":/config.json"));
    if (!source.open(QIODevice::ReadOnly | QIODevice::Text))
        return false;

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
        return false;

    if (file.write(source.readAll()) < 0)
        return false;
    if (!file.commit())
        return false;

    QFile::setPermissions(path,
        QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    return true;
}

QString bundledConfigMarkerValue(qint64 installStamp)
{
    return QString::fromLatin1(APP_BUNDLED_CONFIG_HASH) + QLatin1Char('\n') + QString::number(installStamp);
}

#ifdef Q_OS_ANDROID
qint64 androidInstallStamp()
{
    const QJniObject activity = QNativeInterface::QAndroidApplication::context();
    if (!activity.isValid())
        return -1;

    const QJniObject packageName = activity.callObjectMethod("getPackageName", "()Ljava/lang/String;");
    const QJniObject packageManager = activity.callObjectMethod("getPackageManager", "()Landroid/content/pm/PackageManager;");
    if (!packageName.isValid() || !packageManager.isValid())
        return -1;

    const QJniObject info = packageManager.callObjectMethod(
        "getPackageInfo",
        "(Ljava/lang/String;I)Landroid/content/pm/PackageInfo;",
        packageName.object<jstring>(),
        jint(0));
    if (!info.isValid())
        return -1;

    return static_cast<qint64>(info.getField<jlong>("lastUpdateTime"));
}
#endif

bool bundledConfigIsCurrent(const QString &markerPath, qint64 installStamp)
{
    QFile marker(markerPath);
    if (!marker.open(QIODevice::ReadOnly | QIODevice::Text))
        return false;

    const QByteArray raw = marker.readAll().trimmed();
    const QByteArray expectedHash = QByteArray(APP_BUNDLED_CONFIG_HASH);
    const QByteArray expectedValue = bundledConfigMarkerValue(installStamp).toUtf8();

    return installStamp >= 0 ? (raw == expectedValue) : (raw == expectedHash);
}

} // namespace

QString ConfigManager::resolveConfigPath()
{
    const QString fileName = QStringLiteral("config.json");

#ifndef Q_OS_ANDROID
    const QString exeConfig =
        QDir(QCoreApplication::applicationDirPath()).filePath(fileName);
    if (QFile::exists(exeConfig))
        return exeConfig;
    return QString();
#else
    // Android: use writable AppData directory.
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dir);
    const QString writablePath = QDir(dir).filePath(fileName);
    const QString markerPath = configMarkerPath(writablePath);
    const bool haveConfig = QFile::exists(writablePath);
    const qint64 installStamp = androidInstallStamp();

    if (haveConfig && installStamp >= 0 && bundledConfigIsCurrent(markerPath, installStamp))
        return writablePath;

    if (QFile::exists(QStringLiteral(":/config.json"))) {
        if (!installBundledConfig(writablePath))
            return QString();
        if (installStamp >= 0)
            writeTextFile(markerPath, bundledConfigMarkerValue(installStamp).toUtf8());
        else
            writeTextFile(markerPath, QByteArray(APP_BUNDLED_CONFIG_HASH) + '\n');
        return writablePath;
    }

    return QString();
#endif
}

bool ConfigManager::load(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return false;

    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &err);
    file.close();

    if (doc.isNull())
        return false;

    m_root = doc.object();
    m_path = path;

    if (m_root.contains("mic_gain") && !m_root.contains("mic")) {
        m_root["mic"] = m_root["mic_gain"];
        m_root.remove("mic_gain");
    }
    if (m_root.contains("volume") && !m_root.contains("vol")) {
        m_root["vol"] = m_root["volume"];
        m_root.remove("volume");
    }

    ensureServerConfig();

    return true;
}

static QByteArray encodeScalar(const QJsonValue &v)
{
    QJsonArray wrapper;
    wrapper.append(v);
    QByteArray bytes = QJsonDocument(wrapper).toJson(QJsonDocument::Compact);
    return bytes.mid(1, bytes.size() - 2); // strip surrounding [ and ]
}

QStringList orderedKeysForObject(const QJsonObject &obj)
{
    QStringList ordered;
    const QStringList preferred = {"host", "pass", "port", "password"};

    for (const QString &key : preferred) {
        if (obj.contains(key))
            ordered.append(key);
    }

    for (const QString &key : obj.keys()) {
        if (!ordered.contains(key))
            ordered.append(key);
    }

    return ordered;
}

QByteArray encodeValueIndented(const QJsonValue &value, int indentLevel)
{
    const QByteArray indent(indentLevel * 4, ' ');
    const QByteArray childIndent((indentLevel + 1) * 4, ' ');

    if (value.isObject()) {
        const QJsonObject obj = value.toObject();
        const QStringList keys = orderedKeysForObject(obj);

        QByteArray out = "{\n";
        for (int i = 0; i < keys.size(); ++i) {
            const QString &key = keys[i];
            out += childIndent;
            out += "\"" + key.toUtf8() + "\": ";
            out += encodeValueIndented(obj.value(key), indentLevel + 1);
            if (i + 1 < keys.size())
                out += ",";
            out += "\n";
        }
        out += indent + "}";
        return out;
    }

    if (value.isArray()) {
        const QJsonArray array = value.toArray();
        QByteArray out = "[\n";
        for (int i = 0; i < array.size(); ++i) {
            out += childIndent;
            out += encodeValueIndented(array.at(i), indentLevel + 1);
            if (i + 1 < array.size())
                out += ",";
            out += "\n";
        }
        out += indent + "]";
        return out;
    }

    return encodeScalar(value);
}

bool ConfigManager::save(const QString &path)
{
    const QStringList topOrder = {
        "callsign", "dmrId", "host", "port", "password",
        "server_mode", "servers",
        "input_device", "output_device", "mic", "vol", "hotspots"
    };

    QStringList writeKeys;
    for (const QString &k : topOrder)
        if (m_root.contains(k))
            writeKeys.append(k);
    for (const QString &k : m_root.keys())
        if (!writeKeys.contains(k))
            writeKeys.append(k);

    QByteArray out = "{\n";
    for (int i = 0; i < writeKeys.size(); ++i) {
        const QString &key = writeKeys[i];
        const QJsonValue val = m_root[key];
        const bool isLast = (i == writeKeys.size() - 1);

        out += "    \"" + key.toUtf8() + "\": ";
        out += encodeValueIndented(val, 1);

        if (!isLast) out += ',';
        out += '\n';
    }
    out += "}\n";

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
        return false;
    file.write(out);
    file.close();
    m_path = path;
    return true;
}

bool ConfigManager::save()
{
    return m_path.isEmpty() ? false : save(m_path);
}

QString ConfigManager::host() const      { return m_root["host"].toString("127.0.0.1"); }
quint16 ConfigManager::port() const      { return static_cast<quint16>(m_root["port"].toInt(62031)); }
QString ConfigManager::password() const  { return m_root["password"].toString(); }
QString ConfigManager::serverMode() const { return normalizedServerMode(m_root["server_mode"]); }
bool ConfigManager::isMultipleServerMode() const { return serverMode() == QStringLiteral("multiple"); }
QString ConfigManager::callsign() const  { return m_root["callsign"].toString(); }
quint32 ConfigManager::dmrId() const     { return static_cast<quint32>(m_root["dmrId"].toDouble(0)); }
QString ConfigManager::inputDevice() const { return m_root["input_device"].toString(); }
QString ConfigManager::outputDevice() const { return m_root["output_device"].toString(); }
int ConfigManager::micGain() const { return m_root.contains("mic") ? m_root["mic"].toInt(50) : m_root["mic_gain"].toInt(50); }
int ConfigManager::volume() const  { return m_root.contains("vol") ? m_root["vol"].toInt(100) : m_root["volume"].toInt(100); }

void ConfigManager::setHost(const QString &v)     { m_root["host"] = v; }
void ConfigManager::setPort(quint16 v)            { m_root["port"] = static_cast<int>(v); }
void ConfigManager::setPassword(const QString &v) { m_root["password"] = v; }
void ConfigManager::setServerMode(const QString &v) { m_root["server_mode"] = normalizedServerMode(v); }
void ConfigManager::setCallsign(const QString &v) { m_root["callsign"] = v; }
void ConfigManager::setDmrId(quint32 v)           { m_root["dmrId"] = static_cast<double>(v); }
void ConfigManager::setInputDevice(const QString &v) { m_root["input_device"] = v; }
void ConfigManager::setOutputDevice(const QString &v) { m_root["output_device"] = v; }
void ConfigManager::setMicGain(int v)             { m_root["mic"] = v; m_root.remove("mic_gain"); }
void ConfigManager::setVolume(int v)              { m_root["vol"] = v; m_root.remove("volume"); }

QJsonArray ConfigManager::hotspotsArray() const                { return m_root["hotspots"].toArray(); }
void ConfigManager::setHotspotsArray(const QJsonArray &arr)    { m_root["hotspots"] = arr; }

int ConfigManager::hotspotCount() const { return hotspotsArray().size(); }

QString ConfigManager::hotspotName(int i) const
{
    QJsonArray arr = hotspotsArray();
    return (i >= 0 && i < arr.size()) ? arr[i].toObject()["name"].toString() : QString();
}

int ConfigManager::hotspotSuffix(int i) const
{
    QJsonArray arr = hotspotsArray();
    return (i >= 0 && i < arr.size()) ? arr[i].toObject()["suffix"].toInt(11) : 11;
}

bool ConfigManager::hotspotEnabled(int i) const
{
    QJsonArray arr = hotspotsArray();
    return (i >= 0 && i < arr.size()) ? arr[i].toObject()["enabled"].toBool(true) : false;
}

bool ConfigManager::hotspotIsMain(int i) const
{
    QJsonArray arr = hotspotsArray();
    return (i >= 0 && i < arr.size()) ? arr[i].toObject()["is_main"].toBool(false) : false;
}

quint32 ConfigManager::hotspotDmrId(int i) const
{
    return dmrId() * 100 + static_cast<quint32>(hotspotSuffix(i));
}

bool ConfigManager::hotspotRxEnabled(int i) const
{
    QJsonArray arr = hotspotsArray();
    return (i >= 0 && i < arr.size()) ? arr[i].toObject()["rx_enabled"].toBool(true) : true;
}

QString ConfigManager::hotspotOptions(int i) const
{
    QJsonArray arr = hotspotsArray();
    return (i >= 0 && i < arr.size()) ? arr[i].toObject()["options"].toString() : QString();
}

int ConfigManager::hotspotTxTg(int i) const
{
    QJsonArray arr = hotspotsArray();
    return (i >= 0 && i < arr.size()) ? arr[i].toObject()["tx_tg"].toInt(0) : 0;
}

QString ConfigManager::hotspotServerHost(int i) const
{
    const QJsonObject servers = m_root["servers"].toObject();
    const QJsonObject server = servers[hotspotServerKey(i)].toObject();
    return server["host"].toString();
}

quint16 ConfigManager::hotspotServerPort(int i) const
{
    const QJsonObject servers = m_root["servers"].toObject();
    const QJsonObject server = servers[hotspotServerKey(i)].toObject();
    const QJsonValue value = server["port"];

    bool ok = false;
    int port = 0;
    if (value.isString())
        port = value.toString().toInt(&ok);
    else if (value.isDouble()) {
        port = value.toInt();
        ok = true;
    }

    return ok && port > 0 && port <= 65535
        ? static_cast<quint16>(port)
        : 0;
}

QString ConfigManager::hotspotServerPassword(int i) const
{
    const QJsonObject servers = m_root["servers"].toObject();
    const QJsonObject server = servers[hotspotServerKey(i)].toObject();
    if (server.contains("pass"))
        return server["pass"].toString();
    return server["password"].toString();
}

void ConfigManager::setHotspotName(int i, const QString &v)
{
    QJsonArray arr = hotspotsArray();
    if (i < 0 || i >= arr.size()) return;
    QJsonObject obj = arr[i].toObject();
    obj["name"] = v;
    arr[i] = obj;
    setHotspotsArray(arr);
}

void ConfigManager::setHotspotSuffix(int i, int v)
{
    QJsonArray arr = hotspotsArray();
    if (i < 0 || i >= arr.size()) return;
    QJsonObject obj = arr[i].toObject();
    obj["suffix"] = v;
    arr[i] = obj;
    setHotspotsArray(arr);
}

void ConfigManager::setHotspotEnabled(int i, bool v)
{
    QJsonArray arr = hotspotsArray();
    if (i < 0 || i >= arr.size()) return;
    QJsonObject obj = arr[i].toObject();
    obj["enabled"] = v;
    arr[i] = obj;
    setHotspotsArray(arr);
}

void ConfigManager::setHotspotIsMain(int i, bool v)
{
    QJsonArray arr = hotspotsArray();
    if (i < 0 || i >= arr.size()) return;
    // Ensure only one main — clear others first
    if (v) {
        for (int k = 0; k < arr.size(); ++k) {
            if (k == i) continue;
            QJsonObject o = arr[k].toObject();
            o["is_main"] = false;
            arr[k] = o;
        }
    }
    QJsonObject obj = arr[i].toObject();
    obj["is_main"] = v;
    arr[i] = obj;
    setHotspotsArray(arr);
}

void ConfigManager::setHotspotRxEnabled(int i, bool v)
{
    QJsonArray arr = hotspotsArray();
    if (i < 0 || i >= arr.size()) return;
    QJsonObject obj = arr[i].toObject();
    obj["rx_enabled"] = v;
    arr[i] = obj;
    setHotspotsArray(arr);
}

void ConfigManager::setHotspotOptions(int i, const QString &v)
{
    QJsonArray arr = hotspotsArray();
    if (i < 0 || i >= arr.size()) return;
    QJsonObject obj = arr[i].toObject();
    obj["options"] = v;
    arr[i] = obj;
    setHotspotsArray(arr);
}

void ConfigManager::setHotspotTxTg(int i, int v)
{
    QJsonArray arr = hotspotsArray();
    if (i < 0 || i >= arr.size()) return;
    QJsonObject obj = arr[i].toObject();
    obj["tx_tg"] = v;
    arr[i] = obj;
    setHotspotsArray(arr);
}

void ConfigManager::setHotspotServerHost(int i, const QString &v)
{
    if (i < 0)
        return;

    ensureServerConfig();

    QJsonObject servers = m_root["servers"].toObject();
    QJsonObject server = servers[hotspotServerKey(i)].toObject();
    server["host"] = v;
    servers[hotspotServerKey(i)] = server;
    m_root["servers"] = servers;
}

void ConfigManager::setHotspotServerPort(int i, quint16 v)
{
    if (i < 0)
        return;

    ensureServerConfig();

    QJsonObject servers = m_root["servers"].toObject();
    QJsonObject server = servers[hotspotServerKey(i)].toObject();
    server["port"] = v == 0 ? QJsonValue(QString()) : QJsonValue(QString::number(v));
    servers[hotspotServerKey(i)] = server;
    m_root["servers"] = servers;
}

void ConfigManager::setHotspotServerPassword(int i, const QString &v)
{
    if (i < 0)
        return;

    ensureServerConfig();

    QJsonObject servers = m_root["servers"].toObject();
    QJsonObject server = servers[hotspotServerKey(i)].toObject();
    server["pass"] = v;
    server.remove("password");
    servers[hotspotServerKey(i)] = server;
    m_root["servers"] = servers;
}

QString ConfigManager::hotspotServerKey(int i)
{
    return QStringLiteral("hs%1").arg(i + 1);
}

void ConfigManager::ensureServerConfig()
{
    m_root["server_mode"] = normalizedServerMode(m_root["server_mode"]);

    QJsonObject servers = m_root["servers"].toObject();
    const QString globalHost = host();
    const QString globalPort = QString::number(port());
    const QString globalPass = password();
    const int minServerCount = qMax(4, hotspotCount());

    for (int i = 0; i < minServerCount; ++i) {
        const QString key = hotspotServerKey(i);
        QJsonObject server = servers[key].toObject();
        if (!server.contains("host")) {
            server["host"] = (i == 0) ? globalHost : QString();
        }
        if (!server.contains("port")) {
            server["port"] = (i == 0) ? globalPort : QString();
        }
        if (!server.contains("pass") && !server.contains("password")) {
            server["pass"] = (i == 0) ? globalPass : QString();
        } else if (server.contains("password") && !server.contains("pass")) {
            server["pass"] = server["password"];
            server.remove("password");
        }
        servers[key] = server;
    }

    m_root["servers"] = servers;
}
