#ifndef AMBEDECODER_H
#define AMBEDECODER_H

#include <QByteArray>

#include "mbelib/mbelib.h"

// Decodes AMBE+2 voice data from DMR bursts to PCM samples using mbelib.
//
// Each DMR voice burst carries 33 bytes (264 bits) containing 3 interleaved
// AMBE+2 codewords.  Each codeword decodes to 160 PCM samples (20 ms at 8 kHz).
// One burst therefore produces 480 samples = 960 bytes of 16-bit PCM = 60 ms.
//
// The extraction follows ETSI TS 102 361-1 Annex B: the 264-bit burst is
// laid out as 108 voice bits + 48 sync/EMB bits + 108 voice bits.  The three
// AMBE codewords are interleaved across the 216 voice bits using the DMR
// A/B/C permutation tables.
class AmbeDecoder
{
public:
    AmbeDecoder();

    // Decode one DMR voice burst (33 bytes) → PCM (960 bytes = 60 ms).
    QByteArray decode(const QByteArray &dmrBurst);

    // Reset decoder state (call between voice streams).
    void reset();

private:
    // Extract one AMBE+2 codeword from a 33-byte DMR burst into
    // the ambe_fr[4][24] layout expected by mbelib.
    void extractAmbeCodeword(const unsigned char *burst, int codewordIdx,
                             char ambe_fr[4][24]);

    // Decoder state — must persist across frames within a voice stream
    mbe_parms m_curMp;
    mbe_parms m_prevMp;
    mbe_parms m_prevMpEnhanced;
};

#endif // AMBEDECODER_H
