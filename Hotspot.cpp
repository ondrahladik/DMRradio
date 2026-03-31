#include "Hotspot.h"
#include "AmbeEncoder.h"
#include <QDebug>
#include <QHostInfo>
#include <QCryptographicHash>
#include <QRandomGenerator>

static constexpr int KEEPALIVE_INTERVAL_MS = 5000;  // 5 seconds
static constexpr int TX_FRAME_INTERVAL_MS  = 60;    // DMR voice frame interval (~60 ms)

// ──────────────────────────────────────────────
//  Binary encoding helpers
// ──────────────────────────────────────────────

QByteArray Hotspot::uint32BE(quint32 val)
{
    QByteArray ba(4, '\0');
    ba[0] = static_cast<char>((val >> 24) & 0xFF);
    ba[1] = static_cast<char>((val >> 16) & 0xFF);
    ba[2] = static_cast<char>((val >>  8) & 0xFF);
    ba[3] = static_cast<char>((val      ) & 0xFF);
    return ba;
}

QByteArray Hotspot::uint24BE(quint32 val)
{
    QByteArray ba(3, '\0');
    ba[0] = static_cast<char>((val >> 16) & 0xFF);
    ba[1] = static_cast<char>((val >>  8) & 0xFF);
    ba[2] = static_cast<char>((val      ) & 0xFF);
    return ba;
}

QByteArray Hotspot::ljust(const QString &str, int len, char pad)
{
    QByteArray ba = str.toUtf8().left(len);
    while (ba.size() < len)
        ba.append(pad);
    return ba;
}

QByteArray Hotspot::rjust(const QString &str, int len, char pad)
{
    QByteArray ba = str.toUtf8().left(len);
    while (ba.size() < len)
        ba.prepend(pad);
    return ba;
}

// ──────────────────────────────────────────────
//  Constructor / Destructor
// ──────────────────────────────────────────────

Hotspot::Hotspot(const Config &cfg, QObject *parent)
    : QObject(parent)
    , m_config(cfg)
{
    m_socket = new QUdpSocket(this);
    connect(m_socket, &QUdpSocket::readyRead, this, &Hotspot::onReadyRead);

    m_keepaliveTimer = new QTimer(this);
    m_keepaliveTimer->setInterval(KEEPALIVE_INTERVAL_MS);
    connect(m_keepaliveTimer, &QTimer::timeout, this, &Hotspot::onKeepalive);

    m_txTimer = new QTimer(this);
    m_txTimer->setInterval(TX_FRAME_INTERVAL_MS);
    connect(m_txTimer, &QTimer::timeout, this, &Hotspot::onTxTimer);

    m_rxSilenceTimer = new QTimer(this);
    m_rxSilenceTimer->setSingleShot(true);
    m_rxSilenceTimer->setInterval(RX_SILENCE_TIMEOUT_MS);
    connect(m_rxSilenceTimer, &QTimer::timeout, this, &Hotspot::onRxSilenceTimeout);

    m_encoder = new AmbeEncoder();
}

Hotspot::~Hotspot()
{
    disconnectFromServer();
    delete m_encoder;
}

// ──────────────────────────────────────────────
//  Connection management (MMDVM handshake)
// ──────────────────────────────────────────────

void Hotspot::connectToServer()
{
    if (m_state != State::Disconnected)
        return;

    m_socket->blockSignals(false);

    // Resolve host
    QHostInfo info = QHostInfo::fromName(m_config.host);
    if (info.addresses().isEmpty()) {
        emit logMessage(QString("[%1] DNS resolution failed for %2")
                            .arg(m_config.name, m_config.host));
        return;
    }
    m_serverAddress = info.addresses().first();

    // Bind socket to any available local port
    if (m_socket->state() != QAbstractSocket::BoundState) {
        if (!m_socket->bind(QHostAddress::Any, 0)) {
            emit logMessage(QString("[%1] Failed to bind UDP socket: %2")
                                .arg(m_config.name, m_socket->errorString()));
            return;
        }
    }

    // Step 1: Send RPTL login packet
    sendPacket(buildLoginPacket());
    setState(State::LoginSent);

    emit logMessage(QString("[%1] LOGIN SENT → %2:%3 (DMR ID %4)")
                        .arg(m_config.name, m_serverAddress.toString())
                        .arg(m_config.port)
                        .arg(m_config.dmrId));
}

