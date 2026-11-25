#pragma once

// VOMeter engine: trim -> true-peak limiter, plus dialogue-gated loudness
// metering of the (latency-aligned) input and of the output.
//
// Everything here is allocation-free after prepare() and runs on the audio thread.

#include <juce_core/juce_core.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <vector>

namespace vometer
{
inline double powToDb (double p) noexcept { return 10.0 * std::log10 (std::max (p, 1.0e-30)); }
inline float dbToGain (float db) noexcept { return std::pow (10.0f, db / 20.0f); }
inline float gainToDb (float g) noexcept { return 20.0f * std::log10 (std::max (g, 1.0e-30f)); }
inline double powToLufs (double p) noexcept { return -0.691 + powToDb (p); }

constexpr float kCeilingDbTP = -2.5f;        // the delivery ceiling the user asked for
constexpr float kCeilingMarginDb = 0.05f;    // internal safety so other meters agree
constexpr float kOnTargetLU = 2.0f;          // target turns green within +/- this
constexpr float kTrimRangeDb = 18.0f;
constexpr float kMatchRangeDb = 24.0f;      // Match gain, set by the Match button

// Dialogue-gated loudness uses speech gating *instead of* the -10 LU relative
// level gate (only the -70 LUFS absolute gate stays), as dialogue-gated meters
// do. Applying both drops quiet speech and reads about 0.4 LU high on real VO
// (checked against Youlean's dialogue-gated reading of the same take).
constexpr double kNoRelativeGate = 1000.0;

// -----------------------------------------------------------------------------
// Double-precision biquad (RBJ cookbook), transposed direct form II.
// -----------------------------------------------------------------------------
struct Biquad
{
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;

    void reset() noexcept { z1 = z2 = 0; }

    double process (double x) noexcept
    {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }

    void setHighShelf (double sr, double freq, double q, double gainDb) noexcept
    {
        const double A = std::pow (10.0, gainDb / 40.0);
        const double w0 = juce::MathConstants<double>::twoPi * freq / sr;
        const double cw = std::cos (w0), alpha = std::sin (w0) / (2.0 * q);
        const double sA = 2.0 * std::sqrt (A) * alpha;
        set (A * ((A + 1) + (A - 1) * cw + sA), -2 * A * ((A - 1) + (A + 1) * cw),
             A * ((A + 1) + (A - 1) * cw - sA), (A + 1) - (A - 1) * cw + sA,
             2 * ((A - 1) - (A + 1) * cw), (A + 1) - (A - 1) * cw - sA);
    }

    void setHighPass (double sr, double freq, double q) noexcept
    {
        const double w0 = juce::MathConstants<double>::twoPi * freq / sr;
        const double cw = std::cos (w0), alpha = std::sin (w0) / (2.0 * q);
        set ((1 + cw) / 2, -(1 + cw), (1 + cw) / 2, 1 + alpha, -2 * cw, 1 - alpha);
    }

    void setLowPass (double sr, double freq, double q) noexcept
    {
        const double w0 = juce::MathConstants<double>::twoPi * freq / sr;
        const double cw = std::cos (w0), alpha = std::sin (w0) / (2.0 * q);
        set ((1 - cw) / 2, 1 - cw, (1 - cw) / 2, 1 + alpha, -2 * cw, 1 - alpha);
    }

private:
    void set (double nb0, double nb1, double nb2, double na0, double na1, double na2) noexcept
    {
        b0 = nb0 / na0; b1 = nb1 / na0; b2 = nb2 / na0; a1 = na1 / na0; a2 = na2 / na0;
    }
};

// ITU-R BS.1770 K-weighting: high shelf + high pass. Coefficients by the
// libebur128 formulation, which reproduces the standard's 48 kHz filter exactly
// and adapts it to other sample rates.
struct KWeighting
{
    Biquad shelf, hp;

