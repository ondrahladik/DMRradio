#ifndef AMBEENCODER_H
#define AMBEENCODER_H

#include <QByteArray>

class MBEEncoder;

// Real AMBE+2 encoder for DMR voice transmission.
//
// Uses the OP25 MBEEncoder (via OpenDMR) for actual voice parameter
// extraction from PCM, plus Golay FEC encoding and DMR burst interleaving.
//
// Pipeline: PCM (160 samples @ 8kHz) → IMBE analysis → voice params →
//           Golay FEC → PRNG scrambling → DMR burst interleaving
class AmbeEncoder
{
public:
    AmbeEncoder();
    ~AmbeEncoder();

    // Encode 960 bytes of PCM (3 × 160 samples @ 8kHz 16-bit mono, 60ms)
    // into a 33-byte DMR voice burst (3 interleaved AMBE+2 codewords).
    QByteArray encodeBurst(const QByteArray &pcm960);

    // Pre-computed silence burst (33 bytes) for when no audio is available.
    const QByteArray &silenceBurst() const { return m_silenceBurst; }

    // Reset encoder state (call between TX sessions).
    void reset();

private:
    void buildSilenceBurst();

    MBEEncoder *m_mbeEncoder;
    QByteArray m_silenceBurst;
    int m_frameCount = 0;
};

#endif // AMBEENCODER_H
