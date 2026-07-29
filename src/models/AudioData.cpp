#include "AudioData.h"
#include <QFile>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <immintrin.h>
#include <thread>
#include <atomic>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
}

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

// ── AVX float32 split-radix FFT ─────────────────────────────────
// Operates on separate real[] and imag[] arrays, all float32.
// N must be a power of 2.

static void fft_avx(float* re, float* im, int N)
{
    // Bit-reversal permutation
    for (int i = 1, j = 0; i < N; ++i) {
        int bit = N >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j) {
            std::swap(re[i], re[j]);
            std::swap(im[i], im[j]);
        }
    }

    // Butterfly passes
    // Max half = N/2; N is at most fftSize (2048), so half <= 1024
    alignas(32) float tw_re[1024];
    alignas(32) float tw_im[1024];

    for (int len = 2; len <= N; len <<= 1) {
        int half = len >> 1;
        float angle = -2.0f * M_PI / len;

        // Precompute twiddle factors for this stage
        for (int j = 0; j < half; ++j) {
            float a = angle * j;
            tw_re[j] = std::cos(a);
            tw_im[j] = std::sin(a);
        }

        for (int i = 0; i < N; i += len) {
            float* re0 = re + i;
            float* im0 = im + i;
            float* re1 = re + i + half;
            float* im1 = im + i + half;

            int j = 0;
            // AVX path: 8 butterflies at a time
            for (; j + 7 < half; j += 8) {
                __m256 wr = _mm256_load_ps(tw_re + j);
                __m256 wi = _mm256_load_ps(tw_im + j);

                __m256 br = _mm256_loadu_ps(re1 + j);
                __m256 bi = _mm256_loadu_ps(im1 + j);

                // Complex multiply: (br + bi*i) * (wr + wi*i)
                // vr = br*wr - bi*wi
                // vi = br*wi + bi*wr
                __m256 vr = _mm256_sub_ps(_mm256_mul_ps(br, wr), _mm256_mul_ps(bi, wi));
                __m256 vi = _mm256_add_ps(_mm256_mul_ps(br, wi), _mm256_mul_ps(bi, wr));

                __m256 ar = _mm256_loadu_ps(re0 + j);
                __m256 ai = _mm256_loadu_ps(im0 + j);

                _mm256_storeu_ps(re0 + j, _mm256_add_ps(ar, vr));
                _mm256_storeu_ps(im0 + j, _mm256_add_ps(ai, vi));
                _mm256_storeu_ps(re1 + j, _mm256_sub_ps(ar, vr));
                _mm256_storeu_ps(im1 + j, _mm256_sub_ps(ai, vi));
            }
            // Scalar tail
            for (; j < half; ++j) {
                float wr = tw_re[j];
                float wi = tw_im[j];
                float vr = re1[j] * wr - im1[j] * wi;
                float vi = re1[j] * wi + im1[j] * wr;
                re0[j] = re0[j] + vr;  // use temps to avoid aliasing
                im0[j] = im0[j] + vi;
                float tr = re0[j] - 2.0f * vr; // a - v = (a+v) - 2v
                float ti = im0[j] - 2.0f * vi;
                re1[j] = tr;
                im1[j] = ti;
            }
        }
    }
}

// ── AVX magnitude: sqrt(re^2 + im^2) / N ────────────────────────

static void magnitude_avx(const float* re, const float* im, float* mag, int N, float invN)
{
    __m256 vInvN = _mm256_set1_ps(invN);
    int i = 0;
    for (; i + 7 < N; i += 8) {
        __m256 vr = _mm256_loadu_ps(re + i);
        __m256 vi = _mm256_loadu_ps(im + i);
        __m256 m2 = _mm256_add_ps(_mm256_mul_ps(vr, vr), _mm256_mul_ps(vi, vi));
        __m256 m = _mm256_sqrt_ps(m2);
        _mm256_storeu_ps(mag + i, _mm256_mul_ps(m, vInvN));
    }
    for (; i < N; ++i) {
        mag[i] = std::sqrt(re[i] * re[i] + im[i] * im[i]) * invN;
    }
}

// ── AVX peak min/max ─────────────────────────────────────────────