    void prepare (double sr) noexcept
    {
        {
            const double f0 = 1681.974450955533, G = 3.999843853973347, Q = 0.7071752369554196;
            const double K = std::tan (juce::MathConstants<double>::pi * f0 / sr);
            const double Vh = std::pow (10.0, G / 20.0), Vb = std::pow (Vh, 0.4996667741545416);
            const double a0 = 1.0 + K / Q + K * K;
            shelf.b0 = (Vh + Vb * K / Q + K * K) / a0;
            shelf.b1 = 2.0 * (K * K - Vh) / a0;
            shelf.b2 = (Vh - Vb * K / Q + K * K) / a0;
            shelf.a1 = 2.0 * (K * K - 1.0) / a0;
            shelf.a2 = (1.0 - K / Q + K * K) / a0;
        }
        {
            const double f0 = 38.13547087602444, Q = 0.5003270373238773;
            const double K = std::tan (juce::MathConstants<double>::pi * f0 / sr);
            const double a0 = 1.0 + K / Q + K * K;
            hp.b0 = 1.0; hp.b1 = -2.0; hp.b2 = 1.0;
            hp.a1 = 2.0 * (K * K - 1.0) / a0;
            hp.a2 = (1.0 - K / Q + K * K) / a0;
        }
        reset();
    }
    void reset() noexcept { shelf.reset(); hp.reset(); }
    double process (double x) noexcept { return hp.process (shelf.process (x)); }
};

// -----------------------------------------------------------------------------
// Windowed-sinc interpolator for true-peak detection. Push one sample at a time;
// it reports the largest |value| on the interval [n0, n0 + 1) of the signal,
// L points per sample, where n0 lags the newest sample by `delay` (= Taps / 2).
// Phase 0 is the sample itself, so the result is never below the sample peak.
// -----------------------------------------------------------------------------
template <int L, int Taps>
class TruePeakInterpolator
{
public:
    static constexpr int delay = Taps / 2;

    TruePeakInterpolator()
    {
        // tap i covers sample n0 + (i - (Taps/2 - 1)), i.e. offsets -(Taps/2-1) .. Taps/2
        const double beta = 8.0;
        const double i0b = bessel0 (beta);
        for (int p = 0; p < L; ++p)
            for (int i = 0; i < Taps; ++i)
            {
                const double off = (double) (i - (Taps / 2 - 1));
                const double t = (double) p / L - off;            // distance from that sample
                const double sinc = std::abs (t) < 1.0e-12 ? 1.0 : std::sin (juce::MathConstants<double>::pi * t) / (juce::MathConstants<double>::pi * t);
                const double r = t / (Taps / 2.0);
                const double w = std::abs (r) >= 1.0 ? 0.0 : bessel0 (beta * std::sqrt (1.0 - r * r)) / i0b;
                coef[(size_t) p][(size_t) i] = (float) (sinc * w);
            }
        reset();
    }

    void reset() noexcept { hist.fill (0.0f); pos = 0; }

    // Pushes the newest sample. Returns max |x(t)| for t in [n0, n0 + 1).
    float push (float x) noexcept
    {
        hist[(size_t) pos] = x;
        hist[(size_t) (pos + Taps)] = x;   // mirrored so a window never wraps
        pos = (pos + 1) % Taps;
        // oldest sample in the window is at hist[pos]; window = hist[pos .. pos+Taps-1]
        const float* w = &hist[(size_t) pos];
        float peak = 0.0f;
        for (int p = 0; p < L; ++p)
        {
            float acc = 0.0f;
            const auto& c = coef[(size_t) p];
            for (int i = 0; i < Taps; ++i)
                acc += c[(size_t) i] * w[i];
            peak = std::max (peak, std::abs (acc));
        }
        return peak;
    }

    // The sample at n0 (the one the last push() result starts at).
    float delayedSample() const noexcept { return hist[(size_t) (pos + Taps / 2 - 1)]; }

private:
    static double bessel0 (double x) noexcept
    {
        double sum = 1.0, term = 1.0;
        for (int k = 1; k < 30; ++k)
        {
            term *= (x / (2.0 * k)) * (x / (2.0 * k));
            sum += term;
        }
        return sum;
    }

