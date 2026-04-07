package cz.dmrradio;

import android.media.AudioAttributes;
import android.media.AudioFormat;
import android.media.AudioManager;
import android.media.AudioTrack;
import android.util.Log;

/**
 * Native AudioTrack wrapper with USAGE_VOICE_COMMUNICATION attributes.
 * Android will NOT mute voice-communication streams when the screen is off
 * or the app moves to the background, unlike default USAGE_MEDIA streams
 * created by Qt's QAudioSink / AAudio backend.
 *
 * All public methods are synchronized – safe to call from any thread (JNI).
 */
public class NativeAudio {
    private static final String TAG = "NativeAudio";
    private static AudioTrack sTrack;

    public static synchronized void init(int sampleRate) {
        release();

        AudioAttributes attrs = new AudioAttributes.Builder()
            .setUsage(AudioAttributes.USAGE_VOICE_COMMUNICATION)
            .setContentType(AudioAttributes.CONTENT_TYPE_SPEECH)
            .build();

        AudioFormat fmt = new AudioFormat.Builder()
            .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
            .setSampleRate(sampleRate)
            .setChannelMask(AudioFormat.CHANNEL_OUT_MONO)
            .build();

        int minBuf = AudioTrack.getMinBufferSize(
            sampleRate, AudioFormat.CHANNEL_OUT_MONO, AudioFormat.ENCODING_PCM_16BIT);
        int bufSize = Math.max(minBuf * 4, 8192);

        sTrack = new AudioTrack(attrs, fmt, bufSize,
            AudioTrack.MODE_STREAM, AudioManager.AUDIO_SESSION_ID_GENERATE);
        sTrack.play();
        Log.d(TAG, "AudioTrack created: sr=" + sampleRate + " buf=" + bufSize);
    }

    public static synchronized void write(byte[] data, int offset, int length) {
        if (sTrack != null) {
            sTrack.write(data, offset, length);
        }
    }

    public static synchronized void setVolume(float vol) {
        if (sTrack != null) {
            sTrack.setVolume(Math.max(0.0f, Math.min(1.0f, vol)));
        }
    }

    public static synchronized void release() {
        if (sTrack != null) {
            try { sTrack.stop(); } catch (IllegalStateException ignored) {}
            sTrack.release();
            sTrack = null;
            Log.d(TAG, "AudioTrack released");
        }
    }
}