static void peaks_avx(const float* samples, int count, float& outMin, float& outMax)
{
    __m256 vmin = _mm256_set1_ps(0.0f);
    __m256 vmax = _mm256_set1_ps(0.0f);
    int i = 0;
    for (; i + 7 < count; i += 8) {
        __m256 v = _mm256_loadu_ps(samples + i);
        vmin = _mm256_min_ps(vmin, v);
        vmax = _mm256_max_ps(vmax, v);
    }
    // Horizontal reduce
    alignas(32) float mins[8], maxs[8];
    _mm256_store_ps(mins, vmin);
    _mm256_store_ps(maxs, vmax);
    float mn = 0, mx = 0;
    for (int k = 0; k < 8; ++k) {
        if (mins[k] < mn) mn = mins[k];
        if (maxs[k] > mx) mx = maxs[k];
    }
    // Scalar tail
    for (; i < count; ++i) {
        if (samples[i] < mn) mn = samples[i];
        if (samples[i] > mx) mx = samples[i];
    }
    outMin = mn;
    outMax = mx;
}

// ── AVX Hann window application ──────────────────────────────────

static void apply_hann_avx(const float* src, const float* window, float* dst_re, float* dst_im,
                           int N, float gain)
{
    __m256 vgain = _mm256_set1_ps(gain);
    __m256 vzero = _mm256_setzero_ps();
    int i = 0;
    for (; i + 7 < N; i += 8) {
        __m256 s = _mm256_loadu_ps(src + i);
        __m256 w = _mm256_load_ps(window + i);  // window is aligned
        _mm256_storeu_ps(dst_re + i, _mm256_mul_ps(_mm256_mul_ps(s, w), vgain));
        _mm256_storeu_ps(dst_im + i, vzero);
    }
    for (; i < N; ++i) {
        dst_re[i] = src[i] * window[i] * gain;
        dst_im[i] = 0.0f;
    }
}

// ── Load entry point ─────────────────────────────────────────────