    std::array<std::array<float, (size_t) Taps>, (size_t) L> coef {};
    std::array<float, (size_t) Taps * 2> hist {};
    int pos = 0;
};

// -----------------------------------------------------------------------------
// Stereo-linked true-peak brickwall limiter.
// Detection: 8x-oversampled (32-tap) peak of each sample's neighbourhood (so the
// inter-sample peaks either side of a sample count against it). Gain: min-hold
// over the lookahead window, release, then a box filter of the same length, so
// the gain has fully ramped down before a peak reaches the output.
// -----------------------------------------------------------------------------
class TruePeakLimiter
{
public:
    static constexpr int kMaxChannels = 2;
    using Interp = TruePeakInterpolator<8, 32>;

    void prepare (double sr, int numChannels)
    {
        channels = std::min (numChannels, kMaxChannels);
        window = std::max (2, (int) std::round (0.0015 * sr));
        relCoef = (float) std::exp (-1.0 / (0.080 * sr));
        required.assign ((size_t) window, 1.0f);
        smoothed.assign ((size_t) window, 1.0f);
        for (auto& d : delay) d.assign ((size_t) window, 0.0f);
        ceiling = dbToGain (kCeilingDbTP - kCeilingMarginDb);
        reset();
    }

    void reset() noexcept
    {
        for (auto& i : interp) i.reset();
        std::fill (required.begin(), required.end(), 1.0f);
        std::fill (smoothed.begin(), smoothed.end(), 1.0f);
        for (auto& d : delay) std::fill (d.begin(), d.end(), 0.0f);
        pos = 0; releaseGain = 1.0f; boxSum = (double) window; gainOut = 1.0f; prevInterval = 0.0f;
    }

    int getLatency() const noexcept { return Interp::delay + window - 1; }

    void processFrame (float* frame, int numCh) noexcept
    {
        numCh = std::min (numCh, channels);
        float interval = 0.0f;
        float x0[kMaxChannels] {};
        for (int ch = 0; ch < numCh; ++ch)
        {
            interval = std::max (interval, interp[ch].push (frame[ch]));
            x0[ch] = interp[ch].delayedSample();
        }
        const float peak = std::max (interval, prevInterval);  // ISPs either side of x0
        prevInterval = interval;

        required[(size_t) pos] = peak > ceiling ? ceiling / peak : 1.0f;

        float held = 1.0f;
        for (float r : required)
            held = std::min (held, r);

        if (held < releaseGain) releaseGain = held;
        else                    releaseGain = held + relCoef * (releaseGain - held);

        boxSum += (double) releaseGain - (double) smoothed[(size_t) pos];
        smoothed[(size_t) pos] = releaseGain;
        gainOut = std::min (1.0f, (float) (boxSum / (double) window));

        const int readPos = (pos + 1) % window;
        for (int ch = 0; ch < numCh; ++ch)
        {
            auto& d = delay[(size_t) ch];
            d[(size_t) pos] = x0[ch];
            frame[ch] = juce::jlimit (-ceiling, ceiling, d[(size_t) readPos] * gainOut); // safety only
        }
        pos = readPos;
    }

    float getReductionDb() const noexcept { return std::max (0.0f, -gainToDb (gainOut)); }

private:
    Interp interp[kMaxChannels];
    int channels = 2, window = 2, pos = 0;
    float ceiling = 1.0f, relCoef = 0.0f, releaseGain = 1.0f, gainOut = 1.0f, prevInterval = 0.0f;
    double boxSum = 2.0;
    std::vector<float> required, smoothed;
    std::vector<float> delay[kMaxChannels];
};

// -----------------------------------------------------------------------------
// Histogram of block loudness for exact-as-the-standard integration over the
// whole measurement (no memory limit): absolute gate -70 LUFS, then a relative
// gate `relGateLU` below the abs-gated mean, recomputed over every block so far.
// -----------------------------------------------------------------------------
class LoudnessHistogram
{
public:
    static constexpr double lo = -70.0, hi = 10.0, step = 0.02;
    static constexpr int numBins = (int) ((hi - lo) / step);

