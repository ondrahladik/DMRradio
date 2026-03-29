#ifndef HOTSPOTMANAGER_H
#define HOTSPOTMANAGER_H

#include <QObject>
#include <QList>
#include "Hotspot.h"

class ConfigManager;

// Manages a collection of Hotspot instances.
// Only creates enabled hotspots, tracks the main hotspot, enforces single-TX.
class HotspotManager : public QObject
{
    Q_OBJECT

public:
    explicit HotspotManager(QObject *parent = nullptr);
    ~HotspotManager() override;

    // Load hotspot definitions from ConfigManager (only enabled ones).
    bool loadFromConfig(ConfigManager *cfg);

    // Access individual hotspots
    int count() const { return m_hotspots.size(); }
    Hotspot *hotspot(int index) const;
    QList<Hotspot *> hotspots() const { return m_hotspots; }

    // Main hotspot index within m_hotspots (-1 if none)
    int mainHotspotIndex() const { return m_mainIndex; }

    // Connect / disconnect all hotspots
    void connectAll();
    void disconnectAll();

    // PTT management — ensures only one hotspot transmits at a time.
    bool requestPtt(int hotspotIndex);
    void releasePtt(int hotspotIndex);

    int activeTxIndex() const { return m_activeTxIndex; }

signals:
    void hotspotAdded(int index, Hotspot *hs);
    void logMessage(const QString &msg);
    void pttChanged(int hotspotIndex, bool active);

private:
    void addHotspot(const Hotspot::Config &cfg);

    QList<Hotspot *> m_hotspots;
    int m_activeTxIndex = -1;
    int m_mainIndex = -1;
};

#endif // HOTSPOTMANAGER_H
