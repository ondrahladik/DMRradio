#include "ConfigManager.h"
#include <QFile>
#include <QJsonDocument>
#include <QJsonParseError>

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
    return true;
}

bool ConfigManager::save(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
        return false;

    QJsonDocument doc(m_root);
    file.write(doc.toJson(QJsonDocument::Indented));
    file.close();
    m_path = path;
    return true;
}

bool ConfigManager::save()
{
    return m_path.isEmpty() ? false : save(m_path);
}

// ── Global settings ──

QString ConfigManager::host() const      { return m_root["host"].toString("127.0.0.1"); }
quint16 ConfigManager::port() const      { return static_cast<quint16>(m_root["port"].toInt(62031)); }
QString ConfigManager::password() const  { return m_root["password"].toString(); }
QString ConfigManager::callsign() const  { return m_root["callsign"].toString(); }
quint32 ConfigManager::dmrId() const     { return static_cast<quint32>(m_root["dmrId"].toDouble(0)); }
QString ConfigManager::inputDevice() const { return m_root["input_device"].toString(); }
QString ConfigManager::outputDevice() const { return m_root["output_device"].toString(); }

void ConfigManager::setHost(const QString &v)     { m_root["host"] = v; }
void ConfigManager::setPort(quint16 v)            { m_root["port"] = static_cast<int>(v); }
void ConfigManager::setPassword(const QString &v) { m_root["password"] = v; }
void ConfigManager::setCallsign(const QString &v) { m_root["callsign"] = v; }
void ConfigManager::setDmrId(quint32 v)           { m_root["dmrId"] = static_cast<double>(v); }
void ConfigManager::setInputDevice(const QString &v) { m_root["input_device"] = v; }
void ConfigManager::setOutputDevice(const QString &v) { m_root["output_device"] = v; }

// ── Hotspot access ──

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
    // Computed: base DMR ID * 100 + suffix
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

// ── Hotspot setters ──

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
