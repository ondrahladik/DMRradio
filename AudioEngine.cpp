#include "AudioEngine.h"
#include <QMediaDevices>
#include <QAudioDevice>
#include <QDebug>
#include <algorithm>
#include <cmath>
#include <cstring>

static constexpr int DRAIN_INTERVAL_MS    = 10;
static constexpr int MIC_POLL_INTERVAL_MS = 20;
static constexpr int TARGET_RATE          = 8000;

AudioEngine::AudioEngine(QObject *parent)
    : QObject(parent)
{
    setupFormat();
}

AudioEngine::~AudioEngine()
{
    stopCapture();
    if (m_drainTimer)
        m_drainTimer->stop();
}

void AudioEngine::setupFormat()
{
    m_format.setSampleRate(TARGET_RATE);
    m_format.setChannelCount(1);
    m_format.setSampleFormat(QAudioFormat::Int16);
}

QList<QAudioDevice> AudioEngine::availableInputDevices()
{
    return QMediaDevices::audioInputs();
}

QList<QAudioDevice> AudioEngine::availableOutputDevices()
{
    return QMediaDevices::audioOutputs();
}

QAudioDevice AudioEngine::findInputDevice() const
{
    if (!m_inputDeviceName.isEmpty()) {
        for (const QAudioDevice &dev : QMediaDevices::audioInputs()) {
            if (dev.description() == m_inputDeviceName)
                return dev;
        }
        qWarning() << "AudioEngine: Requested input device not found:" << m_inputDeviceName;
    }
    return QMediaDevices::defaultAudioInput();
}

QAudioDevice AudioEngine::findOutputDevice() const
{
    if (!m_outputDeviceName.isEmpty()) {
        for (const QAudioDevice &dev : QMediaDevices::audioOutputs()) {
            if (dev.description() == m_outputDeviceName)
                return dev;
        }
        qWarning() << "AudioEngine: Requested output device not found:" << m_outputDeviceName;
    }
    return QMediaDevices::defaultAudioOutput();
}

bool AudioEngine::setupOutputSink()
{
    QAudioDevice outputDevice = findOutputDevice();
    if (outputDevice.isNull()) {
        emit logMessage("AudioEngine: No audio output device found");
        return false;
    }

    QAudioSink *newSink = new QAudioSink(outputDevice, m_format, this);
    QIODevice *newSpeaker = newSink->start();
    if (!newSpeaker) {
        emit logMessage("AudioEngine: Failed to open speaker device");
        delete newSink;
        return false;
    }

    if (m_audioSink) {
        m_audioSink->stop();
        delete m_audioSink;
    }

    m_audioSink = newSink;
    m_speakerDevice = newSpeaker;
    m_audioSink->setVolume(std::clamp(m_playbackVolume, 0, 100) / 100.0);
    emit logMessage(QString("AudioEngine: Output device: %1").arg(outputDevice.description()));
    return true;
}

bool AudioEngine::initialize(const QString &inputDeviceName, const QString &outputDeviceName)
{
    if (m_initialized) {
        bool outputChanged = (outputDeviceName != m_outputDeviceName);
        m_inputDeviceName = inputDeviceName;
        m_outputDeviceName = outputDeviceName;

        if (outputChanged) {
            resetPlayback();
            if (!setupOutputSink())
                return false;
        }

        return true;
    }

    m_inputDeviceName = inputDeviceName;
    m_outputDeviceName = outputDeviceName;

    QAudioDevice inputDevice = findInputDevice();
    if (inputDevice.isNull())
        emit logMessage("AudioEngine: No audio input device found (mic disabled)");
    else
        emit logMessage(QString("AudioEngine: Input device: %1").arg(inputDevice.description()));

    if (!setupOutputSink())
        return false;

    m_drainTimer = new QTimer(this);
    m_drainTimer->setInterval(DRAIN_INTERVAL_MS);
    connect(m_drainTimer, &QTimer::timeout, this, &AudioEngine::drainPlaybackBuffer);
    m_drainTimer->start();

    m_micPollTimer = new QTimer(this);
    m_micPollTimer->setInterval(MIC_POLL_INTERVAL_MS);
    connect(m_micPollTimer, &QTimer::timeout, this, &AudioEngine::onMicPollTimer);

    m_initialized = true;
    emit logMessage("AudioEngine: Initialized (8 kHz, mono, 16-bit PCM)");
    return true;
}

