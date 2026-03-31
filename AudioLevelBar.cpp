#include "AudioLevelBar.h"

#include <QPainter>
#include <QPaintEvent>
#include <algorithm>

AudioLevelBar::AudioLevelBar(QWidget *parent) : QWidget(parent)
{
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setMinimumHeight(18);
    setMaximumHeight(18);

    m_decayTimer = new QTimer(this);
    m_decayTimer->setInterval(TIMER_INTERVAL_MS);
    connect(m_decayTimer, &QTimer::timeout, this, &AudioLevelBar::onDecayTimer);
    m_decayTimer->start();
}

QSize AudioLevelBar::sizeHint() const
{
    return QSize(200, 18);
}

void AudioLevelBar::setLevel(float level)
{
    level = std::clamp(level, 0.0f, 1.0f);

    // dB scaling: maps [-50 dB … 0 dB] → [0.0 … 1.0]
    if (level > 0.0f) {
        constexpr float kMinDb = -50.0f;
        float db = 20.0f * std::log10(level);
        level = std::clamp((db - kMinDb) / (-kMinDb), 0.0f, 1.0f);
    }

    if (level > m_level)
        m_level = level;

    if (level >= m_peakLevel) {
        m_peakLevel = level;
        m_peakHoldFrames = PEAK_HOLD_FRAMES;
    }
    update();
}

void AudioLevelBar::onDecayTimer()
{
    bool changed = false;

    if (m_level > 0.0f) {
        m_level = std::max(0.0f, m_level - DECAY_RATE);
        changed = true;
    }

    if (m_peakHoldFrames > 0) {
        --m_peakHoldFrames;
        if (m_peakHoldFrames == 0)
            changed = true;
    } else if (m_peakLevel > 0.0f) {
        m_peakLevel = std::max(0.0f, m_peakLevel - DECAY_RATE);
        changed = true;
    }

    if (changed)
        update();
}

void AudioLevelBar::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, false);

    const int w   = width();
    const int h   = height();
    const int gap = 2;
    const int vPad = 3;

    p.setPen(Qt::NoPen);
    p.fillRect(rect(), QColor(0x0d, 0x0d, 0x0d));

    const float totalGap = float(SEGMENTS - 1) * gap;
    const float segW     = (w - totalGap) / SEGMENTS;

    const int activeSeg = int(m_level * SEGMENTS);
    const int peakSeg   = int(m_peakLevel * SEGMENTS);

    for (int i = 0; i < SEGMENTS; i++) {
        const int x        = int(i * (segW + gap));
        const int segWidth = std::max(1, int(segW));
        const QRect segRect(x, vPad, segWidth, h - 2 * vPad);

        const float pos = float(i) / SEGMENTS;

        QColor color;
        if (i < activeSeg) {
            if (pos < 0.60f)
                color = QColor(0x22, 0xaa, 0x22);
            else if (pos < 0.80f)
                color = QColor(0xbb, 0xbb, 0x00);
            else
                color = QColor(0xcc, 0x22, 0x22);
        } else if (i == peakSeg && m_peakLevel > 0.02f) {
            if (pos < 0.60f)
                color = QColor(0x66, 0xff, 0x66);
            else if (pos < 0.80f)
                color = QColor(0xff, 0xff, 0x44);
            else
                color = QColor(0xff, 0x66, 0x66);
        } else {
            color = QColor(0x25, 0x25, 0x25);
        }

        p.fillRect(segRect, color);
    }
}
