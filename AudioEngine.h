#ifndef AUDIOENGINE_H
#define AUDIOENGINE_H

#include <QObject>
#include <QAudioSource>
#include <QAudioSink>
#include <QAudioFormat>
#include <QAudioDevice>
#include <QIODevice>
#include <QTimer>
#include <QByteArray>
#include <QList>

// Shared audio engine for microphone capture and buffered speaker playback.
// Handles sample rate conversion and audio preprocessing (DC removal, gain).
class AudioEngine : public QObject
{
    Q_OBJECT

public:
    explicit AudioEngine(QObject *parent = nullptr);
    ~AudioEngine() override;

    bool initialize(const QString &inputDeviceName = QString(),
                    const QString &outputDeviceName = QString());
    static QList<QAudioDevice> availableInputDevices();
    static QList<QAudioDevice> availableOutputDevices();

    void startCapture();
    void stopCapture();
    bool isCapturing() const { return m_capturing; }

    void playPCM(const QByteArray &pcm);
    void resetPlayback();
    void setPlaybackVolume(int percent);
    int playbackVolume() const { return m_playbackVolume; }

    QAudioFormat format() const { return m_format; }

signals:
    void pcmCaptured(const QByteArray &data);
    void audioLevelChanged(float level); // 0.0–1.0 RMS
    void logMessage(const QString &msg);

private slots:
    void onMicPollTimer();
    void drainPlaybackBuffer();

private:
    void setupFormat();
    QAudioDevice findInputDevice() const;
    QAudioDevice findOutputDevice() const;
    bool setupOutputSink();
    QByteArray resampleToTarget(const QByteArray &raw);

    // Target format: 8 kHz, mono, 16-bit
    QAudioFormat m_format;
    QString m_inputDeviceName;
    QString m_outputDeviceName;

    int m_captureRate = 8000;
    int m_captureChannels = 1;

    QAudioSource *m_audioSource = nullptr;
    QIODevice *m_micDevice = nullptr;
    QTimer *m_micPollTimer = nullptr;
    bool m_capturing = false;
    bool m_pttPending = false;  // PTT pressed while permission dialog was open
    int m_micDebugCounter = 0;

    QAudioSink *m_audioSink = nullptr;
    QIODevice *m_speakerDevice = nullptr;
    QTimer *m_drainTimer = nullptr;
    QByteArray m_playbackBuffer;
    bool m_initialized = false;
    bool m_bufferPrimed = false;
    int m_playbackVolume = 100;
    static constexpr int JITTER_BUFFER_BYTES = 2880;
};

#endif // AUDIOENGINE_H