void AudioEngine::startCapture()
{
    if (!m_initialized || m_capturing)
        return;

    if (m_audioSource) {
        m_audioSource->stop();
        delete m_audioSource;
        m_audioSource = nullptr;
        m_micDevice = nullptr;
    }

    QAudioDevice inputDevice = findInputDevice();
    if (inputDevice.isNull()) {
        emit logMessage("AudioEngine: No input device — cannot capture");
        return;
    }

    // Prefer 8 kHz; fall back to device's native rate with resampling
    QAudioFormat captureFormat = m_format;
    if (!inputDevice.isFormatSupported(captureFormat)) {
        QAudioFormat pref = inputDevice.preferredFormat();
        captureFormat.setSampleRate(pref.sampleRate());
        captureFormat.setChannelCount(pref.channelCount());
        captureFormat.setSampleFormat(QAudioFormat::Int16);

        emit logMessage(QString("AudioEngine: 8kHz not supported, capturing at %1 Hz %2ch (will resample)")
                            .arg(captureFormat.sampleRate())
                            .arg(captureFormat.channelCount()));
    }

    m_captureRate = captureFormat.sampleRate();
    m_captureChannels = captureFormat.channelCount();

    m_audioSource = new QAudioSource(inputDevice, captureFormat, this);
    int bytesPerFrame = static_cast<int>(sizeof(int16_t)) * m_captureChannels;
    m_audioSource->setBufferSize(m_captureRate / 5 * bytesPerFrame);

    m_micDevice = m_audioSource->start();
    if (!m_micDevice) {
        emit logMessage("AudioEngine: Failed to start QAudioSource!");
        delete m_audioSource;
        m_audioSource = nullptr;
        return;
    }

    m_capturing = true;
    m_micDebugCounter = 0;
    m_micPollTimer->start();

    emit logMessage(QString("AudioEngine: Mic started (%1, %2 Hz %3ch)")
                        .arg(inputDevice.description())
                        .arg(m_captureRate)
                        .arg(m_captureChannels));
}

void AudioEngine::stopCapture()
{
    if (!m_capturing)
        return;

    m_micPollTimer->stop();

    if (m_audioSource) {
        m_audioSource->stop();
        delete m_audioSource;
        m_audioSource = nullptr;
        m_micDevice = nullptr;
    }

    m_capturing = false;
    emit logMessage("AudioEngine: Mic stopped");
}

void AudioEngine::playPCM(const QByteArray &pcm)
{
    if (!m_initialized || !m_speakerDevice)
        return;
    m_playbackBuffer.append(pcm);

    // Emit RMS level for the VU bargraph (RX — incoming audio)
    const auto *s = reinterpret_cast<const int16_t *>(pcm.constData());
    const int count = pcm.size() / 2;
    if (count > 0) {
        float sum = 0.0f;
        for (int i = 0; i < count; i++) {
            float v = s[i] / 32767.0f;
            sum += v * v;
        }
        emit audioLevelChanged(std::sqrt(sum / count));
    }
}

void AudioEngine::resetPlayback()
{
    m_playbackBuffer.clear();
    m_bufferPrimed = false;
}

void AudioEngine::setPlaybackVolume(int percent)
{
    m_playbackVolume = std::clamp(percent, 0, 100);
    if (m_audioSink)
        m_audioSink->setVolume(m_playbackVolume / 100.0);
}

void AudioEngine::drainPlaybackBuffer()
{
    if (m_playbackBuffer.isEmpty() || !m_speakerDevice)
        return;

    if (!m_bufferPrimed) {
        if (m_playbackBuffer.size() < JITTER_BUFFER_BYTES)
            return;
        m_bufferPrimed = true;
    }

    qint64 written = m_speakerDevice->write(m_playbackBuffer);
    if (written > 0)
        m_playbackBuffer.remove(0, static_cast<int>(written));
}

