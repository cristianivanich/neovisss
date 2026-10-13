#include "audio_processor.h"

#include <algorithm>
#include <cmath>

#include "log.h"

namespace {
constexpr float kPi = 3.14159265358979323846f;

inline float normDb(float x, float floorDb, float rangeDb) {
    float db = 20.f * std::log10(x + 1e-7f);
    return std::clamp((db - floorDb) / rangeDb, 0.f, 1.f);
}
}  // namespace

AudioProcessor::AudioProcessor() {
    const int N = kFftSize;
    mRing.assign(N, 0.f);
    mWindow.resize(N);
    mRe.assign(N, 0.f);
    mIm.assign(N, 0.f);
    mCos.resize(N / 2);
    mSin.resize(N / 2);
    mBitRev.resize(N);

    for (int i = 0; i < N; ++i) {
        mWindow[i] = 0.5f * (1.f - std::cos(2.f * kPi * i / (N - 1)));  // Hann
    }
    int bits = 0;
    while ((1 << bits) < N) ++bits;
    for (int i = 0; i < N; ++i) {
        int r = 0;
        for (int b = 0; b < bits; ++b)
            if (i & (1 << b)) r |= 1 << (bits - 1 - b);
        mBitRev[i] = r;
    }
    for (int k = 0; k < N / 2; ++k) {
        mCos[k] = std::cos(2.f * kPi * k / N);
        mSin[k] = -std::sin(2.f * kPi * k / N);
    }
}

AudioProcessor::~AudioProcessor() { stop(); }

bool AudioProcessor::start() {
    if (mStream) return mRunning.load();

    const oboe::InputPreset presets[] = {oboe::InputPreset::Unprocessed,
                                         oboe::InputPreset::VoiceRecognition,
                                         oboe::InputPreset::Generic};
    oboe::Result result = oboe::Result::ErrorInternal;

    for (auto preset : presets) {
        oboe::AudioStreamBuilder builder;
        builder.setDirection(oboe::Direction::Input)
            ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
            ->setSharingMode(oboe::SharingMode::Shared)
            ->setFormat(oboe::AudioFormat::Float)
            ->setChannelCount(oboe::ChannelCount::Mono)
            ->setInputPreset(preset)
            ->setChannelConversionAllowed(true)
            ->setFormatConversionAllowed(true)
            ->setDataCallback(shared_from_this())
            ->setErrorCallback(shared_from_this());

        result = builder.openStream(mStream);
        if (result == oboe::Result::OK) break;
        LOGW("Oboe: openStream fallo con preset %d: %s", static_cast<int>(preset),
             oboe::convertToText(result));
        mStream.reset();
    }
    if (result != oboe::Result::OK || !mStream) {
        LOGE("Oboe: no se pudo abrir el stream de entrada");
        mStream.reset();
        return false;
    }

    mSampleRate.store(mStream->getSampleRate());
    mWrite = 0;
    mSinceAnalysis = 0;
    std::fill(mRing.begin(), mRing.end(), 0.f);

    result = mStream->requestStart();
    if (result != oboe::Result::OK) {
        LOGE("Oboe: requestStart fallo: %s", oboe::convertToText(result));
        mStream->close();
        mStream.reset();
        return false;
    }
    mNeedsRestart.store(false);
    mRunning.store(true);
    LOGI("Oboe: captura iniciada %d Hz, rafaga %d frames, API=%s", mStream->getSampleRate(),
         mStream->getFramesPerBurst(), oboe::convertToText(mStream->getAudioApi()));
    return true;
}

void AudioProcessor::stop() {
    mRunning.store(false);
    if (mStream) {
        mStream->stop();
        mStream->close();
        mStream.reset();  // rompe el ciclo shared_ptr stream <-> callbacks
    }
    mAmp = mBass = mMid = mTreble = 0.f;
}

AudioFeatures AudioProcessor::features() const {
    AudioFeatures f;
    f.amplitude = mAmp.load();
    f.bass      = mBass.load();
    f.mid       = mMid.load();
    f.treble    = mTreble.load();
    f.live      = mRunning.load();
    return f;
}

oboe::DataCallbackResult AudioProcessor::onAudioReady(oboe::AudioStream*, void* audioData,
                                                      int32_t numFrames) {
    const float* in = static_cast<const float*>(audioData);
    for (int32_t i = 0; i < numFrames; ++i) {
        mRing[mWrite] = in[i];
        mWrite = (mWrite + 1) % kFftSize;
        if (++mSinceAnalysis >= kHop) {
            mSinceAnalysis = 0;
            analyze();
        }
    }
    return oboe::DataCallbackResult::Continue;
}

void AudioProcessor::onErrorAfterClose(oboe::AudioStream*, oboe::Result error) {
    LOGW("Oboe: stream cerrado por error: %s", oboe::convertToText(error));
    mRunning.store(false);
    mNeedsRestart.store(true);  // el bucle principal reinicia la captura
}

void AudioProcessor::fft() {
    const int N = kFftSize;
    for (int size = 2; size <= N; size <<= 1) {
        const int half = size / 2;
        const int step = N / size;
        for (int i = 0; i < N; i += size) {
            for (int j = 0; j < half; ++j) {
                const int k = j * step;
                const float wr = mCos[k], wi = mSin[k];
                const int a = i + j, b = a + half;
                const float tr = mRe[b] * wr - mIm[b] * wi;
                const float ti = mRe[b] * wi + mIm[b] * wr;
                mRe[b] = mRe[a] - tr;
                mIm[b] = mIm[a] - ti;
                mRe[a] += tr;
                mIm[a] += ti;
            }
        }
    }
}

void AudioProcessor::analyze() {
    const int N = kFftSize;
    float sumSq = 0.f;
    for (int i = 0; i < N; ++i) {
        const float s = mRing[(mWrite + i) % N];  // de la muestra más antigua a la más nueva
        sumSq += s * s;
        mRe[mBitRev[i]] = s * mWindow[i];
        mIm[i] = 0.f;
    }
    // mIm se indexa en orden natural pero vale 0 en todas partes: no importa el orden.
    const float rms = std::sqrt(sumSq / N);
    fft();

    const float binHz = static_cast<float>(mSampleRate.load()) / N;
    auto band = [&](float lo, float hi) {
        const int a = std::max(1, static_cast<int>(std::ceil(lo / binHz)));
        const int b = std::min(N / 2 - 1, static_cast<int>(std::floor(hi / binHz)));
        if (b < a) return 0.f;
        float sum = 0.f;
        for (int k = a; k <= b; ++k) {
            const float m = std::sqrt(mRe[k] * mRe[k] + mIm[k] * mIm[k]) / (N * 0.25f);
            sum += m * m;
        }
        return normDb(std::sqrt(sum / (b - a + 1)), -80.f, 50.f);
    };

    mAmp.store(normDb(rms, -60.f, 50.f));
    mBass.store(band(20.f, 250.f));
    mMid.store(band(250.f, 4000.f));
    mTreble.store(band(4000.f, 12000.f));
}
