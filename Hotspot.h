#ifndef HOTSPOT_H
#define HOTSPOT_H

#include <QObject>
#include <QUdpSocket>
#include <QTimer>
#include <QHostAddress>
#include <QByteArray>

class AmbeEncoder;

class Hotspot : public QObject
{
    Q_OBJECT

public:
    enum class State {
        Disconnected,
        LoginSent,
        AuthSent,
        ConfigSent,
        OptionsSent,
        Connected
    };
    Q_ENUM(State)

    struct Config {
        QString name;
        QString host;
        quint16 port = 0;
        int     talkgroup = 0;
        quint32 dmrId = 0;
        quint32 srcDmrId = 0;
        QString callsign;
        QString password;
        QString options;
        int     configIndex = -1;
    };

    explicit Hotspot(const Config &cfg, QObject *parent = nullptr);
    ~Hotspot() override;

    QString name() const { return m_config.name; }
    int talkgroup() const { return m_config.talkgroup; }
    quint32 dmrId() const { return m_config.dmrId; }
    int configIndex() const { return m_config.configIndex; }
    State state() const { return m_state; }
    bool isConnected() const { return m_state == State::Connected; }
    bool isTx() const { return m_transmitting; }
    bool rxEnabled() const { return m_rxEnabled; }
    bool isRxStreamActive() const { return m_rxStreamActive; }

    int txTalkgroup() const;
    void setTxTalkgroup(int tg);

    bool isPrivateCall() const { return m_privateCall; }
    void setPrivateCall(bool priv) { m_privateCall = priv; }

    void sendAudioData(const QByteArray &pcm);

public slots:
    void connectToServer();
    void disconnectFromServer();
    void setRxEnabled(bool enabled);
    void startTransmit();
    void stopTransmit();

signals:
    void stateChanged(Hotspot::State newState);
    void transmittingChanged(bool tx);
    void audioDataReceived(const QByteArray &data);
    void voiceStreamEnded();
    void voiceStreamStarted(quint32 srcId, quint32 dstId);
    void logMessage(const QString &msg);

private slots:
    void onReadyRead();
    void onKeepalive();
    void onTxTimer();
    void onRxSilenceTimeout();

private:
    void setState(State s);
    void sendPacket(const QByteArray &data);
    void sendVoiceBurst(const QByteArray &pcm);
    void flushTxBuffer(bool padPartialBurst);

    QByteArray buildLoginPacket();
    QByteArray buildAuthPacket();
    QByteArray buildConfigPacket();
    QByteArray buildOptionsPacket();
    QByteArray buildPingPacket();
    QByteArray buildDisconnectPacket();
    QByteArray buildDmrdPacket(const QByteArray &dmrPayload, bool isHeader, bool isTerminator);
    void processDatagram(const QByteArray &data);

    static QByteArray uint32BE(quint32 val);
    static QByteArray uint24BE(quint32 val);
    static QByteArray ljust(const QString &str, int len, char pad = ' ');
    static QByteArray rjust(const QString &str, int len, char pad = '0');

    Config m_config;
    QUdpSocket *m_socket = nullptr;
    QTimer *m_keepaliveTimer = nullptr;
    QTimer *m_txTimer = nullptr;
    State m_state = State::Disconnected;
    bool m_transmitting = false;
    bool m_rxEnabled = true;
    QHostAddress m_serverAddress;

    QByteArray m_salt;
    quint8 m_txSequence = 0;
    quint32 m_txStreamId = 0;
    int m_txFrameCount = 0;
    int m_rxSkipFrames = 0;

    AmbeEncoder *m_encoder = nullptr;
    QByteArray m_txPcmBuffer;
    int m_txTalkgroup = 0;
    bool m_privateCall = false;
    QString m_lastSentTag;
    int m_lastSentSize = 0;

    // RX stream state — recovers from lost header/terminator UDP packets
    bool    m_rxStreamActive = false;
    quint32 m_rxLastSrcId = 0;
    quint32 m_rxLastDstId = 0;
    QTimer *m_rxSilenceTimer = nullptr;
    static constexpr int RX_SILENCE_TIMEOUT_MS = 4000;
};

#endif // HOTSPOT_H