void Hotspot::disconnectFromServer()
{
    if (m_state == State::Disconnected)
        return;

    stopTransmit();
    m_keepaliveTimer->stop();

    // Send RPTCL disconnect packet
    sendPacket(buildDisconnectPacket());

    m_socket->blockSignals(true);
    m_socket->close();
    setState(State::Disconnected);

    // Clear any active RX stream on disconnect
    if (m_rxStreamActive) {
        m_rxStreamActive = false;
        m_rxSilenceTimer->stop();
        emit voiceStreamEnded();
    }

    emit logMessage(QString("[%1] DISCONNECTED").arg(m_config.name));
}

void Hotspot::setRxEnabled(bool enabled)
{
    m_rxEnabled = enabled;
    emit logMessage(QString("[%1] RX %2").arg(m_config.name, enabled ? "enabled" : "disabled"));
}

// ──────────────────────────────────────────────
//  PTT / TX
// ──────────────────────────────────────────────

void Hotspot::startTransmit()
{
    if (m_state != State::Connected || m_transmitting)
        return;

    m_transmitting = true;
    m_txStreamId = QRandomGenerator::global()->generate();
    m_txFrameCount = 0;
    m_txSequence = 0;
    m_txPcmBuffer.clear();
    m_encoder->reset();
    emit transmittingChanged(true);

    // Send voice LC header
    QByteArray headerPayload(33, '\0');
    sendPacket(buildDmrdPacket(headerPayload, true, false));

    emit logMessage(QString("[%1] PTT ON — TX START TG %2 (stream 0x%3, src DMR ID %4)")
                        .arg(m_config.name)
                        .arg(txTalkgroup())
                        .arg(m_txStreamId, 8, 16, QChar('0'))
                        .arg(m_config.srcDmrId));

    // Start periodic voice frame timer
    m_txTimer->start();
}

void Hotspot::stopTransmit()
{
    if (!m_transmitting)
        return;

    m_txTimer->stop();

    // Send voice LC terminator
    QByteArray termPayload(33, '\0');
    sendPacket(buildDmrdPacket(termPayload, false, true));

    m_transmitting = false;
    emit transmittingChanged(false);

    emit logMessage(QString("[%1] PTT OFF — TX STOP (%2 voice frames sent)")
                        .arg(m_config.name)
                        .arg(m_txFrameCount));
}

void Hotspot::sendAudioData(const QByteArray &pcm)
{
    if (!m_transmitting)
        return;

    m_txPcmBuffer.append(pcm);

    // Cap buffer to ~300ms (prevent latency buildup)
    static constexpr int MAX_TX_BUFFER = 4800; // 300ms at 8kHz/16bit/mono
    if (m_txPcmBuffer.size() > MAX_TX_BUFFER)
        m_txPcmBuffer = m_txPcmBuffer.right(MAX_TX_BUFFER);
}

int Hotspot::txTalkgroup() const
{
    return m_txTalkgroup > 0 ? m_txTalkgroup : m_config.talkgroup;
}

void Hotspot::setTxTalkgroup(int tg)
{
    m_txTalkgroup = tg;
}

void Hotspot::onTxTimer()
{
    if (!m_transmitting || m_state != State::Connected)
        return;

    // Need exactly 960 bytes (3×160 samples) for one DMR burst
    static constexpr int BURST_PCM_SIZE = 960;

    QByteArray burst;
    if (m_txPcmBuffer.size() >= BURST_PCM_SIZE) {
        QByteArray pcm = m_txPcmBuffer.left(BURST_PCM_SIZE);
        m_txPcmBuffer.remove(0, BURST_PCM_SIZE);
        burst = m_encoder->encodeBurst(pcm);

        if (m_txFrameCount < 3)
            qDebug() << QString("[%1] TX voice burst #%2 (buf %3 bytes)")
                            .arg(m_config.name).arg(m_txFrameCount).arg(m_txPcmBuffer.size());
    } else {
        // No audio yet — send silence (limit to first 2 frames)
        burst = m_encoder->silenceBurst();
        if (m_txFrameCount < 5)
            qDebug() << QString("[%1] TX silence #%2 (buf %3 bytes)")
                            .arg(m_config.name).arg(m_txFrameCount).arg(m_txPcmBuffer.size());
    }

    sendPacket(buildDmrdPacket(burst, false, false));
    m_txFrameCount++;
}

// ──────────────────────────────────────────────
//  Keepalive
// ──────────────────────────────────────────────

void Hotspot::onKeepalive()
{
    if (m_state != State::Connected)
        return;

    sendPacket(buildPingPacket());
}

// ──────────────────────────────────────────────
//  Incoming data
// ──────────────────────────────────────────────

