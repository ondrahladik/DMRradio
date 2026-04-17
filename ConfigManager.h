#ifndef CONFIGMANAGER_H
#define CONFIGMANAGER_H

#include <QString>
#include <QJsonObject>
#include <QJsonArray>

class ConfigManager
{
public:
    ConfigManager() = default;

    // Returns the platform-appropriate path to config.json.
    // Priority: exe-dir (non-Android) → writable AppData → resource fallback.
    // On Android, the bundled config is copied to writable AppData and refreshed
    // after an app install/update so stale data from an older APK is not reused.
    static QString resolveConfigPath();

    bool load(const QString &path);
    bool save(const QString &path);
    bool save();

    QString configPath() const { return m_path; }

    QString host() const;
    quint16 port() const;
    QString password() const;
    QString serverMode() const;
    bool isMultipleServerMode() const;
    QString callsign() const;
    quint32 dmrId() const;
    QString inputDevice() const;
    QString outputDevice() const;
    int micGain() const;
    int volume() const;

    void setHost(const QString &v);
    void setPort(quint16 v);
    void setPassword(const QString &v);
    void setServerMode(const QString &v);
    void setCallsign(const QString &v);
    void setDmrId(quint32 v);
    void setInputDevice(const QString &v);
    void setOutputDevice(const QString &v);
    void setMicGain(int v);
    void setVolume(int v);

    int hotspotCount() const;
    QString hotspotName(int i) const;
    int hotspotSuffix(int i) const;
    bool hotspotEnabled(int i) const;
    bool hotspotIsMain(int i) const;
    bool hotspotRxEnabled(int i) const;
    QString hotspotOptions(int i) const;
    int hotspotTxTg(int i) const;
    QString hotspotServerHost(int i) const;
    quint16 hotspotServerPort(int i) const;
    QString hotspotServerPassword(int i) const;

    // Computed: dmrId * 100 + suffix
    quint32 hotspotDmrId(int i) const;

    void setHotspotName(int i, const QString &v);
    void setHotspotSuffix(int i, int v);
    void setHotspotEnabled(int i, bool v);
    void setHotspotIsMain(int i, bool v);
    void setHotspotRxEnabled(int i, bool v);
    void setHotspotOptions(int i, const QString &v);
    void setHotspotTxTg(int i, int v);
    void setHotspotServerHost(int i, const QString &v);
    void setHotspotServerPort(int i, quint16 v);
    void setHotspotServerPassword(int i, const QString &v);

private:
    QJsonArray hotspotsArray() const;
    void setHotspotsArray(const QJsonArray &arr);
    void ensureServerConfig();
    static QString hotspotServerKey(int i);

    QString m_path;
    QJsonObject m_root;
};

#endif // CONFIGMANAGER_H