// Resample raw captured audio to 8 kHz mono 16-bit with anti-aliasing.
QByteArray AudioEngine::resampleToTarget(const QByteArray &raw)
{
    if (m_captureRate == TARGET_RATE && m_captureChannels == 1)
        return raw;

    const auto *src = reinterpret_cast<const int16_t *>(raw.constData());
    int totalSamples = raw.size() / static_cast<int>(sizeof(int16_t));
    int srcFrames = totalSamples / m_captureChannels;
    if (srcFrames < 2)
        return QByteArray();

    int decimation = m_captureRate / TARGET_RATE; // e.g. 6 for 48k→8k
    if (decimation < 1) decimation = 1;

    int dstFrames = static_cast<int>(
        static_cast<double>(srcFrames) * TARGET_RATE / m_captureRate);
    if (dstFrames < 1)
        return QByteArray();

    QByteArray result(dstFrames * static_cast<int>(sizeof(int16_t)), '\0');
    auto *dst = reinterpret_cast<int16_t *>(result.data());

    double ratio = static_cast<double>(m_captureRate) / TARGET_RATE;

    // Anti-aliasing filter radius (half the decimation factor)
    int filterRadius = decimation / 2;
    if (filterRadius < 1) filterRadius = 1;
    int filterLen = 2 * filterRadius + 1;

    for (int i = 0; i < dstFrames; i++) {
        double srcCenter = i * ratio;
        int center = static_cast<int>(srcCenter + 0.5);

        // Read mono value (mix down channels if stereo)
        auto readMono = [&](int frame) -> double {
            if (frame < 0) frame = 0;
            if (frame >= srcFrames) frame = srcFrames - 1;
            if (m_captureChannels == 1)
                return src[frame];
            double sum = 0.0;
            for (int ch = 0; ch < m_captureChannels; ch++)
                sum += src[frame * m_captureChannels + ch];
            return sum / m_captureChannels;
        };

        // Box-filter average over filterLen samples (anti-aliasing)
        double acc = 0.0;
        for (int k = -filterRadius; k <= filterRadius; k++)
            acc += readMono(center + k);
        acc /= filterLen;

        dst[i] = static_cast<int16_t>(qBound(-32768.0, acc, 32767.0));
    }

    return result;
}

void AudioEngine::onMicPollTimer()
{
    if (!m_micDevice || !m_capturing)
        return;

    QByteArray raw = m_micDevice->readAll();
    if (raw.isEmpty())
        return;

    // Resample to 8 kHz mono if needed
    QByteArray pcm = resampleToTarget(raw);
    if (pcm.isEmpty())
        return;

    // No external preprocessing — the IMBE encoder has its own DC removal
    // and gain handling. Passing raw PCM preserves maximum signal quality.

    // Debug log every ~500ms
    m_micDebugCounter++;
    if (m_micDebugCounter % 25 == 1) {
        // Log PCM level for diagnostics
        const auto *s = reinterpret_cast<const int16_t *>(pcm.constData());
        int count = pcm.size() / 2;
        int16_t minVal = 0, maxVal = 0;
        for (int i = 0; i < count; i++) {
            if (s[i] < minVal) minVal = s[i];
            if (s[i] > maxVal) maxVal = s[i];
        }
        qDebug() << "AudioEngine:" << raw.size() << "B @" << m_captureRate
                 << "Hz →" << pcm.size() << "B @8kHz  peak:" << minVal << "/" << maxVal;
    }

    emit pcmCaptured(pcm);

    // Emit RMS level for the VU bargraph (TX — microphone input)
    {
        const auto *s = reinterpret_cast<const int16_t *>(pcm.constData());
        const int count = pcm.size() / 2;
        if (count > 0) {
            float sum = 0.0f;
            for (int i = 0; i < count; i++) {
                float v = s[i] / 32767.0f;
                sum += v * v;
            }
            emit audioLevelChanged(std::sqrt(sum / count));
        }
    }
}