void Hotspot::onReadyRead()
{
    if (m_socket->state() != QAbstractSocket::BoundState)
        return;

    while (m_socket->hasPendingDatagrams()) {
        QByteArray datagram;
        datagram.resize(m_socket->pendingDatagramSize());
        QHostAddress sender;
        quint16 senderPort;
        m_socket->readDatagram(datagram.data(), datagram.size(), &sender, &senderPort);
        processDatagram(datagram);
    }
}

void Hotspot::processDatagram(const QByteArray &data)
{
    if (data.size() < 4)
        return;

    // ── RPTACK: Server acknowledgement (at various handshake stages) ──
    if (data.startsWith("RPTACK")) {
        if (m_state == State::LoginSent && data.size() >= 10) {
            // Response to RPTL: RPTACK + 4-byte salt
            m_salt = data.mid(6, 4);
            emit logMessage(QString("[%1] LOGIN ACK — salt 0x%2")
                                .arg(m_config.name, m_salt.toHex()));

            // Step 2: Send RPTK auth with SHA256(salt + password)
            sendPacket(buildAuthPacket());
            setState(State::AuthSent);
            emit logMessage(QString("[%1] AUTH SENT").arg(m_config.name));
        }
        else if (m_state == State::AuthSent) {
            emit logMessage(QString("[%1] AUTH ACK — authenticated ✓").arg(m_config.name));

            // Step 3: Send RPTC config
            sendPacket(buildConfigPacket());
            setState(State::ConfigSent);
            emit logMessage(QString("[%1] CONFIG SENT (%2 bytes)")
                                .arg(m_config.name)
                                .arg(buildConfigPacket().size()));
        }
        else if (m_state == State::ConfigSent) {
            emit logMessage(QString("[%1] CONFIG ACK").arg(m_config.name));

            if (!m_config.options.isEmpty()) {
                // Step 4: Send RPTO options
                sendPacket(buildOptionsPacket());
                setState(State::OptionsSent);
                emit logMessage(QString("[%1] OPTIONS SENT: %2")
                                    .arg(m_config.name, m_config.options));
            } else {
                // No options — go straight to Connected
                setState(State::Connected);
                m_keepaliveTimer->start();
                emit logMessage(QString("[%1] CONNECTED ✓ (no options)").arg(m_config.name));
            }
        }
        else if (m_state == State::OptionsSent) {
            setState(State::Connected);
            m_keepaliveTimer->start();
            emit logMessage(QString("[%1] OPTIONS ACK — CONNECTED ✓").arg(m_config.name));
        }
        return;
    }

    // ── MSTNAK: Server rejection ──
    if (data.startsWith("MSTNAK")) {
        emit logMessage(QString("[%1] ✗ SERVER NAK — rejected in state %2 after %3 (%4 bytes)")
                            .arg(m_config.name)
                            .arg(static_cast<int>(m_state))
                            .arg(m_lastSentTag.isEmpty() ? QStringLiteral("unknown") : m_lastSentTag)
                            .arg(m_lastSentSize));
        stopTransmit();
        m_keepaliveTimer->stop();
        m_socket->blockSignals(true);
        m_socket->close();
        setState(State::Disconnected);
        QTimer::singleShot(1000, this, &Hotspot::connectToServer);
        return;
    }

    // ── MSTCL: Server closed connection ──
    if (data.startsWith("MSTCL")) {
        emit logMessage(QString("[%1] ✗ SERVER CLOSE — connection terminated by master")
                            .arg(m_config.name));
        stopTransmit();
        m_keepaliveTimer->stop();
        m_socket->blockSignals(true);
        m_socket->close();
        setState(State::Disconnected);
        QTimer::singleShot(1000, this, &Hotspot::connectToServer);
        return;
    }

    // ── MSTPONG: Keepalive response ──
    if (data.startsWith("MSTPONG")) {
        // Silently acknowledge — don't flood the log
        return;
    }

    // ── DMRD: Incoming DMR voice/data frame (53 bytes) ──
    if (data.startsWith("DMRD") && data.size() >= 53) {
        quint8  seq   = static_cast<quint8>(data[4]);
        quint32 srcId = (static_cast<quint8>(data[5]) << 16) |
                        (static_cast<quint8>(data[6]) <<  8) |
                         static_cast<quint8>(data[7]);
        quint32 dstId = (static_cast<quint8>(data[8]) << 16) |
                        (static_cast<quint8>(data[9]) <<  8) |
                         static_cast<quint8>(data[10]);
        quint8  flags = static_cast<quint8>(data[15]);
        int     slot  = (flags & 0x80) ? 2 : 1;
        bool    group = !(flags & 0x40);
        quint8  ftype = (flags >> 4) & 0x03;

        QString frameDesc;
        if (ftype == 0x02) {
            quint8 dt = flags & 0x0F;
            if (dt == 0x01)      frameDesc = "Voice Header";
            else if (dt == 0x02) frameDesc = "Voice Terminator";
            else                 frameDesc = QString("Data (type %1)").arg(dt);

            if (dt == 0x01 || dt == 0x02) {
                m_rxSkipFrames = (dt == 0x01) ? 3 : 0;
                if (dt == 0x01) {
                    m_rxLastSrcId = srcId;
                    m_rxLastDstId = dstId;
                    m_rxStreamActive = true;
                    m_rxSilenceTimer->start();
                    emit voiceStreamStarted(srcId, dstId);
                } else {
                    m_rxStreamActive = false;
                    m_rxSilenceTimer->stop();
                    emit voiceStreamEnded();
                }
            }
        } else {
            frameDesc = QString("Voice seq=%1").arg(flags & 0x0F);
        }

        bool isVoiceFrame = (ftype == 0x00 || ftype == 0x01);
        if (!isVoiceFrame || (flags & 0x0F) == 0) {
            emit logMessage(QString("[%1] RX DMRD: seq=%2 src=%3 dst=%4 TS%5 %6 %7")
                                .arg(m_config.name)
                                .arg(seq)
                                .arg(srcId)
                                .arg(dstId)
                                .arg(slot)
                                .arg(group ? "Group" : "Private", frameDesc));
        }

        // Only forward actual voice frames to the audio pipeline
        // (skip headers, terminators, and other data sync frames)
        if (m_rxEnabled && isVoiceFrame) {
            if (m_rxSkipFrames > 0) {
                m_rxSkipFrames--;
                return;
            }

            // Recover from a lost voice header packet
            if (!m_rxStreamActive) {
                m_rxLastSrcId = srcId;
                m_rxLastDstId = dstId;
                m_rxStreamActive = true;
                emit voiceStreamStarted(srcId, dstId);
            }

            m_rxSilenceTimer->start();

            QByteArray ambePayload = data.mid(20, 33);
            emit audioDataReceived(ambePayload);
        }
        return;
    }

    // ── Unknown packet ──
    emit logMessage(QString("[%1] RX UNKNOWN: tag=0x%2 size=%3")
                        .arg(m_config.name,
                             data.left(6).toHex(),
                             QString::number(data.size())));
}

