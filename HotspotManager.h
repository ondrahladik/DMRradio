#ifndef HOTSPOTMANAGER_H
#define HOTSPOTMANAGER_H

#include <QObject>
#include <QList>
#include <QHash>
#include "Hotspot.h"
#include "AmbeDecoder.h"

class ConfigManager;
class AudioEngine;

class HotspotManager : public QObject
{
    Q_OBJECT

public:
    explicit HotspotManager(QObject *parent = nullptr);
    ~HotspotManager() override;

    bool loadFromConfig(ConfigManager *cfg);

    // Must be called before hotspots start receiving audio.
    // Wires direct (same-thread) connections so the audio pipeline
    // bypasses the UI thread entirely — critical for Android background.
    void setAudioEngine(AudioEngine *audio);

    // Pass DMR ID → name lookup so the background notification can show caller names.
    // Safe to call from any thread before audio connections are wired.
    void setNameLookup(const QHash<quint32, QPair<QString, QString>> &lookup);

    int count() const { return m_hotspots.size(); }
    Hotspot *hotspot(int index) const;
    QList<Hotspot *> hotspots() const { return m_hotspots; }

    // Main hotspot index within m_hotspots (-1 if none)
    int mainHotspotIndex() const { return m_mainIndex; }

    void connectAll();
    void disconnectAll();

    // PTT management — ensures only one hotspot transmits at a time.
    bool requestPtt(int hotspotIndex);
    void releasePtt(int hotspotIndex);

public slots:
    void onPcmCaptured(const QByteArray &data);

    int activeTxIndex() const { return m_activeTxIndex; }

signals:
    void hotspotAdded(int index, Hotspot *hs);
    void logMessage(const QString &msg);
    void pttChanged(int hotspotIndex, bool active);

private slots:
    void onAudioData(const QByteArray &ambe);
    void onVoiceEnded();
    void onVoiceStreamStarted(quint32 srcId, quint32 dstId);

private:
    void addHotspot(const Hotspot::Config &cfg);
    void wireAudioConnections(Hotspot *hs);

    QList<Hotspot *> m_hotspots;
    int m_activeTxIndex = -1;
    int m_mainIndex = -1;

    AmbeDecoder m_decoder;
    AudioEngine *m_audio = nullptr;
    QHash<quint32, QPair<QString, QString>> m_nameLookup; // callsign, name
};

#endif // HOTSPOTMANAGER_H
