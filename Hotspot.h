#ifndef HOTSPOT_H
#define HOTSPOT_H

#include <QObject>
#include <QUdpSocket>
#include <QTimer>
#include <QHostAddress>
#include <QByteArray>

class AmbeEncoder;

// Represents a single DMR hotspot connection using the MMDVM Homebrew protocol.
// Implements the full login handshake (RPTL → RPTK → RPTC → RPTO),
// keepalive (RPTPING/MSTPONG), and DMRD voice frame TX/RX.
class Hotspot : public QObject
{
    Q_OBJECT

public:
    // Connection state machine matching MMDVM handshake phases
    enum class State {
        Disconnected,
        LoginSent,      // RPTL sent, waiting for salt
        AuthSent,       // RPTK sent, waiting for ACK
        ConfigSent,     // RPTC sent, waiting for ACK
        OptionsSent,    // RPTO sent, waiting for ACK
        Connected       // Fully authenticated, keepalive active
    };
    Q_ENUM(State)

    struct Config {
        QString name;
        QString host;
        quint16 port = 0;
        int     talkgroup = 0;
        quint32 dmrId = 0;         // Repeater DMR ID for this hotspot instance
        quint32 srcDmrId = 0;      // User's source DMR ID (used in TX voice frames)
        QString callsign;          // Shared callsign (e.g. "OK1KKY")
        QString password;          // Server password
        QString options;           // MMDVM options string (e.g. "TS1=;TS2=446;...")
        int     configIndex = -1;  // Position in config.json hotspots array
    };

    explicit Hotspot(const Config &cfg, QObject *parent = nullptr);
    ~Hotspot() override;

    // Accessors
    QString name() const { return m_config.name; }
    int talkgroup() const { return m_config.talkgroup; }
    quint32 dmrId() const { return m_config.dmrId; }
    int configIndex() const { return m_config.configIndex; }
    State state() const { return m_state; }
    bool isConnected() const { return m_state == State::Connected; }
    bool isTx() const { return m_transmitting; }
    bool rxEnabled() const { return m_rxEnabled; }

    // Dynamic TX talkgroup (overrides config talkgroup)
    int txTalkgroup() const;
    void setTxTalkgroup(int tg);

    // Buffer PCM audio for TX encoding
    void sendAudioData(const QByteArray &pcm);

public slots:
    void connectToServer();
    void disconnectFromServer();
    void setRxEnabled(bool enabled);

    // PTT control — called by HotspotManager which enforces single-TX
    void startTransmit();
    void stopTransmit();

signals:
    void stateChanged(Hotspot::State newState);
    void transmittingChanged(bool tx);
    void audioDataReceived(const QByteArray &data);
    void voiceStreamEnded();          // Emitted on Voice Terminator (reset decoder)
    void voiceStreamStarted(quint32 srcId, quint32 dstId);  // Header with caller + target TG
    void logMessage(const QString &msg);

private slots:
    void onReadyRead();
    void onKeepalive();
    void onTxTimer();

private:
    void setState(State s);
    void sendPacket(const QByteArray &data);

    // MMDVM Homebrew protocol packet builders
    QByteArray buildLoginPacket();          // RPTL + repeater ID
    QByteArray buildAuthPacket();           // RPTK + repeater ID + SHA256(salt+pass)
    QByteArray buildConfigPacket();         // RPTC + repeater ID + config fields (302 B)
    QByteArray buildOptionsPacket();        // RPTO + repeater ID + options string
    QByteArray buildPingPacket();           // RPTPING + repeater ID
    QByteArray buildDisconnectPacket();     // RPTCL + repeater ID

    // Build a 53-byte DMRD voice/data frame
    QByteArray buildDmrdPacket(const QByteArray &dmrPayload,
                               bool isHeader, bool isTerminator);

    // Incoming packet dispatcher
    void processDatagram(const QByteArray &data);

    // Binary encoding helpers
    static QByteArray uint32BE(quint32 val);
    static QByteArray uint24BE(quint32 val);
    static QByteArray ljust(const QString &str, int len, char pad = ' ');
    static QByteArray rjust(const QString &str, int len, char pad = '0');

    Config m_config;
    QUdpSocket *m_socket = nullptr;
    QTimer *m_keepaliveTimer = nullptr;
    QTimer *m_txTimer = nullptr;            // Periodic TX voice frame timer (60 ms)
    State m_state = State::Disconnected;
    bool m_transmitting = false;
    bool m_rxEnabled = true;
    QHostAddress m_serverAddress;

    // Protocol state
    QByteArray m_salt;                      // 4-byte salt received from server
    quint8 m_txSequence = 0;                // DMRD packet sequence counter (wraps 0-255)
    quint32 m_txStreamId = 0;               // Random stream ID per TX session
    int m_txFrameCount = 0;                 // Voice frame counter within current TX
    int m_rxSkipFrames = 0;                 // Skip initial voice frames after stream header

    // TX audio pipeline
    AmbeEncoder *m_encoder = nullptr;
    QByteArray m_txPcmBuffer;               // PCM data waiting to be encoded
    int m_txTalkgroup = 0;                  // Dynamic TX TG (0 = use config)
};

#endif // HOTSPOT_H