AudioData* AudioData::loadFromFile(const QString& path, std::function<void(double)> progress)
{
    if (!QFile::exists(path))
        return nullptr;

    auto* data = new AudioData;
    data->filePath = path;

    // ── Decode with libav (direct FFmpeg) ────────────────────────
    std::string pathUtf8 = path.toUtf8().constData();

    AVFormatContext* fmtCtx = nullptr;
    if (avformat_open_input(&fmtCtx, pathUtf8.c_str(), nullptr, nullptr) < 0) {
        delete data;
        return nullptr;
    }
    if (avformat_find_stream_info(fmtCtx, nullptr) < 0) {
        avformat_close_input(&fmtCtx);
        delete data;
        return nullptr;
    }

    // Find best audio stream
    int streamIdx = av_find_best_stream(fmtCtx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (streamIdx < 0) {
        avformat_close_input(&fmtCtx);
        delete data;
        return nullptr;
    }

    auto* codecPar = fmtCtx->streams[streamIdx]->codecpar;
    const AVCodec* codec = avcodec_find_decoder(codecPar->codec_id);
    if (!codec) {
        avformat_close_input(&fmtCtx);
        delete data;
        return nullptr;
    }

    AVCodecContext* codecCtx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(codecCtx, codecPar);
    if (avcodec_open2(codecCtx, codec, nullptr) < 0) {
        avcodec_free_context(&codecCtx);
        avformat_close_input(&fmtCtx);
        delete data;
        return nullptr;
    }

    // Setup resampler → mono float 44100Hz
    const int outSampleRate = 44100;
    SwrContext* swr = nullptr;
    AVChannelLayout outChLayout = AV_CHANNEL_LAYOUT_MONO;
    swr_alloc_set_opts2(&swr,
        &outChLayout, AV_SAMPLE_FMT_FLT, outSampleRate,
        &codecCtx->ch_layout, codecCtx->sample_fmt, codecCtx->sample_rate,
        0, nullptr);
    if (!swr || swr_init(swr) < 0) {
        if (swr) swr_free(&swr);
        avcodec_free_context(&codecCtx);
        avformat_close_input(&fmtCtx);
        delete data;
        return nullptr;
    }

    std::vector<float> allSamples;
    int sampleRate = outSampleRate;

    // Estimate total samples for pre-allocation
    if (fmtCtx->duration > 0) {
        int64_t estSamples = static_cast<int64_t>(
            static_cast<double>(fmtCtx->duration) / AV_TIME_BASE * outSampleRate);
        allSamples.reserve(estSamples);
    }

    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    int64_t totalDurationTs = fmtCtx->duration > 0 ? fmtCtx->duration : 1;

    while (av_read_frame(fmtCtx, pkt) >= 0) {
        if (pkt->stream_index == streamIdx) {
            if (avcodec_send_packet(codecCtx, pkt) >= 0) {
                while (avcodec_receive_frame(codecCtx, frame) >= 0) {
                    // Resample frame to mono float
                    int outSamples = swr_get_out_samples(swr, frame->nb_samples);
                    size_t prevSize = allSamples.size();
                    allSamples.resize(prevSize + outSamples);
                    float* outBuf = allSamples.data() + prevSize;
                    int converted = swr_convert(swr,
                        reinterpret_cast<uint8_t**>(&outBuf), outSamples,
                        const_cast<const uint8_t**>(frame->extended_data), frame->nb_samples);
                    // Trim to actual count
                    allSamples.resize(prevSize + (converted > 0 ? converted : 0));
                }
            }
        }
        av_packet_unref(pkt);

        if (progress && totalDurationTs > 0) {
            // Estimate progress from packet pts
            auto* stream = fmtCtx->streams[streamIdx];
            if (pkt->pts != AV_NOPTS_VALUE) {
                double sec = pkt->pts * av_q2d(stream->time_base);
                double total = static_cast<double>(fmtCtx->duration) / AV_TIME_BASE;
                if (total > 0)
                    progress(std::min(20.0, sec / total * 20.0));
            }
        }
    }

    // Flush decoder
    avcodec_send_packet(codecCtx, nullptr);
    while (avcodec_receive_frame(codecCtx, frame) >= 0) {
        int outSamples = swr_get_out_samples(swr, frame->nb_samples);
        size_t prevSize = allSamples.size();
        allSamples.resize(prevSize + outSamples);
        float* outBuf = allSamples.data() + prevSize;
        int converted = swr_convert(swr,
            reinterpret_cast<uint8_t**>(&outBuf), outSamples,
            const_cast<const uint8_t**>(frame->extended_data), frame->nb_samples);
        allSamples.resize(prevSize + (converted > 0 ? converted : 0));
    }

    // Flush resampler
    {
        int outSamples = swr_get_out_samples(swr, 0);
        if (outSamples > 0) {
            size_t prevSize = allSamples.size();
            allSamples.resize(prevSize + outSamples);
            float* outBuf = allSamples.data() + prevSize;
            int converted = swr_convert(swr,
                reinterpret_cast<uint8_t**>(&outBuf), outSamples, nullptr, 0);
            allSamples.resize(prevSize + (converted > 0 ? converted : 0));
        }
    }

    av_frame_free(&frame);
    av_packet_free(&pkt);
    swr_free(&swr);
    avcodec_free_context(&codecCtx);
    avformat_close_input(&fmtCtx);

    if (allSamples.empty()) {
        delete data;
        return nullptr;
    }
    if (progress) progress(20.0);

    data->totalDurationSeconds = static_cast<double>(allSamples.size()) / sampleRate;

    const int numThreads = std::max(1u, std::thread::hardware_concurrency());

    // ── Generate peaks (AVX, parallel) ───────────────────────────
    int samplesPerBlock = sampleRate / static_cast<int>(data->samplesPerSecond);
    if (samplesPerBlock == 0) samplesPerBlock = 1;
    int totalBlocks = (static_cast<int>(allSamples.size()) + samplesPerBlock - 1) / samplesPerBlock;
    data->peaks.resize(totalBlocks);

    {
        std::vector<std::thread> threads;
        int blocksPerThread = (totalBlocks + numThreads - 1) / numThreads;
        for (int t = 0; t < numThreads; ++t) {
            int bStart = t * blocksPerThread;
            int bEnd = std::min(bStart + blocksPerThread, totalBlocks);
            if (bStart >= bEnd) break;
            threads.emplace_back([&, bStart, bEnd]() {
                for (int b = bStart; b < bEnd; ++b) {
                    int startIdx = b * samplesPerBlock;
                    int count = std::min(samplesPerBlock, static_cast<int>(allSamples.size()) - startIdx);
                    float mn, mx;
                    peaks_avx(allSamples.data() + startIdx, count, mn, mx);
                    data->peaks[b] = {mn, mx};
                }
            });
        }
        for (auto& th : threads) th.join();
    }
    if (progress) progress(30.0);

    // ── Spectrogram (AVX FFT, parallel) ──────────────────────────
    const int fftSize = 2048;
    const int stepSize = 256;
    const int numLogBins = 256;
    const float minFreq = 120.0f;
    const float maxFreq = static_cast<float>(sampleRate) / 2.0f;
    const float invFftSize = 1.0f / fftSize;
    const float gain = 2.0f;

    auto& mono = allSamples;
    int monoLen = static_cast<int>(mono.size());

    // Pre-calculate log freq bin mapping (shared read-only)
    std::vector<int> binMapping(fftSize / 2);
    {
        float minLog = std::log10(minFreq);
        float maxLog = std::log10(maxFreq);
        float logRange = maxLog - minLog;
        std::vector<float> logFreq(numLogBins + 1);
        for (int i = 0; i <= numLogBins; ++i)
            logFreq[i] = std::pow(10.0f, minLog + i * logRange / numLogBins);

        for (int i = 0; i < fftSize / 2; ++i) {
            float fftFreq = static_cast<float>(i) * sampleRate / fftSize;
            int j = 0;
            while (j < numLogBins && logFreq[j + 1] < fftFreq)
                ++j;
            binMapping[i] = j;
        }
    }

    // Precompute Hann window (shared read-only, aligned for AVX)
    alignas(32) float hannWindow[fftSize];
    for (int i = 0; i < fftSize; ++i)
        hannWindow[i] = 0.5f * (1.0f - std::cos(2.0f * M_PI * i / (fftSize - 1)));

    int totalSteps = std::max(0, (monoLen - fftSize) / stepSize);
    data->spectrogram.resize(totalSteps + 1);
    data->spectrogramSlicesPerSecond = static_cast<double>(sampleRate) / stepSize;

    {
        std::atomic<int> completedSteps{0};
        std::vector<std::thread> threads;
        int stepsPerThread = (totalSteps + 1 + numThreads - 1) / numThreads;

        for (int t = 0; t < numThreads; ++t) {
            int sStart = t * stepsPerThread;
            int sEnd = std::min(sStart + stepsPerThread, totalSteps + 1);
            if (sStart >= sEnd) break;
            threads.emplace_back([&, sStart, sEnd]() {
                // Per-thread aligned buffers
                alignas(32) float fftRe[fftSize];
                alignas(32) float fftIm[fftSize];
                alignas(32) float mag[fftSize / 2];

                for (int step = sStart; step < sEnd; ++step) {
                    int offset = step * stepSize;

                    apply_hann_avx(mono.data() + offset, hannWindow, fftRe, fftIm, fftSize, gain);
                    fft_avx(fftRe, fftIm, fftSize);
                    magnitude_avx(fftRe, fftIm, mag, fftSize / 2, invFftSize);

                    // Bin into logarithmic frequency bands
                    float logBinMag[numLogBins] = {};
                    int logBinCount[numLogBins] = {};

                    for (int j = 0; j < fftSize / 2; ++j) {
                        int bin = binMapping[j];
                        if (bin < numLogBins) {
                            logBinMag[bin] += mag[j];
                            logBinCount[bin]++;
                        }
                    }

                    // Convert to dB
                    std::vector<float> finalMag(numLogBins);
                    float lastVal = -90.0f;
                    for (int j = 0; j < numLogBins; ++j) {
                        if (logBinCount[j] > 0) {
                            float avg = logBinMag[j] / logBinCount[j];
                            finalMag[j] = 20.0f * std::log10(avg);
                            lastVal = finalMag[j];
                        } else {
                            finalMag[j] = lastVal;
                        }
                    }
                    data->spectrogram[step] = std::move(finalMag);

                    completedSteps.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }

        // Progress reporting from main thread while workers run
        if (progress) {
            int total = totalSteps + 1;
            while (true) {
                int done = completedSteps.load(std::memory_order_relaxed);
                progress(30.0 + static_cast<double>(done) / total * 70.0);
                if (done >= total) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }

        for (auto& th : threads) th.join();
    }

    if (progress) progress(100.0);
    return data;
}
