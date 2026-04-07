#include "ConfigManager.h"
#include "ConfigBuildInfo.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QStandardPaths>
#include <QSaveFile>

namespace {

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

bool bundledConfigIsCurrent(const QString &markerPath)
{
    QFile marker(markerPath);
    if (!marker.open(QIODevice::ReadOnly | QIODevice::Text))
        return false;
    return marker.readAll().trimmed() == QByteArrayLiteral(APP_BUNDLED_CONFIG_HASH);
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

    if (haveConfig && bundledConfigIsCurrent(markerPath))
        return writablePath;

    if (QFile::exists(QStringLiteral(":/config.json"))) {
        if (!installBundledConfig(writablePath))
            return QString();
        const QByteArray hash = QByteArrayLiteral(APP_BUNDLED_CONFIG_HASH);
        writeTextFile(markerPath, hash + '\n');
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

    return true;
}

static QByteArray encodeScalar(const QJsonValue &v)
{
    QJsonArray wrapper;
    wrapper.append(v);
    QByteArray bytes = QJsonDocument(wrapper).toJson(QJsonDocument::Compact);
    return bytes.mid(1, bytes.size() - 2); // strip surrounding [ and ]
}

bool ConfigManager::save(const QString &path)
{
    const QStringList topOrder = {
        "callsign", "dmrId", "host", "port", "password",
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

        if (val.isArray()) {
            // Serialize the array with Qt indentation, then re-indent by 4 extra spaces
            QByteArray arrBytes = QJsonDocument(val.toArray()).toJson(QJsonDocument::Indented);
            QList<QByteArray> lines = arrBytes.split('\n');
            while (!lines.isEmpty() && lines.last().trimmed().isEmpty())
                lines.removeLast();
            for (int j = 1; j < lines.size(); ++j)
                if (!lines[j].isEmpty())
                    lines[j] = "    " + lines[j];
            out += lines.join('\n');
        } else if (val.isObject()) {
            out += QJsonDocument(val.toObject()).toJson(QJsonDocument::Compact);
        } else {
            out += encodeScalar(val);
        }

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
QString ConfigManager::callsign() const  { return m_root["callsign"].toString(); }
quint32 ConfigManager::dmrId() const     { return static_cast<quint32>(m_root["dmrId"].toDouble(0)); }
QString ConfigManager::inputDevice() const { return m_root["input_device"].toString(); }
QString ConfigManager::outputDevice() const { return m_root["output_device"].toString(); }
int ConfigManager::micGain() const { return m_root.contains("mic") ? m_root["mic"].toInt(50) : m_root["mic_gain"].toInt(50); }
int ConfigManager::volume() const  { return m_root.contains("vol") ? m_root["vol"].toInt(100) : m_root["volume"].toInt(100); }

void ConfigManager::setHost(const QString &v)     { m_root["host"] = v; }
void ConfigManager::setPort(quint16 v)            { m_root["port"] = static_cast<int>(v); }
void ConfigManager::setPassword(const QString &v) { m_root["password"] = v; }
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
