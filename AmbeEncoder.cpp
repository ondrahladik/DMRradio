#include "AmbeEncoder.h"
#include "mbeenc.h"
#include <cstring>
#include <cstdint>
#include <QDebug>

// Known AMBE+2 silence codeword (72 bits = 9 bytes, FEC-encoded).
static const unsigned char AMBE_SILENCE_CW[9] = {
    0xB9, 0xE8, 0x81, 0x52, 0x61, 0x73, 0x00, 0x2A, 0x6B
};

static inline int readBit(const unsigned char *data, unsigned int pos)
{
    return (data[pos >> 3] >> (7 - (pos & 7))) & 1;
}

static inline void setBit(unsigned char *data, unsigned int pos)
{
    data[pos >> 3] |= static_cast<unsigned char>(1U << (7 - (pos & 7)));
}

// Pack b[9] voice parameters into 49-bit stream using the correct
// DMR AMBE+2 bit ordering.  MSBs of each parameter come first (protected
// by Golay FEC in the A/B blocks); LSBs are grouped at the end (C block).
// This ordering MUST match OP25/OpenDMR encode_49bit exactly.
static void packVoiceParams(unsigned char packed[7], const int b[9])
{
    uint8_t bits[49];
    // MSBs — protected by Golay FEC
    bits[0]  = (b[0] >> 6) & 1;
    bits[1]  = (b[0] >> 5) & 1;
    bits[2]  = (b[0] >> 4) & 1;
    bits[3]  = (b[0] >> 3) & 1;
    bits[4]  = (b[1] >> 4) & 1;
    bits[5]  = (b[1] >> 3) & 1;
    bits[6]  = (b[1] >> 2) & 1;
    bits[7]  = (b[1] >> 1) & 1;
    bits[8]  = (b[2] >> 4) & 1;
    bits[9]  = (b[2] >> 3) & 1;
    bits[10] = (b[2] >> 2) & 1;
    bits[11] = (b[2] >> 1) & 1;
    bits[12] = (b[3] >> 8) & 1;
    bits[13] = (b[3] >> 7) & 1;
    bits[14] = (b[3] >> 6) & 1;
    bits[15] = (b[3] >> 5) & 1;
    bits[16] = (b[3] >> 4) & 1;
    bits[17] = (b[3] >> 3) & 1;
    bits[18] = (b[3] >> 2) & 1;
    bits[19] = (b[3] >> 1) & 1;
    bits[20] = (b[4] >> 6) & 1;
    bits[21] = (b[4] >> 5) & 1;
    bits[22] = (b[4] >> 4) & 1;
    bits[23] = (b[4] >> 3) & 1;
    bits[24] = (b[5] >> 4) & 1;
    bits[25] = (b[5] >> 3) & 1;
    bits[26] = (b[5] >> 2) & 1;
    bits[27] = (b[5] >> 1) & 1;
    bits[28] = (b[6] >> 3) & 1;
    bits[29] = (b[6] >> 2) & 1;
    bits[30] = (b[6] >> 1) & 1;
    bits[31] = (b[7] >> 3) & 1;
    bits[32] = (b[7] >> 2) & 1;
    bits[33] = (b[7] >> 1) & 1;
    bits[34] = (b[8] >> 2) & 1;
    // LSBs — lower priority, in C block (raw, no Golay)
    bits[35] = b[1] & 1;
    bits[36] = b[2] & 1;
    bits[37] = (b[0] >> 2) & 1;
    bits[38] = (b[0] >> 1) & 1;
    bits[39] = b[0] & 1;
    bits[40] = b[3] & 1;
    bits[41] = (b[4] >> 2) & 1;
    bits[42] = (b[4] >> 1) & 1;
    bits[43] = b[4] & 1;
    bits[44] = b[5] & 1;
    bits[45] = b[6] & 1;
    bits[46] = b[7] & 1;
    bits[47] = (b[8] >> 1) & 1;
    bits[48] = b[8] & 1;

    std::memset(packed, 0, 7);
    for (int i = 0; i < 49; i++)
        if (bits[i])
            packed[i >> 3] |= static_cast<unsigned char>(1U << (7 - (i & 7)));
}

// Place a 72-bit (9-byte) FEC-encoded codeword into a 33-byte DMR burst.
// cwIdx 0 → burst bits 0-71, cwIdx 1 → 72-107+156-191, cwIdx 2 → 192-263.
static void placeCwIntoBurst(unsigned char *burst, int cwIdx,
                             const unsigned char *cwData)
{
    for (int i = 0; i < 72; i++) {
        if (!readBit(cwData, static_cast<unsigned>(i)))
            continue;
        unsigned int dstPos;
        if (cwIdx == 0)
            dstPos = static_cast<unsigned>(i);
        else if (cwIdx == 1)
            dstPos = 72U + static_cast<unsigned>(i < 36 ? i : i + 48);
        else
            dstPos = 192U + static_cast<unsigned>(i);
        setBit(burst, dstPos);
    }
}

AmbeEncoder::AmbeEncoder()
{
    m_mbeEncoder = new MBEEncoder();
    m_mbeEncoder->set_dmr_mode();
    m_mbeEncoder->set_gain_adjust(0.0f);  // No gain offset — encoder tracks natural levels
    buildSilenceBurst();
}

AmbeEncoder::~AmbeEncoder()
{
    delete m_mbeEncoder;
}

void AmbeEncoder::buildSilenceBurst()
{
    unsigned char burst[33];
    std::memset(burst, 0, 33);
    placeCwIntoBurst(burst, 0, AMBE_SILENCE_CW);
    placeCwIntoBurst(burst, 1, AMBE_SILENCE_CW);
    placeCwIntoBurst(burst, 2, AMBE_SILENCE_CW);
    m_silenceBurst = QByteArray(reinterpret_cast<char *>(burst), 33);
}

void AmbeEncoder::reset()
{
    delete m_mbeEncoder;
    m_mbeEncoder = new MBEEncoder();
    m_mbeEncoder->set_dmr_mode();
    m_mbeEncoder->set_gain_adjust(0.0f);  // No gain offset — encoder tracks natural levels
    m_frameCount = 0;
}

QByteArray AmbeEncoder::encodeBurst(const QByteArray &pcm960)
{
    if (pcm960.size() < 960)
        return m_silenceBurst;

    const auto *samples = reinterpret_cast<const int16_t *>(pcm960.constData());
    unsigned char burst[33];
    std::memset(burst, 0, 33);

    for (int cw = 0; cw < 3; cw++) {
        // Step 1: PCM → 9 voice parameters via IMBE vocoder
        int b[9] = {0};
        m_mbeEncoder->encode_dmr_params(samples + cw * 160, b);

        // Step 2: Pack params using correct AMBE+2 bit ordering
        unsigned char packed49[7];
        packVoiceParams(packed49, b);

        // Step 3: FEC encode via library (Golay + PRNG + codeword interleave)
        unsigned char cwOut[9];
        m_mbeEncoder->encode_dmr(packed49, cwOut);

        // Step 4: Place 72-bit codeword into correct burst position
        placeCwIntoBurst(burst, cw, cwOut);
    }

    m_frameCount++;
    if (m_frameCount <= 3)
        qDebug() << "AmbeEncoder: voice burst" << m_frameCount;

    return QByteArray(reinterpret_cast<char *>(burst), 33);
}
