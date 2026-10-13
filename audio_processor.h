#pragma once
#include <oboe/Oboe.h>

#include <atomic>
#include <memory>
#include <vector>

struct AudioFeatures {
    float amplitude = 0.f;  // 0..1 (nivel RMS normalizado en dB)
    float bass      = 0.f;  // 20-250 Hz
    float mid       = 0.f;  // 250-4000 Hz
    float treble    = 0.f;  // 4-12 kHz
    bool  live      = false; // true si hay captura de audio activa
};

// Captura de micrófono con Oboe (baja latencia) + FFT de 1024 puntos.
class AudioProcessor : public oboe::AudioStreamDataCallback,
                       public oboe::AudioStreamErrorCallback,
                       public std::enable_shared_from_this<AudioProcessor> {
public:
    static constexpr int kFftSize = 1024;
    static constexpr int kHop     = 512;

    AudioProcessor();
    ~AudioProcessor() override;

    bool start();
    void stop();
    bool isRunning() const { return mRunning.load(); }
    bool needsRestart() const { return mNeedsRestart.load(); }
    AudioFeatures features() const;

    // Hilo de audio de Oboe
    oboe::DataCallbackResult onAudioReady(oboe::AudioStream* stream, void* audioData,
                                          int32_t numFrames) override;
    void onErrorAfterClose(oboe::AudioStream* stream, oboe::Result error) override;

private:
    void analyze();
    void fft();

    std::shared_ptr<oboe::AudioStream> mStream;
    std::atomic<bool> mRunning{false};
    std::atomic<bool> mNeedsRestart{false};
    std::atomic<int>  mSampleRate{48000};

    std::atomic<float> mAmp{0.f}, mBass{0.f}, mMid{0.f}, mTreble{0.f};

    // Buffers preasignados (no se reserva memoria en el hilo de audio)
    std::vector<float> mRing, mWindow, mRe, mIm, mCos, mSin;
    std::vector<int>   mBitRev;
    int mWrite = 0;
    int mSinceAnalysis = 0;
};