    LoudnessHistogram() : count ((size_t) numBins, 0.0), power ((size_t) numBins, 0.0) {}

    void reset() noexcept
    {
        std::fill (count.begin(), count.end(), 0.0);
        std::fill (power.begin(), power.end(), 0.0);
        total = 0;
    }

    void add (double blockPow) noexcept
    {
        const double lufs = powToLufs (blockPow);
        if (! (lufs > lo)) return;                     // absolute gate
        const int b = juce::jlimit (0, numBins - 1, (int) ((lufs - lo) / step));
        count[(size_t) b] += 1.0;
        power[(size_t) b] += blockPow;
        ++total;
    }

    long long blocks() const noexcept { return total; }

    // Integrated loudness in LUFS, or NaN when nothing has passed the gates.
    double integrated (double relGateLU = 10.0) const noexcept
    {
        if (total == 0) return std::numeric_limits<double>::quiet_NaN();
        double n = 0, p = 0;
        for (int b = 0; b < numBins; ++b) { n += count[(size_t) b]; p += power[(size_t) b]; }
        const double thr = powToLufs (p / n) - relGateLU;
        const int first = juce::jlimit (0, numBins, (int) std::ceil ((thr - lo) / step - 0.5));
        n = 0; p = 0;
        for (int b = first; b < numBins; ++b) { n += count[(size_t) b]; p += power[(size_t) b]; }
        return n > 0 ? powToLufs (p / n) : std::numeric_limits<double>::quiet_NaN();
    }

    // EBU Tech 3342 loudness range of short-term values added here
    // (relative gate -20 LU, 10th to 95th percentile). NaN if too little data.
    double loudnessRange() const noexcept
    {
        if (total < 10) return std::numeric_limits<double>::quiet_NaN();
        double n = 0, p = 0;
        for (int b = 0; b < numBins; ++b) { n += count[(size_t) b]; p += power[(size_t) b]; }
        const double thr = powToLufs (p / n) - 20.0;
        const int first = juce::jlimit (0, numBins, (int) std::ceil ((thr - lo) / step - 0.5));
        double kept = 0;
        for (int b = first; b < numBins; ++b) kept += count[(size_t) b];
        if (kept < 2) return std::numeric_limits<double>::quiet_NaN();
        auto percentile = [&] (double q)
        {
            const double target = q * kept;
            double acc = 0;
            for (int b = first; b < numBins; ++b)
            {
                acc += count[(size_t) b];
                if (acc >= target) return lo + (b + 0.5) * step;
            }
            return hi;
        };
        return percentile (0.95) - percentile (0.10);
    }

private:
    std::vector<double> count, power;
    long long total = 0;
};

// -----------------------------------------------------------------------------
// Speech detector. Works on 20 ms frames of the signal; a 400 ms loudness block
// counts as dialogue when
//   * enough of its frames are "voice active": above the noise floor by 10 dB,
//     above -60 LUFS, and with most of their energy in the voice band
//     (250 Hz - 3.5 kHz), which rules out hum, rumble and hiss; and
//   * the level of those active frames moves the way speech does (syllables),
//     which rules out steady sounds such as tones, room tone and noise beds.
// It's a level/spectrum/modulation heuristic, not Dolby Dialogue Intelligence.
// -----------------------------------------------------------------------------
class SpeechDetector
{
public:
    void prepare (double sr) noexcept
    {
        frameLen = std::max (1, (int) std::round (0.02 * sr));
        floorRisePerFrame = 1.0 * 0.02;  // 1 dB per second
        bandHp.setHighPass (sr, 250.0, 0.7071);
        bandLp.setLowPass (sr, 3500.0, 0.7071);
        fullHp.setHighPass (sr, 40.0, 0.7071);
        reset();
    }

