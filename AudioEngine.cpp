#include "AudioEngine.h"
#include <QMediaDevices>
#include <QAudioDevice>
#include <QDebug>
#include <algorithm>
#include <cmath>
#include <cstring>
#ifdef Q_OS_ANDROID
#include <QPermissions>
#include <QCoreApplication>
#include <QJniObject>
#include <QJniEnvironment>
#endif

static constexpr int DRAIN_INTERVAL_MS    = 10;
static constexpr int MIC_POLL_INTERVAL_MS = 20;
static constexpr int TARGET_RATE          = 8000;
static constexpr float MIC_GAIN_MAX_SCALE = 0.75f;

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
#ifdef Q_OS_ANDROID
    if (m_nativeAudioReady) {
        QJniObject::callStaticMethod<void>(
            "cz/dmrradio/NativeAudio", "release", "()V");
        m_nativeAudioReady = false;
    }
#endif
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
#ifdef Q_OS_ANDROID
    // Use Android native AudioTrack with USAGE_VOICE_COMMUNICATION so the
    // system audio policy will NOT mute us when the screen is off / app is
    // in the background.  Qt's QAudioSink (AAudio backend) uses USAGE_MEDIA
    // which Android silences on window-focus-loss.
    QJniObject::callStaticMethod<void>(
        "cz/dmrradio/NativeAudio", "init", "(I)V", TARGET_RATE);
    m_nativeAudioReady = true;
    QJniObject::callStaticMethod<void>(
        "cz/dmrradio/NativeAudio", "setVolume", "(F)V",
        static_cast<float>(std::clamp(m_playbackVolume, 0, 100) / 100.0));
    emit logMessage("AudioEngine: Native AudioTrack (VOICE_COMMUNICATION, 8 kHz mono)");
    return true;
#else
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

    // Start suspended — will resume when audio actually arrives.
    // Prevents Android AudioTrack "mute" spam when idle.
    m_audioSink->suspend();

    emit logMessage(QString("AudioEngine: Output device: %1").arg(outputDevice.description()));
    return true;
#endif
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

#ifdef Q_OS_ANDROID
    // Android requires runtime RECORD_AUDIO permission (API 23+)
    QMicrophonePermission micPerm;
    switch (qApp->checkPermission(micPerm)) {
    case Qt::PermissionStatus::Undetermined:
        m_pttPending = true;  // remember that PTT was pressed
        qApp->requestPermission(micPerm, this, [this](const QPermission &perm) {
            if (perm.status() == Qt::PermissionStatus::Granted) {
                if (m_pttPending) {
                    m_pttPending = false;
                    startCapture();  // PTT was still "logically" held — start now
                }
            } else {
                m_pttPending = false;
                emit logMessage("AudioEngine: Microphone permission denied by user");
            }
        });
        return;
    case Qt::PermissionStatus::Denied:
        emit logMessage("AudioEngine: Microphone permission denied — enable in system settings");
        return;
    case Qt::PermissionStatus::Granted:
        break;
    }
#endif

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
    m_pttPending = false;  // cancel any pending PTT waiting for permission

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
    if (!m_initialized || m_capturing)
        return;

#ifdef Q_OS_ANDROID
    if (!m_nativeAudioReady) {
        if (!setupOutputSink())
            return;
    }
#else
    if (!m_speakerDevice)
        return;

    if (!m_audioSink || m_audioSink->state() == QAudio::StoppedState) {
        if (!setupOutputSink())
            return;
    }

    if (m_audioSink && m_audioSink->state() == QAudio::SuspendedState)
        m_audioSink->resume();
#endif

    m_playbackBuffer.append(pcm);

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
#ifndef Q_OS_ANDROID
    if (m_audioSink && m_audioSink->state() == QAudio::ActiveState)
        m_audioSink->suspend();
#endif
}

void AudioEngine::setPlaybackVolume(int percent)
{
    m_playbackVolume = std::clamp(percent, 0, 100);
#ifdef Q_OS_ANDROID
    if (m_nativeAudioReady) {
        QJniObject::callStaticMethod<void>(
            "cz/dmrradio/NativeAudio", "setVolume", "(F)V",
            static_cast<float>(m_playbackVolume / 100.0));
    }
#else
    if (m_audioSink)
        m_audioSink->setVolume(m_playbackVolume / 100.0);
#endif
}

void AudioEngine::setMicGain(int percent)
{
    m_micGain = std::clamp(percent, 0, 100);
}

QByteArray AudioEngine::applyMicGain(const QByteArray &pcm) const
{
    if (pcm.isEmpty())
        return pcm;

    QByteArray out(pcm);
    auto *samples = reinterpret_cast<int16_t *>(out.data());
    const int count = out.size() / static_cast<int>(sizeof(int16_t));
    const float gain = static_cast<float>(m_micGain) / 100.0f * MIC_GAIN_MAX_SCALE;

    for (int i = 0; i < count; ++i) {
        const float scaled = static_cast<float>(samples[i]) * gain;
        samples[i] = static_cast<int16_t>(std::clamp(scaled, -32768.0f, 32767.0f));
    }

    return out;
}

void AudioEngine::drainPlaybackBuffer()
{
    if (m_playbackBuffer.isEmpty())
        return;

#ifdef Q_OS_ANDROID
    if (!m_nativeAudioReady)
        return;

    if (!m_bufferPrimed) {
        if (m_playbackBuffer.size() < JITTER_BUFFER_BYTES)
            return;
        m_bufferPrimed = true;
    }

    QJniEnvironment env;
    const int len = m_playbackBuffer.size();
    jbyteArray jdata = env->NewByteArray(len);
    env->SetByteArrayRegion(jdata, 0, len,
        reinterpret_cast<const jbyte *>(m_playbackBuffer.constData()));
    QJniObject::callStaticMethod<void>(
        "cz/dmrradio/NativeAudio", "write", "([BII)V",
        jdata, jint(0), jint(len));
    env->DeleteLocalRef(jdata);
    m_playbackBuffer.clear();
#else
    if (!m_speakerDevice)
        return;

    if (m_audioSink && m_audioSink->state() == QAudio::StoppedState) {
        if (!setupOutputSink())
            return;
    }

    if (!m_bufferPrimed) {
        if (m_playbackBuffer.size() < JITTER_BUFFER_BYTES)
            return;
        m_bufferPrimed = true;
    }

    qint64 written = m_speakerDevice->write(m_playbackBuffer);
    if (written > 0)
        m_playbackBuffer.remove(0, static_cast<int>(written));

    // Suspend output when buffer is fully drained to avoid idle AudioTrack
    if (m_playbackBuffer.isEmpty() && m_audioSink
        && m_audioSink->state() == QAudio::ActiveState) {
        m_audioSink->suspend();
        m_bufferPrimed = false;
    }
#endif
}

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

    QByteArray pcm = resampleToTarget(raw);
    if (pcm.isEmpty())
        return;

    pcm = applyMicGain(pcm);

    m_micDebugCounter++;
    if (m_micDebugCounter % 25 == 1) {
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
