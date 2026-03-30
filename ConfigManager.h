#ifndef CONFIGMANAGER_H
#define CONFIGMANAGER_H

#include <QString>
#include <QJsonObject>
#include <QJsonArray>

// Manages loading and saving of the JSON configuration file.
class ConfigManager
{
public:
    ConfigManager() = default;

    bool load(const QString &path);
    bool save(const QString &path);
    bool save();

    QString configPath() const { return m_path; }

    // Global settings
    QString host() const;
    quint16 port() const;
    QString password() const;
    QString callsign() const;
    quint32 dmrId() const;
    QString inputDevice() const;
    QString outputDevice() const;

    void setHost(const QString &v);
    void setPort(quint16 v);
    void setPassword(const QString &v);
    void setCallsign(const QString &v);
    void setDmrId(quint32 v);
    void setInputDevice(const QString &v);
    void setOutputDevice(const QString &v);

    // Hotspot access
    int hotspotCount() const;
    QString hotspotName(int i) const;
    int hotspotSuffix(int i) const;
    bool hotspotEnabled(int i) const;
    bool hotspotIsMain(int i) const;
    bool hotspotRxEnabled(int i) const;
    QString hotspotOptions(int i) const;
    int hotspotTxTg(int i) const;

    // Computed: dmrId * 100 + suffix
    quint32 hotspotDmrId(int i) const;

    void setHotspotName(int i, const QString &v);
    void setHotspotSuffix(int i, int v);
    void setHotspotEnabled(int i, bool v);
    void setHotspotIsMain(int i, bool v);
    void setHotspotRxEnabled(int i, bool v);
    void setHotspotOptions(int i, const QString &v);
    void setHotspotTxTg(int i, int v);

private:
    QJsonArray hotspotsArray() const;
    void setHotspotsArray(const QJsonArray &arr);

    QString m_path;
    QJsonObject m_root;
};

#endif // CONFIGMANAGER_H
