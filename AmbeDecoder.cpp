#include "AmbeDecoder.h"
#include <QDebug>
#include <cstring>

// ──────────────────────────────────────────────
//  DMR AMBE+2 burst-level interleave tables
//  (ETSI TS 102 361-1, Annex B / MMDVM Host AMBEFEC.cpp)
//
//  A DMR voice burst (264 bits / 33 bytes) carries 3 AMBE+2
//  codewords whose bits are spread across the burst.  The tables
//  below give, for each FEC group of one codeword, the burst-
//  relative bit positions (0-71 within the first codeword region).
//
//  Codeword 0 starts at burst offset 0.
//  Codeword 1 starts at burst offset 72  (spans the 48-bit sync gap).
//  Codeword 2 starts at burst offset 192 (second voice segment).
//
//  Table entries are in MSB-first order: index 0 = most-significant
//  bit of the FEC group as transmitted over the air.
//
//  mbelib convention: ambe_fr[row][col] with HIGH col index = MSB.
//  Therefore, table index i must be stored at the REVERSED position
//  within ambe_fr (e.g. ambe_fr[0][23-i] for C0, ambe_fr[1][22-i]
//  for C1).
// ──────────────────────────────────────────────

// C0 (ambe_fr[0]): 24 bits — 1 parity + 23-bit Golay(23,12) codeword
static const unsigned int DMR_A_TABLE[] = {
     0U,  4U,  8U, 12U, 16U, 20U, 24U, 28U, 32U, 36U, 40U, 44U,
    48U, 52U, 56U, 60U, 64U, 68U,  1U,  5U,  9U, 13U, 17U, 21U
};

// C1 (ambe_fr[1]): 23 bits — Golay(23,12) codeword
static const unsigned int DMR_B_TABLE[] = {
    25U, 29U, 33U, 37U, 41U, 45U, 49U, 53U, 57U, 61U, 65U, 69U,
     2U,  6U, 10U, 14U, 18U, 22U, 26U, 30U, 34U, 38U, 42U
};

// C2 (ambe_fr[2]): 11 bits + C3 (ambe_fr[3]): 14 bits — unprotected
static const unsigned int DMR_C_TABLE[] = {
    46U, 50U, 54U, 58U, 62U, 66U, 70U,  3U,  7U, 11U, 15U, 19U,
    23U, 27U, 31U, 35U, 39U, 43U, 47U, 51U, 55U, 59U, 63U, 67U, 71U
};

// Read a single bit from a byte array (MSB-first / network byte order).
static inline int readBit(const unsigned char *data, unsigned int pos)
{
    return (data[pos >> 3] >> (7 - (pos & 7))) & 1;
}

// ──────────────────────────────────────────────
//  Constructor
// ──────────────────────────────────────────────

AmbeDecoder::AmbeDecoder()
{
    reset();
}

// ──────────────────────────────────────────────
//  Decode one DMR voice burst (33 bytes → 960 bytes PCM)
//
//  Pipeline per codeword:
//    1. extractAmbeCodeword()  — burst deinterleave → ambe_fr[4][24]
//    2. mbelib internally:
//       a. mbe_demodulateAmbe3600x2450Data  — AMBE demodulation
//       b. mbe_eccAmbe3600x2450C0/Data      — Golay FEC correction
//       c. mbe_decodeAmbe2450Parms          — parameter extraction
//       d. mbe_synthesizeSpeech             — speech synthesis
//    3. 160 PCM samples output per codeword
// ──────────────────────────────────────────────

QByteArray AmbeDecoder::decode(const QByteArray &dmrBurst)
{
    if (dmrBurst.size() < 33) {
        qWarning() << "AmbeDecoder: burst too short:" << dmrBurst.size() << "bytes";
        return {};
    }

    const auto *burst = reinterpret_cast<const unsigned char *>(dmrBurst.constData());
    QByteArray pcm;
    pcm.reserve(960);  // 3 codewords × 160 samples × 2 bytes

    int totalErrs = 0;
    for (int cw = 0; cw < 3; cw++) {
        char ambe_fr[4][24];
        char ambe_d[49];
        short aout_buf[160];
        int errs = 0, errs2 = 0;
        char err_str[64];
        std::memset(err_str, 0, sizeof(err_str));

        extractAmbeCodeword(burst, cw, ambe_fr);

        mbe_processAmbe3600x2450Frame(
            aout_buf, &errs, &errs2, err_str,
            ambe_fr, ambe_d,
            &m_curMp, &m_prevMp, &m_prevMpEnhanced,
            3);  // uvquality = 3 (good quality/speed balance)

        totalErrs += errs + errs2;
        pcm.append(reinterpret_cast<const char *>(aout_buf),
                   160 * static_cast<int>(sizeof(short)));
    }

    if (totalErrs > 0)
        qDebug() << "AmbeDecoder: FEC errs:" << totalErrs;

    return pcm;
}

