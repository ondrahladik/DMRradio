#ifndef AUDIOLEVELLBAR_H
#define AUDIOLEVELLBAR_H

#include <QWidget>
#include <QTimer>

class AudioLevelBar : public QWidget
{
    Q_OBJECT

public:
    explicit AudioLevelBar(QWidget *parent = nullptr);
    QSize sizeHint() const override;

public slots:
    void setLevel(float level); // 0.0–1.0

protected:
    void paintEvent(QPaintEvent *event) override;

private slots:
    void onDecayTimer();

private:
    float m_level = 0.0f;
    float m_peakLevel = 0.0f;
    int   m_peakHoldFrames = 0;
    QTimer *m_decayTimer = nullptr;

    static constexpr int   SEGMENTS          = 32;
    static constexpr float DECAY_RATE        = 0.05f;
    static constexpr int   PEAK_HOLD_FRAMES  = 28;
    static constexpr int   TIMER_INTERVAL_MS = 50;
};

#endif // AUDIOLEVELLBAR_H