    void reset() noexcept
    {
        bandHp.reset(); bandLp.reset(); fullHp.reset();
        accK = accBand = accFull = 0; n = 0;
        floorDb = -90.0; framesSeen = 0; ringPos = 0;
        frameDbRing.fill (-90.0f);
        activeRing.fill (0);
    }

    // kPow: K-weighted power summed over channels; mono: the channel average.
    void push (double kPow, double mono) noexcept
    {
        const double full = fullHp.process (mono);
        const double band = bandLp.process (bandHp.process (mono));
        accK += kPow; accFull += full * full; accBand += band * band;
        if (++n < frameLen) return;

        const double frameLufs = powToLufs (accK / n);
        const double ratio = accFull > 1.0e-20 ? accBand / accFull : 0.0;

        if (frameLufs < floorDb) floorDb = std::max (-90.0, frameLufs);
        else                     floorDb = std::min (frameLufs, floorDb + floorRisePerFrame);

        const bool active = frameLufs > -60.0 && frameLufs > floorDb + 10.0 && ratio > 0.4;

        frameDbRing[(size_t) ringPos] = (float) std::max (frameLufs, floorDb);
        activeRing[(size_t) ringPos] = active ? 1 : 0;
        ringPos = (ringPos + 1) % kRing;
        ++framesSeen;
        accK = accFull = accBand = 0; n = 0;
    }

    // Decision for the 400 ms block that just ended.
    bool blockIsSpeech() const noexcept
    {
        if (framesSeen < kBlockFrames) return false;
        int active = 0;
        for (int i = 1; i <= kBlockFrames; ++i)
            active += activeRing[(size_t) ((ringPos - i + kRing) % kRing)];
        if (active < 4) return false;

        // Syllabic modulation, over the block's active frames only:
        //  * their levels must spread (standard deviation >= 3 dB), and
        //  * they must not be mostly steady (70% or more within +/-1.5 dB of
        //    the median), so a steady sound starting or stopping part-way
        //    through a frame doesn't look like speech.
        std::array<float, kBlockFrames> lv {};
        double sum = 0, sq = 0;
        int k = 0;
        for (int i = 1; i <= kBlockFrames; ++i)
        {
            const auto idx = (size_t) ((ringPos - i + kRing) % kRing);
            if (! activeRing[idx]) continue;
            const double v = frameDbRing[idx];
            lv[(size_t) k++] = (float) v;
            sum += v; sq += v * v;
        }
        const double mean = sum / k;
        const double sd = std::sqrt (std::max (0.0, sq / k - mean * mean));
        if (sd < 3.0) return false;

        auto sorted = lv;
        std::nth_element (sorted.begin(), sorted.begin() + k / 2, sorted.begin() + k);
        const float median = sorted[(size_t) (k / 2)];
        int steady = 0;
        for (int i = 0; i < k; ++i)
            steady += std::abs (lv[(size_t) i] - median) <= 1.5f ? 1 : 0;
        return steady < 0.7 * k;
    }

private:
    static constexpr int kRing = 32, kBlockFrames = 20;   // 20 ms frames; a block is 20 of them
    Biquad bandHp, bandLp, fullHp;
    double accK = 0, accBand = 0, accFull = 0, floorDb = -90.0, floorRisePerFrame = 0.02;
    int frameLen = 960, n = 0, ringPos = 0;
    long long framesSeen = 0;
    std::array<float, kRing> frameDbRing {};
    std::array<unsigned char, kRing> activeRing {};
};

// -----------------------------------------------------------------------------
// The meter: fed latency-aligned input (before gain) and output frames.
// Dialogue readings: speech blocks, absolute gate only. Programme readings:
// standard BS.1770 (absolute + relative gate).
// 400 ms blocks every 100 ms. The speech decision comes from the input (it is
// level-independent, so the trim can't change what counts as dialogue) and is
// shared by the input and output measurements.
// -----------------------------------------------------------------------------
class LoudnessMeter
{
public:
    LoudnessMeter() { reset(); }