// ──────────────────────────────────────────────
//  Reset decoder state (call at stream boundaries)
// ──────────────────────────────────────────────

void AmbeDecoder::reset()
{
    mbe_initMbeParms(&m_curMp, &m_prevMp, &m_prevMpEnhanced);
}

// ──────────────────────────────────────────────
//  Extract one AMBE+2 codeword from a DMR burst
//
//  DMR burst (264 bits / 33 bytes):
//    bits   0-107  : voice info segment 1 (108 bits)
//    bits 108-155  : SYNC / embedded signaling (48 bits)
//    bits 156-263  : voice info segment 2 (108 bits)
//
//  Three codewords occupy offsets 0, 72, 192 within the burst.
//  Codeword 1 (offset 72) spans the sync gap; any bit position
//  ≥ 108 must be shifted by +48 to land in voice segment 2.
//
//  CRITICAL: mbelib stores bits with HIGH index = MSB.
//  The DMR_A/B/C tables list bits in MSB-first (transmission) order.
//  We must reverse the index when filling ambe_fr so that:
//    table[0] (first transmitted, MSB) → highest ambe_fr index
//    table[N] (last transmitted, LSB)  → lowest ambe_fr index
//
//  C0 special case: ambe_fr[0][0] = overall parity bit,
//    ambe_fr[0][1..23] = Golay(23,12) codeword with [23] = MSB.
// ──────────────────────────────────────────────

void AmbeDecoder::extractAmbeCodeword(const unsigned char *burst,
                                      int codewordIdx,
                                      char ambe_fr[4][24])
{
    std::memset(ambe_fr, 0, sizeof(char) * 4 * 24);

    unsigned int offset;
    switch (codewordIdx) {
    case 0:  offset = 0U;   break;
    case 1:  offset = 72U;  break;
    case 2:  offset = 192U; break;
    default: return;
    }

    // C0 → ambe_fr[0]  (24 bits: parity + 23 Golay)
    // table[0]=Golay MSB→fr[0][23], table[23]=parity→fr[0][0]
    for (int i = 0; i < 24; i++) {
        unsigned int pos = DMR_A_TABLE[i] + offset;
        if (codewordIdx == 1 && pos >= 108U)
            pos += 48U;
        ambe_fr[0][23 - i] = static_cast<char>(readBit(burst, pos));
    }

    // C1 → ambe_fr[1]  (23 bits Golay)
    // table[0]=MSB→fr[1][22], table[22]=LSB→fr[1][0]
    for (int i = 0; i < 23; i++) {
        unsigned int pos = DMR_B_TABLE[i] + offset;
        if (codewordIdx == 1 && pos >= 108U)
            pos += 48U;
        ambe_fr[1][22 - i] = static_cast<char>(readBit(burst, pos));
    }

    // C2 → ambe_fr[2]  (11 bits, unprotected)
    // table[0]=MSB→fr[2][10], table[10]=LSB→fr[2][0]
    for (int i = 0; i < 11; i++) {
        unsigned int pos = DMR_C_TABLE[i] + offset;
        if (codewordIdx == 1 && pos >= 108U)
            pos += 48U;
        ambe_fr[2][10 - i] = static_cast<char>(readBit(burst, pos));
    }

    // C3 → ambe_fr[3]  (14 bits, unprotected)
    // table[11]=MSB→fr[3][13], table[24]=LSB→fr[3][0]
    for (int i = 0; i < 14; i++) {
        unsigned int pos = DMR_C_TABLE[11 + i] + offset;
        if (codewordIdx == 1 && pos >= 108U)
            pos += 48U;
        ambe_fr[3][13 - i] = static_cast<char>(readBit(burst, pos));
    }
}