// ──────────────────────────────────────────────
//  MMDVM Homebrew packet builders
// ──────────────────────────────────────────────

QByteArray Hotspot::buildLoginPacket()
{
    // RPTL(4) + RepeaterID(4) = 8 bytes
    QByteArray pkt;
    pkt.append("RPTL", 4);
    pkt.append(uint32BE(m_config.dmrId));
    return pkt;
}

QByteArray Hotspot::buildAuthPacket()
{
    // RPTK(4) + RepeaterID(4) + SHA256(salt + password)(32) = 40 bytes
    QByteArray hashInput = m_salt + m_config.password.toUtf8();
    QByteArray hash = QCryptographicHash::hash(hashInput, QCryptographicHash::Sha256);

    QByteArray pkt;
    pkt.append("RPTK", 4);
    pkt.append(uint32BE(m_config.dmrId));
    pkt.append(hash);
    return pkt;
}

QByteArray Hotspot::buildConfigPacket()
{
    // RPTC(4) + ID(4) + Callsign(8) + RxFreq(9) + TxFreq(9) + Power(2)
    //   + ColorCode(2) + Lat(8) + Lon(9) + Height(3) + Location(20)
    //   + Description(19) + Slots(1) + URL(124) + SoftwareID(40) + PackageID(40)
    // Total: 302 bytes
    QByteArray pkt;
    pkt.append("RPTC", 4);
    pkt.append(uint32BE(m_config.dmrId));
    pkt.append(ljust(m_config.callsign, 8));        // Callsign
    pkt.append(ljust("446000000", 9, '0'));          // RX frequency (Hz)
    pkt.append(ljust("446000000", 9, '0'));          // TX frequency (Hz)
    pkt.append(rjust("1", 2, '0'));                  // TX power
    pkt.append(rjust("1", 2, '0'));                  // Color code
    pkt.append(ljust("0.0000", 8));                  // Latitude
    pkt.append(ljust("0.0000", 9));                  // Longitude
    pkt.append(rjust("0", 3, '0'));                  // Height (m)
    pkt.append(ljust("", 20));                       // Location
    pkt.append(ljust("DMR MultiHS", 19));            // Description
    pkt.append(ljust("2", 1));                       // Slots (TS2)
    pkt.append(ljust("", 124));                      // URL
    pkt.append(ljust("DMR-MultiHS", 40));            // Software ID
    pkt.append(ljust("v0.1", 40));                   // Package ID
    return pkt;
}

