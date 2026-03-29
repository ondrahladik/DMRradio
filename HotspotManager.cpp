#include "HotspotManager.h"
#include "ConfigManager.h"
#include <QDebug>

HotspotManager::HotspotManager(QObject *parent)
    : QObject(parent)
{
}

HotspotManager::~HotspotManager()
{
    disconnectAll();
}

// Parse talkgroup from options string (e.g. "TS2=446" → 446)
static int parseTalkgroupFromOptions(const QString &options)
{
    for (const QString &part : options.split(';')) {
        QString trimmed = part.trimmed();
        if (trimmed.startsWith("TS2=")) {
            bool ok;
            int tg = trimmed.mid(4).toInt(&ok);
            if (ok) return tg;
        }
    }
    return 0;
}

bool HotspotManager::loadFromConfig(ConfigManager *cfg)
{
    if (!cfg)
        return false;

    QString host     = cfg->host();
    quint16 port     = cfg->port();
    QString password = cfg->password();
    QString callsign = cfg->callsign();
    quint32 srcDmrId = cfg->dmrId();

    int total = cfg->hotspotCount();
    if (total == 0) {
        emit logMessage("No hotspots defined in config.");
        return false;
    }

    for (int i = 0; i < total; ++i) {
        if (!cfg->hotspotEnabled(i))
            continue;

        Hotspot::Config hcfg;
        hcfg.name      = cfg->hotspotName(i);
        hcfg.host      = host;
        hcfg.port      = port;
        hcfg.options   = cfg->hotspotOptions(i);
        hcfg.talkgroup = parseTalkgroupFromOptions(hcfg.options);
        hcfg.dmrId     = cfg->hotspotDmrId(i);  // base*100 + suffix
        hcfg.callsign  = callsign;
        hcfg.password  = password;
        hcfg.srcDmrId  = srcDmrId;
        hcfg.configIndex = i;

        int newIndex = m_hotspots.size();
        addHotspot(hcfg);

        if (cfg->hotspotIsMain(i))
            m_mainIndex = newIndex;
    }

    // If no main was set, default to first
    if (m_mainIndex < 0 && !m_hotspots.isEmpty())
        m_mainIndex = 0;

    emit logMessage(QString("Loaded %1 hotspot(s), main=#%2")
                        .arg(m_hotspots.size())
                        .arg(m_mainIndex));
    return !m_hotspots.isEmpty();
}

Hotspot *HotspotManager::hotspot(int index) const
{
    if (index < 0 || index >= m_hotspots.size())
        return nullptr;
    return m_hotspots.at(index);
}

void HotspotManager::connectAll()
{
    for (Hotspot *hs : m_hotspots)
        hs->connectToServer();
}

void HotspotManager::disconnectAll()
{
    for (Hotspot *hs : m_hotspots)
        hs->disconnectFromServer();
}

bool HotspotManager::requestPtt(int hotspotIndex)
{
    if (m_activeTxIndex >= 0 && m_activeTxIndex != hotspotIndex) {
        emit logMessage(QString("PTT denied for HS#%1 — HS#%2 already transmitting")
                            .arg(hotspotIndex)
                            .arg(m_activeTxIndex));
        return false;
    }

    Hotspot *hs = hotspot(hotspotIndex);
    if (!hs || !hs->isConnected())
        return false;

    m_activeTxIndex = hotspotIndex;
    hs->startTransmit();
    emit pttChanged(hotspotIndex, true);
    return true;
}

void HotspotManager::releasePtt(int hotspotIndex)
{
    if (m_activeTxIndex != hotspotIndex)
        return;

    Hotspot *hs = hotspot(hotspotIndex);
    if (hs)
        hs->stopTransmit();

    m_activeTxIndex = -1;
    emit pttChanged(hotspotIndex, false);
}

void HotspotManager::addHotspot(const Hotspot::Config &cfg)
{
    auto *hs = new Hotspot(cfg, this);
    int index = m_hotspots.size();

    connect(hs, &Hotspot::logMessage, this, &HotspotManager::logMessage);

    m_hotspots.append(hs);
    emit hotspotAdded(index, hs);

    emit logMessage(QString("Added hotspot [%1] %2 (DMR ID %3) -> %4:%5")
                        .arg(index)
                        .arg(cfg.name)
                        .arg(cfg.dmrId)
                        .arg(cfg.host)
                        .arg(cfg.port));
}