    // Keeps the measurement: hosts call prepareToPlay at moments the user
    // doesn't expect (Logic does on entering and leaving an offline bounce), and
    // the reading must survive that. Only Reset Meter clears it.
    void prepare (double sr, int numChannels)
    {
        channels = std::min (numChannels, 2);
        hopLen = std::max (1, (int) std::round (0.1 * sr));
        for (auto& k : kIn) k.prepare (sr);
        for (auto& k : kOut) k.prepare (sr);
        for (auto& t : tpMeter) t.reset();
        speech.prepare (sr);
        restartBlocks();
    }

    // Clears the measurement (not the filters or the speech detector's history).
    void reset() noexcept
    {
        inDialogue.reset(); outDialogue.reset(); inProgramme.reset(); outProgramme.reset(); shortTerm.reset();
        tpMax = 0.0f; hopsMeasured = 0; speechHops = 0;
        for (auto& t : tpMeter) t.reset();
        updateCache();
    }

    // Starts block assembly afresh (e.g. when playback starts) so a block never
    // mixes audio from before and after a stop.
    void restartBlocks() noexcept
    {
        acc = accIn = 0; count = 0; filled = 0; subIdx = 0;
        hopIn.fill (0.0); hopOut.fill (0.0);
    }

    void pushFrame (const float* in, const float* out, int numCh, bool measuring) noexcept
    {
        numCh = std::min (numCh, channels);
        double pIn = 0, pOut = 0, mono = 0;
        for (int ch = 0; ch < numCh; ++ch)
        {
            const double ki = kIn[ch].process (in[ch]);
            const double ko = kOut[ch].process (out[ch]);
            pIn += ki * ki; pOut += ko * ko;
            mono += in[ch];
            if (measuring)
                tpMax = std::max (tpMax, tpMeter[ch].push (out[ch]));
        }
        speech.push (pIn, mono / std::max (1, numCh));

        if (! measuring) return;

        accIn += pIn; acc += pOut;
        if (++count < hopLen) return;

        hopIn[(size_t) subIdx] = accIn / hopLen;
        hopOut[(size_t) subIdx] = acc / hopLen;
        subIdx = (subIdx + 1) % kShortHops;
        filled = std::min (filled + 1, kShortHops);
        acc = accIn = 0; count = 0;
        ++hopsMeasured;

        if (filled >= 4)
        {
            const double bIn = average (hopIn, 4), bOut = average (hopOut, 4);
            inProgramme.add (bIn);
            outProgramme.add (bOut);
            if (speech.blockIsSpeech())
            {
                inDialogue.add (bIn);
                outDialogue.add (bOut);
                ++speechHops;
            }
        }
        if (filled >= kShortHops)
            shortTerm.add (average (hopOut, kShortHops));
        updateCache();
    }

    // Cached once per 100 ms hop (the histograms are too big to scan per block).
    double outputDialogueLufs() const noexcept { return cache[0]; }
    double inputDialogueLufs() const noexcept  { return cache[1]; }
    double outputProgrammeLufs() const noexcept { return cache[2]; }
    double inputProgrammeLufs() const noexcept  { return cache[3]; }
    double loudnessRange() const noexcept { return cache[4]; }
    float maxTruePeakDb() const noexcept { return tpMax > 0.0f ? gainToDb (tpMax) : -std::numeric_limits<float>::infinity(); }
    double measuredSeconds() const noexcept { return hopsMeasured * 0.1; }
    double speechSeconds() const noexcept { return speechHops * 0.1; }

private:
    static constexpr int kShortHops = 30;  // 3 s short-term window

