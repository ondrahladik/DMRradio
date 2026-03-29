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

    bool initialize(const QString &inputDeviceName = QString());
    static QList<QAudioDevice> availableInputDevices();

    void startCapture();
    void stopCapture();
    bool isCapturing() const { return m_capturing; }

    void playPCM(const QByteArray &pcm);
    void resetPlayback();

    QAudioFormat format() const { return m_format; }

signals:
    void pcmCaptured(const QByteArray &data);
    void logMessage(const QString &msg);

private slots:
    void onMicPollTimer();
    void drainPlaybackBuffer();

private:
    void setupFormat();
    QAudioDevice findInputDevice() const;
    QByteArray resampleToTarget(const QByteArray &raw);

    // Target format: 8 kHz, mono, 16-bit
    QAudioFormat m_format;
    QString m_inputDeviceName;

    // Actual capture format (may differ from target on Windows)
    int m_captureRate = 8000;
    int m_captureChannels = 1;

    // Mic capture
    QAudioSource *m_audioSource = nullptr;
    QIODevice *m_micDevice = nullptr;
    QTimer *m_micPollTimer = nullptr;
    bool m_capturing = false;
    int m_micDebugCounter = 0;

    // Speaker playback
    QAudioSink *m_audioSink = nullptr;
    QIODevice *m_speakerDevice = nullptr;
    QTimer *m_drainTimer = nullptr;
    QByteArray m_playbackBuffer;
    bool m_initialized = false;
    bool m_bufferPrimed = false;
    static constexpr int JITTER_BUFFER_BYTES = 2880;
};

#endif // AUDIOENGINE_H