QByteArray Hotspot::buildOptionsPacket()
{
    // RPTO(4) + RepeaterID(4) + Options(variable)
    QByteArray pkt;
    pkt.append("RPTO", 4);
    pkt.append(uint32BE(m_config.dmrId));
    pkt.append(m_config.options.toUtf8());
    return pkt;
}

QByteArray Hotspot::buildPingPacket()
{
    // RPTPING(7) + RepeaterID(4) = 11 bytes
    QByteArray pkt;
    pkt.append("RPTPING", 7);
    pkt.append(uint32BE(m_config.dmrId));
    return pkt;
}

QByteArray Hotspot::buildDisconnectPacket()
{
    // RPTCL(5) + RepeaterID(4) = 9 bytes
    QByteArray pkt;
    pkt.append("RPTCL", 5);
    pkt.append(uint32BE(m_config.dmrId));
    return pkt;
}

QByteArray Hotspot::buildDmrdPacket(const QByteArray &dmrPayload,
                                     bool isHeader, bool isTerminator)
{
    // DMRD(4) + Seq(1) + SrcID(3) + DstID(3) + RptrID(4)
    //   + Flags(1) + StreamID(4) + DMR(33) = 53 bytes
    //
    // Flags byte layout:
    //   Bit 7: Slot (0=TS1, 1=TS2)
    //   Bit 6: Call type (0=Group, 1=Private)
    //   Bits 5-4: Frame type (00=Voice, 01=VoiceSync, 10=DataSync)
    //   Bits 3-0: Voice sequence (0-5) or data type
    quint8 flags = 0x80;  // TS2, Group call

    if (isHeader) {
        flags |= 0x20 | 0x01;  // DataSync + Voice LC Header
    } else if (isTerminator) {
        flags |= 0x20 | 0x02;  // DataSync + Voice LC Terminator
    } else {
        quint8 voiceSeq = static_cast<quint8>(m_txFrameCount % 6);
        if (voiceSeq == 0)
            flags |= 0x10;         // VoiceSync for first frame in superframe
        else
            flags |= voiceSeq;     // Voice with sequence number
    }

    QByteArray pkt;
    pkt.append("DMRD", 4);
    pkt.append(static_cast<char>(m_txSequence++));
    pkt.append(uint24BE(m_config.srcDmrId));                                // Source ID (user's DMR ID)
    pkt.append(uint24BE(static_cast<quint32>(txTalkgroup())));              // Destination ID (dynamic TG)
    pkt.append(uint32BE(m_config.dmrId));                                   // Repeater ID
    pkt.append(static_cast<char>(flags));
    pkt.append(uint32BE(m_txStreamId));

    // Ensure exactly 33 bytes of DMR payload
    QByteArray payload = dmrPayload.left(33);
    while (payload.size() < 33)
        payload.append('\0');
    pkt.append(payload);

    return pkt;
}

// ──────────────────────────────────────────────
//  State management
// ──────────────────────────────────────────────

void Hotspot::setState(State s)
{
    if (m_state == s)
        return;
    m_state = s;
    emit stateChanged(m_state);
}

void Hotspot::sendPacket(const QByteArray &data)
{
    if (m_socket->state() != QAbstractSocket::BoundState) {
        emit logMessage(QString("[%1] ! SEND skipped — UDP socket not bound (%2)")
                            .arg(m_config.name, m_socket->errorString()));
        return;
    }

    if (data.startsWith("RPTPING"))
        m_lastSentTag = "RPTPING";
    else if (data.startsWith("RPTCL"))
        m_lastSentTag = "RPTCL";
    else if (data.startsWith("RPTL"))
        m_lastSentTag = "RPTL";
    else if (data.startsWith("RPTK"))
        m_lastSentTag = "RPTK";
    else if (data.startsWith("RPTC"))
        m_lastSentTag = "RPTC";
    else if (data.startsWith("RPTO"))
        m_lastSentTag = "RPTO";
    else if (data.startsWith("DMRD"))
        m_lastSentTag = "DMRD";
    else
        m_lastSentTag = "unknown";
    m_lastSentSize = data.size();
    m_socket->writeDatagram(data, m_serverAddress, m_config.port);
}

void Hotspot::onRxSilenceTimeout()
{
    if (!m_rxStreamActive)
        return;

    m_rxStreamActive = false;
    emit logMessage(QString("[%1] RX stream timed out — clearing caller info").arg(m_config.name));
    emit voiceStreamEnded();
}