    void updateCache() noexcept
    {
        cache[0] = outDialogue.integrated (kNoRelativeGate);
        cache[1] = inDialogue.integrated (kNoRelativeGate);
        cache[2] = outProgramme.integrated();
        cache[3] = inProgramme.integrated();
        cache[4] = shortTerm.loudnessRange();
    }

    template <size_t N>
    double average (const std::array<double, N>& hops, int last) const noexcept
    {
        double s = 0;
        for (int i = 1; i <= last; ++i)
            s += hops[(size_t) ((subIdx - i + kShortHops) % kShortHops)];
        return s / last;
    }

    int channels = 2, hopLen = 4800, count = 0, filled = 0, subIdx = 0;
    double acc = 0, accIn = 0;
    std::array<double, kShortHops> hopIn {}, hopOut {};
    KWeighting kIn[2], kOut[2];
    TruePeakInterpolator<8, 32> tpMeter[2];   // true-peak meter (8x, finer than BS.1770's 4x)
    SpeechDetector speech;
    LoudnessHistogram inDialogue, outDialogue, inProgramme, outProgramme, shortTerm;
    double cache[5] {};
    float tpMax = 0.0f;
    long long hopsMeasured = 0, speechHops = 0;
};
// -----------------------------------------------------------------------------
// Dialogue-gated and standard integrated loudness of one signal: the programme
// audio arriving on the sidechain. Same blocks, gates and speech detector as
// the narration meter.
// -----------------------------------------------------------------------------
class ProgrammeMeter
{
public:
    ProgrammeMeter() { reset(); }

    void prepare (double sr, int numChannels)   // keeps the measurement, like LoudnessMeter
    {
        channels = std::min (numChannels, 2);
        hopLen = std::max (1, (int) std::round (0.1 * sr));
        for (auto& k : kw) k.prepare (sr);
        speech.prepare (sr);
        restartBlocks();
    }

    void reset() noexcept
    {
        dialogue.reset(); full.reset();
        hopsMeasured = 0; speechHops = 0;
        cache[0] = cache[1] = std::numeric_limits<double>::quiet_NaN();
    }

    void restartBlocks() noexcept { acc = 0; count = 0; filled = 0; subIdx = 0; hops.fill (0.0); }

    void push (const float* x, int numCh, bool measuring) noexcept
    {
        numCh = std::min (numCh, channels);
        double p = 0, mono = 0;
        for (int ch = 0; ch < numCh; ++ch)
        {
            const double k = kw[ch].process (x[ch]);
            p += k * k;
            mono += x[ch];
        }
        speech.push (p, mono / std::max (1, numCh));
        if (! measuring) return;

        acc += p;
        if (++count < hopLen) return;
        hops[(size_t) subIdx] = acc / hopLen;
        subIdx = (subIdx + 1) % 4;
        filled = std::min (filled + 1, 4);
        acc = 0; count = 0;
        ++hopsMeasured;
        if (filled < 4) return;

        const double block = (hops[0] + hops[1] + hops[2] + hops[3]) * 0.25;
        full.add (block);
        if (speech.blockIsSpeech())
        {
            dialogue.add (block);
            ++speechHops;
        }
        cache[0] = dialogue.integrated (kNoRelativeGate);
        cache[1] = full.integrated();
    }

    double dialogueLufs() const noexcept { return cache[0]; }
    double programmeLufs() const noexcept { return cache[1]; }
    double speechSeconds() const noexcept { return speechHops * 0.1; }
    double measuredSeconds() const noexcept { return hopsMeasured * 0.1; }

private:
    int channels = 2, hopLen = 4800, count = 0, filled = 0, subIdx = 0;
    double acc = 0, cache[2] {};
    std::array<double, 4> hops {};
    KWeighting kw[2];
    SpeechDetector speech;
    LoudnessHistogram dialogue, full;
    long long hopsMeasured = 0, speechHops = 0;
};
} // namespace vometer
