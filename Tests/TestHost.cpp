// Headless tests for VOMeter (run by ctest on every platform in CI).
// Drives the plugin's processor directly, no plugin host needed.
//
//   VOMeterTest                          run the tests
//   VOMeterTest --render take.wav [-24 -27 ...]
//       measure a real take, apply the suggested trim for each target, write
//       take_VOMeter-24.wav etc. next to it and print the readings.

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <cstdio>

namespace
{
int failures = 0;

#define CHECK(cond, ...)                                          \
    do {                                                          \
        if (! (cond)) {                                           \
            ++failures;                                           \
            std::printf ("FAIL %s:%d: ", __FILE__, __LINE__);     \
            std::printf (__VA_ARGS__);                            \
            std::printf ("\n");                                   \
        }                                                         \
    } while (0)

constexpr int kBlock = 512;
const double kRates[] = { 44100.0, 48000.0, 96000.0 };

using Signal = juce::AudioBuffer<float>;

Signal silence (int channels, int samples)
{
    Signal s (channels, samples);
    s.clear();
    return s;
}

void setParam (VOMeterProcessor& p, const char* id, float plainValue)
{
    auto* param = p.apvts.getParameter (id);
    param->setValueNotifyingHost (param->convertTo0to1 (plainValue));
}

float getParam (VOMeterProcessor& p, const char* id)
{
    return p.apvts.getRawParameterValue (id)->load();
}

// Main bus mono, stereo or 5.1 (L R C LFE Ls Rs), plus the stereo programme side chain.
std::unique_ptr<VOMeterProcessor> makeProcessor (double sr, int channels)
{
    auto p = std::make_unique<VOMeterProcessor>();
    const auto set = channels == 1 ? juce::AudioChannelSet::mono()
                   : channels == 6 ? juce::AudioChannelSet::create5point1() : juce::AudioChannelSet::stereo();
    juce::AudioProcessor::BusesLayout layout;
    layout.inputBuses.add (set);
    layout.inputBuses.add (juce::AudioChannelSet::stereo());
    layout.outputBuses.add (set);
    const bool ok = p->setBusesLayout (layout);
    CHECK (ok, "layout with side chain should be supported");
    p->prepareToPlay (sr, kBlock);
    return p;
}

// Runs a copy of `in` (and optionally `sideChain` on the programme input)
// through the processor and returns the main output.
Signal process (VOMeterProcessor& p, const Signal& in, int block = kBlock, const Signal* sideChain = nullptr)
{
    const int mainCh = in.getNumChannels();
    const int total = std::max (p.getTotalNumInputChannels(), p.getTotalNumOutputChannels());
    Signal work (total, in.getNumSamples());
    work.clear();
    for (int c = 0; c < mainCh; ++c)
        work.copyFrom (c, 0, in, c, 0, in.getNumSamples());
    if (sideChain != nullptr)
        for (int c = 0; c < std::min (2, total - mainCh); ++c)
            work.copyFrom (mainCh + c, 0, *sideChain, std::min (c, sideChain->getNumChannels() - 1), 0,
                           std::min (in.getNumSamples(), sideChain->getNumSamples()));

    juce::MidiBuffer midi;
    for (int start = 0; start < work.getNumSamples(); start += block)
    {
        const int len = std::min (block, work.getNumSamples() - start);
        juce::AudioBuffer<float> view (work.getArrayOfWritePointers(), total, start, len);
        p.processBlock (view, midi);
    }
    Signal out (mainCh, in.getNumSamples());
    for (int c = 0; c < mainCh; ++c)
        out.copyFrom (c, 0, work, c, 0, in.getNumSamples());
    return out;
}

Signal sine (double sr, int channels, double seconds, double freq, float dbfs, double phase = 0.0)
{
    const int n = (int) (seconds * sr);
    const float a = juce::Decibels::decibelsToGain (dbfs);
    Signal s (channels, n);
    for (int i = 0; i < n; ++i)
        for (int ch = 0; ch < channels; ++ch)
            s.setSample (ch, i, a * (float) std::sin (juce::MathConstants<double>::twoPi * freq * i / sr + phase));
    return s;
}

Signal concat (std::initializer_list<Signal> parts)
{
    int n = 0, ch = 1;
    for (auto& p : parts) { n += p.getNumSamples(); ch = p.getNumChannels(); }
    Signal s (ch, n);
    int pos = 0;
    for (auto& p : parts)
    {
        for (int c = 0; c < ch; ++c)
            s.copyFrom (c, pos, p, c, 0, p.getNumSamples());
        pos += p.getNumSamples();
    }
    return s;
}

// --- Synthetic narration ------------------------------------------------------
// A voice-like test signal: a glottal-ish pulse train through three formants,
// shaped into syllables (with fricative bursts), words, phrases and pauses,
// over quiet room tone. Not real speech, but it moves the way speech does.
struct Bandpass
{
    double b0 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    Bandpass (double sr, double f, double q)
    {
        const double w0 = juce::MathConstants<double>::twoPi * f / sr, alpha = std::sin (w0) / (2 * q);
        const double a0 = 1 + alpha;
        b0 = alpha / a0; b2 = -alpha / a0; a1 = -2 * std::cos (w0) / a0; a2 = (1 - alpha) / a0;
    }
    double process (double x)
    {
        const double y = b0 * x + z1;
        z1 = -a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};

Signal synthNarration (double sr, int channels, double seconds, float peakDbfs, int seed,
                       float bedDbfs = -200.0f)
{
    juce::Random rng (seed);
    const int n = (int) (seconds * sr);
    std::vector<float> env ((size_t) n, 0.0f), fric ((size_t) n, 0.0f);

    // Envelope schedule: phrases of syllables separated by pauses.
    int pos = (int) (0.3 * sr);
    while (pos < n)
    {
        const int syllables = 6 + rng.nextInt (9);
        for (int s = 0; s < syllables && pos < n; ++s)
        {
            const int len = (int) ((0.14 + 0.14 * rng.nextDouble()) * sr);
            const float lvl = juce::Decibels::decibelsToGain (-4.0f * rng.nextFloat());
            for (int i = 0; i < len && pos + i < n; ++i)
            {
                const double x = (double) i / len;
                env[(size_t) (pos + i)] = lvl * (float) std::pow (std::sin (juce::MathConstants<double>::pi * x), 1.5);
            }
            if (rng.nextFloat() < 0.4f)  // fricative at the syllable start
                for (int i = 0; i < (int) (0.05 * sr) && pos + i < n; ++i)
                    fric[(size_t) (pos + i)] = 0.25f * lvl;
            pos += len + (int) ((0.02 + 0.08 * rng.nextDouble()) * sr);
        }
        pos += (int) ((0.4 + 0.8 * rng.nextDouble()) * sr);
    }

    Bandpass f1 (sr, 700, 4), f2 (sr, 1200, 5), f3 (sr, 2600, 6), fr (sr, 6000, 1.5), room (sr, 300, 0.5);
    Signal s (channels, n);
    double phase = 0;
    // Optional steady tone between the phrases (where the voice has been
    // silent for 150 ms), standing in for stings or SFX between lines.
    const float bed = juce::Decibels::decibelsToGain (bedDbfs);
    std::vector<float> bedMask ((size_t) n, 0.0f);
    {
        const int hold = (int) (0.15 * sr);
        int quiet = 0;
        for (int i = 0; i < n; ++i)
        {
            quiet = env[(size_t) i] > 0.0f || fric[(size_t) i] > 0.0f ? 0 : quiet + 1;
            bedMask[(size_t) i] = quiet > hold ? 1.0f : 0.0f;
        }
        // stop before the next phrase starts, too
        int until = 0;
        for (int i = n - 1; i >= 0; --i)
        {
            until = env[(size_t) i] > 0.0f || fric[(size_t) i] > 0.0f ? 0 : until + 1;
            if (until <= hold) bedMask[(size_t) i] = 0.0f;
        }
    }
    std::vector<float> voice ((size_t) n);
    float peak = 0;
    for (int i = 0; i < n; ++i)
    {
        const double t = i / sr;
        const double f0 = 125.0 + 20.0 * std::sin (juce::MathConstants<double>::twoPi * 0.4 * t);
        phase += f0 / sr;
        phase -= std::floor (phase);
        const double src = 2.0 * phase - 1.0;
        const double v = 1.0 * f1.process (src) + 0.6 * f2.process (src) + 0.3 * f3.process (src);
        const double noise = rng.nextDouble() * 2 - 1;
        const float x = (float) (v * env[(size_t) i] + fr.process (noise) * fric[(size_t) i]);
        voice[(size_t) i] = x;
        peak = std::max (peak, std::abs (x));
    }
    const float scale = juce::Decibels::decibelsToGain (peakDbfs) / peak;
    const float roomTone = juce::Decibels::decibelsToGain (-68.0f) * 3.0f;
    for (int i = 0; i < n; ++i)
    {
        const float x = voice[(size_t) i] * scale + roomTone * (float) room.process (rng.nextDouble() * 2 - 1)
                        + bed * bedMask[(size_t) i] * (float) std::sin (juce::MathConstants<double>::twoPi * 440.0 * i / sr);
        for (int ch = 0; ch < channels; ++ch)
            s.setSample (ch, i, x);
    }
    return s;
}

// --- Reference true-peak meter (32x, long windowed sinc), for checking the limiter.
float referenceTruePeakDb (const Signal& s, float onlyAboveDb)
{
    constexpr int L = 32, half = 24;
    const float gate = juce::Decibels::decibelsToGain (onlyAboveDb);
    float peak = 0;
    for (int ch = 0; ch < s.getNumChannels(); ++ch)
    {
        const float* x = s.getReadPointer (ch);
        const int n = s.getNumSamples();
        for (int i = half; i < n - half; ++i)  // the buffer's edges would read as steps
        {
            peak = std::max (peak, std::abs (x[i]));
            if (std::abs (x[i]) < gate && (i + 1 >= n || std::abs (x[i + 1]) < gate))
                continue;  // inter-sample peaks only matter next to big samples
            for (int p = 1; p < L; ++p)
            {
                const double t = (double) p / L;
                double acc = 0;
                for (int k = -half + 1; k <= half; ++k)
                {
                    const int j = i + k;
                    if (j < 0 || j >= n) continue;
                    const double d = t - k;
                    const double r = d / half;
                    const double w = 0.42 + 0.5 * std::cos (juce::MathConstants<double>::pi * r) + 0.08 * std::cos (2 * juce::MathConstants<double>::pi * r);
                    acc += x[j] * std::sin (juce::MathConstants<double>::pi * d) / (juce::MathConstants<double>::pi * d) * w;
                }
                peak = std::max (peak, (float) std::abs (acc));
            }
        }
    }
    return juce::Decibels::gainToDecibels (peak, -200.0f);
}

// A host play head whose transport can be stopped and started.
struct TestPlayHead : juce::AudioPlayHead
{
    bool playing = true;
    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo info;
        info.setIsPlaying (playing);
        return info;
    }
};

// =============================================================================

void testLatencyAndTransparency()
{
    // Promise: at 0 dB trim on normal VO levels the audio is untouched, only
    // delayed by the latency the plugin reports (so Logic compensates it exactly).
    for (double sr : kRates)
        for (int ch : { 1, 2 })
        {
            auto p = makeProcessor (sr, ch);
            const int lat = p->getLatencySamples();
            CHECK (lat > 0 && lat < (int) (0.003 * sr), "latency %d samples at %.0f Hz should be under 3 ms", lat, sr);
            const auto in = synthNarration (sr, ch, 3.0, -10.0f, 1);
            const auto out = process (*p, in);
            float err = 0;
            for (int c = 0; c < ch; ++c)
                for (int i = lat; i < in.getNumSamples(); ++i)
                    err = std::max (err, std::abs (out.getSample (c, i) - in.getSample (c, i - lat)));
            CHECK (err < 1.0e-6f, "0 dB trim should be transparent after latency (err %g, %.0f Hz, %d ch)", err, sr, ch);
        }
}

void testTrimGain()
{
    // Promise: on a mono (narration) track the trim is a plain gain change, in dB.
    auto p = makeProcessor (48000.0, 1);
    setParam (*p, VOMeterProcessor::trimId, -6.0f);
    const auto in = sine (48000.0, 1, 1.0, 1000.0, -20.0f);
    const auto out = process (*p, in);
    const int half = out.getNumSamples() / 2;
    const float gainDb = juce::Decibels::gainToDecibels (out.getMagnitude (0, half, half) / in.getMagnitude (0, half, half));
    CHECK (std::abs (gainDb + 6.0f) < 0.02f, "-6 dB trim gave %.3f dB", gainDb);

    auto* trim = dynamic_cast<juce::RangedAudioParameter*> (p->apvts.getParameter (VOMeterProcessor::trimId));
    CHECK (trim != nullptr && trim->isAutomatable(), "Trim must be automatable");
    CHECK (trim->getNormalisableRange().start == -18.0f && trim->getNormalisableRange().end == 18.0f,
           "Trim range should be -18 to +18 dB");
}

void testTruePeakCeiling()
{
    // Promise: whatever goes in and however far the trim is pushed, the output
    // never exceeds -2.5 dBTP (checked with an independent 32x true-peak meter).
    for (double sr : kRates)
        for (int ch : { 1, 2 })
        {
            // Inter-sample-peak torture: a sine at fs/4 sampled 45 degrees off its
            // peaks reads 3 dB lower as samples than it really is.
            const auto isp = sine (sr, ch, 1.0, sr / 4.0, -1.0f, juce::MathConstants<double>::pi / 4.0);
            const auto voice = synthNarration (sr, ch, 4.0, -6.0f, 2);
            for (auto* sig : { &isp, &voice })
                for (float trimDb : { 0.0f, 18.0f })
                {
                    auto p = makeProcessor (sr, ch);
                    setParam (*p, VOMeterProcessor::matchId, trimDb);
                    const auto out = process (*p, *sig);
                    const float tp = referenceTruePeakDb (out, -6.0f);
                    CHECK (tp <= -2.5f, "true peak %.3f dBTP above -2.5 (%.0f Hz, %d ch, trim %+.0f, %s)",
                           tp, sr, ch, trimDb, sig == &isp ? "ISP sine" : "narration");
                    const float meterTp = p->readings.maxTruePeak.load();
                    CHECK (meterTp <= -2.5f && std::abs (meterTp - tp) < 0.1f, "plugin's own max true peak read %.3f dBTP (ref %.3f, %.0f Hz, %d ch, trim %+.0f, %s)",
                           meterTp, tp, sr, ch, trimDb, sig == &isp ? "ISP sine" : "narration");
                }
        }
}

void testProgrammeMeterMatchesStandard()
{
    // Promise: the underlying BS.1770 measurement agrees with the EBU Tech 3341
    // reference signals (stereo 1 kHz at -23 dBFS reads -23.0; test 4,
    // -72/-36/-23/-36/-72 dBFS, reads -23.0), to within 0.1 LU.
    for (double sr : kRates)
    {
        auto p = makeProcessor (sr, 2);
        process (*p, sine (sr, 2, 20.0, 1000.0, -23.0f));
        const float a = p->readings.outProgramme.load();
        CHECK (std::abs (a + 23.0f) < 0.1f, "-23 dBFS 1 kHz stereo read %.2f LUFS at %.0f Hz", a, sr);

        auto q = makeProcessor (sr, 2);
        process (*q, concat ({ sine (sr, 2, 10.0, 1000.0, -72.0f), sine (sr, 2, 10.0, 1000.0, -36.0f),
                               sine (sr, 2, 60.0, 1000.0, -23.0f), sine (sr, 2, 10.0, 1000.0, -36.0f),
                               sine (sr, 2, 10.0, 1000.0, -72.0f) }));
        const float b = q->readings.outProgramme.load();
        CHECK (std::abs (b + 23.0f) < 0.1f, "Tech 3341 gating test read %.2f LUFS at %.0f Hz", b, sr);
    }
}

void testSteadyToneIsNotDialogue()
{
    // Promise: a steady tone (line-up tone, hum) never counts as dialogue.
    auto p = makeProcessor (48000.0, 2);
    process (*p, sine (48000.0, 2, 15.0, 1000.0, -20.0f));
    CHECK (std::isnan (p->readings.outDialogue.load()), "a steady tone was counted as dialogue (%.2f)", p->readings.outDialogue.load());
    CHECK (p->readings.speechSeconds.load() < 0.5f, "tone gave %.1f s of speech", p->readings.speechSeconds.load());
}

void testDialogueMatchesProgrammeOnCleanVO()
{
    // Promise: on a clean narration track, the dialogue-gated reading is close
    // to the standard integrated reading (within 1 LU), and most of the take is
    // recognised as speech.
    for (double sr : kRates)
    {
        auto p = makeProcessor (sr, 1);
        process (*p, synthNarration (sr, 1, 40.0, -10.0f, 3));
        const float dia = p->readings.outDialogue.load(), prog = p->readings.outProgramme.load();
        const float share = p->readings.speechSeconds.load() / p->readings.measuredSeconds.load();
        CHECK (! std::isnan (dia) && std::abs (dia - prog) < 1.0f, "dialogue %.2f vs programme %.2f LUFS at %.0f Hz", dia, prog, sr);
        CHECK (share > 0.5f && share < 0.95f, "speech share %.0f%% (expected most of the take, not the pauses)", 100 * share);
    }
}

void testNonSpeechBedIsIgnored()
{
    // Promise: a steady sound between the lines, louder than the voice (so it
    // drags the standard integrated reading up), barely moves the dialogue reading.
    auto clean = makeProcessor (48000.0, 1);
    process (*clean, synthNarration (48000.0, 1, 40.0, -10.0f, 4));
    auto bed = makeProcessor (48000.0, 1);
    process (*bed, synthNarration (48000.0, 1, 40.0, -10.0f, 4, -22.0f));

    const float d0 = clean->readings.outDialogue.load(), d1 = bed->readings.outDialogue.load();
    const float p0 = clean->readings.outProgramme.load(), p1 = bed->readings.outProgramme.load();
    std::printf ("  bed test: dialogue %.2f -> %.2f, programme %.2f -> %.2f LUFS\n", d0, d1, p0, p1);
    CHECK (std::abs (d1 - d0) < 0.5f, "dialogue reading moved %.2f LU with sound between lines", d1 - d0);
    CHECK (p1 - p0 > 1.5f * std::abs (d1 - d0), "the standard reading should move more (%.2f LU)", p1 - p0);
}

void testMatchHitsTarget()
{
    // Promise: play the take, press Match, reset, play again: with the trim at
    // 0 dB the dialogue loudness of the output lands on the target (within
    // 0.5 LU), for quiet and hot takes, at every sample rate. Match sets the
    // Match gain and leaves the trim alone.
    for (double sr : kRates)
        for (float target : { -24.0f, -27.0f, -23.0f })
            for (float peak : { -12.0f, -6.0f })
            {
                auto p = makeProcessor (sr, 2);
                setParam (*p, VOMeterProcessor::targetId, target);
                const auto take = synthNarration (sr, 2, 30.0, peak, 5);
                process (*p, take);
                CHECK (p->applySuggestedMatch(), "Match should work once dialogue was heard");
                CHECK (std::abs (getParam (*p, VOMeterProcessor::trimId)) < 0.01f, "Match must not touch the trim");
                p->requestMeterReset();
                process (*p, take);
                const float out = p->readings.outDialogue.load();
                CHECK (std::abs (out - target) < 0.5f, "after Match: %.2f LUFS for target %.0f (peak %.0f dBFS, %.0f Hz, match %+.1f)",
                       out, target, peak, sr, getParam (*p, VOMeterProcessor::matchId));
                CHECK (! p->readings.trimMovedSinceReset.load(), "trim didn't move after the reset");
            }
}

void testSuggestedMatchClamps()
{
    // Promise: a very quiet take suggests at most +24 dB (the Match gain's range).
    auto p = makeProcessor (48000.0, 1);
    setParam (*p, VOMeterProcessor::targetId, -10.0f);
    process (*p, synthNarration (48000.0, 1, 15.0, -30.0f, 6));
    const float s = p->getSuggestedMatchDb();
    CHECK (std::abs (s - 24.0f) < 0.01f, "suggested Match gain %.2f should clamp to +24", s);
}

void testTrimRidesOnTopOfMatch()
{
    // Promise: the trim rides on top of the Match gain. With a ride of +3 dB in
    // place, Match still lands "trim at 0 dB" on the target, keeps the ride,
    // and the output sits 3 LU above the target. The suggestion doesn't depend
    // on the trim.
    auto p = makeProcessor (48000.0, 1);
    setParam (*p, VOMeterProcessor::targetId, -27.0f);
    setParam (*p, VOMeterProcessor::trimId, 3.0f);
    const auto take = synthNarration (48000.0, 1, 30.0, -12.0f, 11);
    process (*p, take);
    const float before = p->getSuggestedMatchDb();
    p->applySuggestedMatch();
    CHECK (std::abs (getParam (*p, VOMeterProcessor::trimId) - 3.0f) < 0.01f, "Match changed the trim");
    p->requestMeterReset();
    process (*p, take);
    const float out = p->readings.outDialogue.load();
    CHECK (std::abs (out - (-27.0f + 3.0f)) < 0.5f, "match + 3 dB ride read %.2f, expected about -24", out);
    CHECK (std::abs (p->getSuggestedMatchDb() - before) < 0.2f, "suggestion moved with the trim (%.2f vs %.2f)",
           p->getSuggestedMatchDb(), before);

    p->clearMatch();
    CHECK (std::abs (getParam (*p, VOMeterProcessor::matchId)) < 0.01f, "Clear should set Match gain to 0 dB");
    process (*p, silence (2, kBlock));
    CHECK (std::isnan (p->readings.outDialogue.load()), "Clear should reset the meter");
    CHECK (std::abs (getParam (*p, VOMeterProcessor::trimId) - 3.0f) < 0.01f, "Clear changed the trim");

    // Match resets the meter too (the readings were taken at the old gain), but not the trim.
    process (*p, take);
    CHECK (p->applySuggestedMatch(), "Match should work after replaying");
    process (*p, silence (2, kBlock));
    CHECK (std::isnan (p->readings.outDialogue.load()) && std::isnan (p->readings.inDialogue.load()),
           "Match should reset the meter");
    CHECK (std::abs (getParam (*p, VOMeterProcessor::trimId) - 3.0f) < 0.01f, "Match changed the trim");
}

void testProgrammeSideChain()
{
    // Promise: programme audio on the side chain is measured the same way as
    // the narration (dialogue-gated), Use sets the target from it, a steady
    // tone there never counts, and with nothing on the side chain the
    // window can say so.
    const double sr = 48000.0;
    const auto narration = synthNarration (sr, 1, 20.0, -12.0f, 12);
    const auto programmeAudio = synthNarration (sr, 2, 20.0, -9.0f, 13);

    auto reference = makeProcessor (sr, 2);   // the same audio on the main input
    process (*reference, programmeAudio);
    const float expected = reference->readings.inDialogue.load();

    auto p = makeProcessor (sr, 1);
    process (*p, narration, kBlock, &programmeAudio);
    const float prog = p->readings.programmeDialogue.load();
    CHECK (std::abs (prog - expected) < 0.05f, "programme read %.2f, main meter %.2f on the same audio", prog, expected);
    CHECK (p->readings.programmeSignal.load(), "side-chain signal should be detected");
    CHECK (p->useProgrammeAsTarget(), "Use should work with a programme reading");
    CHECK (std::abs (getParam (*p, VOMeterProcessor::targetId) - std::round (prog * 10.0f) / 10.0f) < 0.06f,
           "target %.2f should be the programme's %.2f", getParam (*p, VOMeterProcessor::targetId), prog);

    p->requestMeterReset();
    process (*p, silence (1, 2 * kBlock));
    CHECK (std::isnan (p->readings.programmeDialogue.load()), "reset should clear the programme reading");

    const auto tone = sine (sr, 2, 10.0, 1000.0, -20.0f);
    process (*p, synthNarration (sr, 1, 10.0, -12.0f, 14), kBlock, &tone);
    CHECK (std::isnan (p->readings.programmeDialogue.load()), "a tone on the side chain was counted as dialogue (%.2f LUFS)",
           p->readings.programmeDialogue.load());

    auto none = makeProcessor (sr, 2);
    process (*none, synthNarration (sr, 2, 5.0, -12.0f, 15));
    CHECK (! none->readings.programmeSignal.load() && std::isnan (none->readings.programmeDialogue.load()),
           "with a silent side chain there should be no programme signal or reading");
    CHECK (! none->useProgrammeAsTarget(), "Use should do nothing without a programme reading");

    // Logic, Side Chain "None": the side chain carries a copy of the track's own
    // audio. That must not be measured as programme.
    auto copy = makeProcessor (sr, 1);
    process (*copy, narration, kBlock, &narration);
    CHECK (! copy->readings.programmeSignal.load() && std::isnan (copy->readings.programmeDialogue.load()),
           "a side chain that copies the input was measured as programme (%.2f)", copy->readings.programmeDialogue.load());
}

void testResetClearsMeterNotTrim()
{
    // Promise: Reset Meter clears the readings but leaves the trim alone.
    auto p = makeProcessor (48000.0, 2);
    setParam (*p, VOMeterProcessor::trimId, 4.5f);
    process (*p, synthNarration (48000.0, 2, 10.0, -12.0f, 7));
    CHECK (! std::isnan (p->readings.outDialogue.load()), "should have a reading before reset");
    process (*p, silence (2, 4800));  // let the limiter's delayed tail out first
    p->requestMeterReset();
    process (*p, silence (2, 256));   // the reset happens on the audio thread's next block
    CHECK (std::isnan (p->readings.outDialogue.load()) && std::isnan (p->readings.inDialogue.load()), "reset should clear the loudness");
    CHECK (! std::isfinite (p->readings.maxTruePeak.load()), "reset should clear the max true peak");
    CHECK (p->readings.measuredSeconds.load() < 0.2f, "reset should clear the measured time");
    CHECK (std::abs (getParam (*p, VOMeterProcessor::trimId) - 4.5f) < 0.01f, "reset must not touch the trim");
}

void testOnlyMeasuresWhilePlaying()
{
    // Promise: while the transport is stopped, nothing is added to the reading.
    TestPlayHead head;
    auto p = makeProcessor (48000.0, 2);
    p->setPlayHead (&head);
    const auto take = synthNarration (48000.0, 2, 10.0, -12.0f, 8);

    head.playing = false;
    process (*p, take);
    CHECK (p->readings.measuredSeconds.load() == 0.0f && std::isnan (p->readings.outDialogue.load()),
           "measured %.1f s while stopped", p->readings.measuredSeconds.load());
    CHECK (! p->readings.measuring.load(), "should report not measuring");

    head.playing = true;
    process (*p, take);
    CHECK (std::abs (p->readings.measuredSeconds.load() - 10.0f) < 0.5f && ! std::isnan (p->readings.outDialogue.load()),
           "measured %.1f s while playing 10 s", p->readings.measuredSeconds.load());
    p->setPlayHead (nullptr);
}

void testOfflineBounce()
{
    // Promise: an offline bounce measures like real-time playback, and the
    // reading survives the host re-preparing the plugin as it enters and
    // leaves offline mode (JUCE's AU wrapper calls prepareToPlay both times).
    TestPlayHead head;
    head.playing = false;   // some hosts don't report "playing" while bouncing
    auto p = makeProcessor (48000.0, 2);
    p->setPlayHead (&head);
    const auto take = synthNarration (48000.0, 2, 20.0, -12.0f, 10);

    p->setNonRealtime (true);
    p->prepareToPlay (48000.0, 1024);
    process (*p, take, 1024);
    const float during = p->readings.outDialogue.load();
    CHECK (! std::isnan (during) && std::abs (p->readings.measuredSeconds.load() - 20.0f) < 0.5f,
           "offline bounce measured %.1f s, reading %.2f", p->readings.measuredSeconds.load(), during);

    p->setNonRealtime (false);
    p->prepareToPlay (48000.0, kBlock);
    process (*p, silence (2, kBlock));  // the host keeps running, stopped
    const float after = p->readings.outDialogue.load();
    CHECK (std::abs (after - during) < 1.0e-4f, "reading changed after the bounce ended (%.2f -> %.2f)", during, after);
    CHECK (std::abs (p->readings.measuredSeconds.load() - 20.0f) < 0.5f, "measured time changed after the bounce");

    // A second bounce of the same take, without a reset, gives the same reading.
    p->setNonRealtime (true);
    p->prepareToPlay (48000.0, 1024);
    process (*p, take, 1024);
    CHECK (std::abs (p->readings.outDialogue.load() - during) < 0.05f, "second bounce read %.2f vs %.2f",
           p->readings.outDialogue.load(), during);
    p->setPlayHead (nullptr);
}

void testMonoInStereoMix()
{
    // Promise: on a mono track, "Stereo mix" reads what the narration measures
    // once it plays in both channels of a stereo mix (the same as a stereo track
    // carrying it dual-mono), 3 dB above the mono track alone; "5.1 mix"
    // (centre channel only) reads the mono track as it is. Match follows.
    const double sr = 48000.0;
    const auto mono = synthNarration (sr, 1, 20.0, -12.0f, 16);
    const auto dual = synthNarration (sr, 2, 20.0, -12.0f, 16);

    auto st = makeProcessor (sr, 2);
    process (*st, dual);
    auto m = makeProcessor (sr, 1);
    process (*m, mono);
    auto five = makeProcessor (sr, 1);
    setParam (*five, VOMeterProcessor::mixId, 1.0f);
    process (*five, mono);

    const float s = st->readings.outDialogue.load(), a = m->readings.outDialogue.load(), b = five->readings.outDialogue.load();
    CHECK (std::abs (a - s) < 0.05f, "mono in stereo mix read %.2f, dual-mono stereo track %.2f", a, s);
    CHECK (std::abs ((a - b) - 3.01f) < 0.02f, "stereo vs 5.1 mix should differ by 3.01 dB (%.2f vs %.2f)", a, b);
    CHECK (std::abs (st->readings.mixOffsetDb.load()) < 1.0e-6f, "a stereo track gets no offset");

    // Matching to -27 in a stereo mix leaves the mono track itself at about -30.
    setParam (*m, VOMeterProcessor::targetId, -27.0f);
    m->applySuggestedMatch();
    m->requestMeterReset();
    process (*m, mono);
    const float inMix = m->readings.outDialogue.load();
    CHECK (std::abs (inMix + 27.0f) < 0.5f, "mono track matched in a stereo mix read %.2f", inMix);
    CHECK (std::abs ((inMix - m->readings.mixOffsetDb.load()) + 30.0f) < 0.5f, "the mono track itself should be about -30");
}

void testMixQuestion()
{
    // Promise: a mono track asks "stereo or 5.1 mix?" once; the answer sets the
    // mix, is saved with the session, and the question doesn't come back. A
    // stereo track never asks.
   #if JUCE_LINUX
    if (juce::SystemStats::getEnvironmentVariable ("DISPLAY", {}).isEmpty())
        return;
   #endif
    auto questionShown = [] (juce::AudioProcessorEditor& e)
    {
        auto* c = e.findChildWithID ("mixQuestion");
        return c != nullptr && c->isVisible();
    };

    auto mono = makeProcessor (48000.0, 1);
    CHECK (! mono->isMixFormatChosen(), "a new mono instance hasn't been asked yet");
    {
        std::unique_ptr<juce::AudioProcessorEditor> e (mono->createEditor());
        CHECK (questionShown (*e), "a mono track should ask stereo or 5.1");
    }
    mono->setMixFormat (true);
    CHECK (mono->isMixFormatChosen() && getParam (*mono, VOMeterProcessor::mixId) > 0.5f, "answering 5.1 should set the mix");
    {
        std::unique_ptr<juce::AudioProcessorEditor> e (mono->createEditor());
        CHECK (! questionShown (*e), "the question shouldn't come back once answered");
    }

    juce::MemoryBlock state;
    mono->getStateInformation (state);
    auto reopened = makeProcessor (48000.0, 1);
    reopened->setStateInformation (state.getData(), (int) state.getSize());
    CHECK (reopened->isMixFormatChosen(), "the answer should be saved with the session");

    auto stereo = makeProcessor (48000.0, 2);
    std::unique_ptr<juce::AudioProcessorEditor> e (stereo->createEditor());
    CHECK (! questionShown (*e), "a stereo track should never ask about the mix");
}

void testMeasureQuestion()
{
    // Promise: stereo and 5.1 tracks ask once whether to measure dialogue-gated
    // or integrated loudness; the answer is saved with the session. Mono tracks
    // (narration) never ask and always measure dialogue.
   #if JUCE_LINUX
    if (juce::SystemStats::getEnvironmentVariable ("DISPLAY", {}).isEmpty())
        return;
   #endif
    auto shown = [] (VOMeterProcessor& p, const char* id)
    {
        std::unique_ptr<juce::AudioProcessorEditor> e (p.createEditor());
        auto* c = e->findChildWithID (id);
        return c != nullptr && c->isVisible();
    };
    for (int ch : { 2, 6 })
    {
        auto p = makeProcessor (48000.0, ch);
        CHECK (shown (*p, "measureQuestion"), "a %d-channel track should ask dialogue-gated or integrated", ch);
        CHECK (! shown (*p, "mixQuestion"), "a %d-channel track shouldn't ask stereo or 5.1", ch);
        p->setMeasureMode (true);
        CHECK (p->isIntegratedMode() && ! shown (*p, "measureQuestion"), "answering should set the mode and close the question");

        juce::MemoryBlock state;
        p->getStateInformation (state);
        auto reopened = makeProcessor (48000.0, ch);
        reopened->setStateInformation (state.getData(), (int) state.getSize());
        CHECK (reopened->isMeasureModeChosen() && reopened->isIntegratedMode(), "the measure answer should be saved");
    }
    auto mono = makeProcessor (48000.0, 1);
    CHECK (! shown (*mono, "measureQuestion"), "a mono track shouldn't ask what to measure");
}

void testIntegratedMode()
{
    // Promise: in Integrated mode the readout, the Match gain and Use follow
    // standard BS.1770 integrated loudness of the whole signal (music and effects
    // included), not just the dialogue. Mono tracks stay dialogue-gated.
    const double sr = 48000.0;
    // Narration, then a louder music sting: the integrated level includes the sting.
    const auto mixSig = concat ({ synthNarration (sr, 2, 20.0, -14.0f, 21), sine (sr, 2, 10.0, 440.0, -16.0f) });

    auto p = makeProcessor (sr, 2);
    p->setMeasureMode (true);
    process (*p, mixSig);
    const auto& r = p->readings;
    CHECK (r.integratedMode.load(), "integrated mode should be reported");
    CHECK (std::abs (r.outLoudness.load() - r.outProgramme.load()) < 1.0e-4f, "integrated readout %.2f should be the BS.1770 integrated %.2f",
           r.outLoudness.load(), r.outProgramme.load());
    CHECK (std::abs (r.outLoudness.load() - r.outDialogue.load()) > 0.3f, "test signal should read differently gated (%.2f vs %.2f)",
           r.outLoudness.load(), r.outDialogue.load());

    setParam (*p, VOMeterProcessor::targetId, -23.0f);
    CHECK (p->applySuggestedMatch(), "Match should work in integrated mode");
    process (*p, mixSig);
    CHECK (std::abs (r.outLoudness.load() + 23.0f) < 0.3f, "Match in integrated mode read %.2f, expected -23", r.outLoudness.load());

    // No trim on a full mix (stereo / 5.1), dialogue-gated or integrated: a
    // trim value (or automation) changes nothing, and the knob is hidden.
    for (int ch : { 2, 6 })
    for (bool integ : { false, true })
    {
        const auto sig = ch == 2 ? mixSig : [&] { auto x = silence (6, mixSig.getNumSamples());
                                                  x.copyFrom (0, 0, mixSig, 0, 0, x.getNumSamples());
                                                  x.copyFrom (2, 0, mixSig, 1, 0, x.getNumSamples()); return x; }();
        auto a = makeProcessor (sr, ch), b = makeProcessor (sr, ch);
        a->setMeasureMode (integ); b->setMeasureMode (integ);
        setParam (*b, VOMeterProcessor::trimId, 6.0f);
        a->prepareToPlay (sr, kBlock); b->prepareToPlay (sr, kBlock);
        const auto oa = process (*a, sig), ob = process (*b, sig);
        float diff = 0;
        for (int c = 0; c < ch; ++c)
            for (int i = 0; i < oa.getNumSamples(); ++i)
                diff = std::max (diff, std::abs (oa.getSample (c, i) - ob.getSample (c, i)));
        CHECK (diff < 1.0e-7f, "the trim should be ignored on a %d-channel mix (diff %g, %s)", ch, diff, integ ? "integrated" : "dialogue-gated");
       #if ! JUCE_LINUX
        std::unique_ptr<juce::AudioProcessorEditor> e (b->createEditor());
        bool knobShown = false;
        for (auto* c : e->getChildren())
            if (auto* sl = dynamic_cast<juce::Slider*> (c))
                knobShown = knobShown || sl->isVisible();
        CHECK (! knobShown, "the trim knob should be hidden on a %d-channel mix", ch);
       #endif
    }

    // Programme on the side chain: Use takes its integrated loudness.
    auto q = makeProcessor (sr, 2);
    q->setMeasureMode (true);
    const auto narration = synthNarration (sr, 2, 30.0, -10.0f, 22);
    process (*q, narration, kBlock, &mixSig);
    CHECK (std::abs (q->readings.programmeLoudness.load() - q->readings.programmeDialogue.load()) > 0.3f,
           "programme should read integrated, not dialogue-gated");
    CHECK (q->useProgrammeAsTarget(), "Use should work");
    CHECK (std::abs (getParam (*q, VOMeterProcessor::targetId) - q->readings.programmeLoudness.load()) < 0.06f,
           "Use should take the programme's integrated loudness");

    // Mono: the measure parameter is ignored.
    auto m = makeProcessor (sr, 1);
    setParam (*m, VOMeterProcessor::measureId, 1.0f);
    const auto monoTake = synthNarration (sr, 1, 20.0, -12.0f, 23);
    process (*m, monoTake);
    CHECK (! m->readings.integratedMode.load() && std::abs (m->readings.outLoudness.load() - m->readings.outDialogue.load()) < 1.0e-4f,
           "a mono track should stay dialogue-gated");
}

void testFiveOne()
{
    // Promise: on a 5.1 track, loudness uses the BS.1770 channel weights (front
    // channels 1.0, surrounds +1.5 dB, LFE not counted), the limiter holds every
    // channel under -2.5 dBTP, and 0 dB is transparent.
    const double sr = 48000.0;
    auto toneIn = [&] (int channel, float dbfs)
    {
        auto s = silence (6, (int) (sr * 10.0));
        const auto t = sine (sr, 1, 10.0, 1000.0, dbfs);
        s.copyFrom (channel, 0, t, 0, 0, t.getNumSamples());
        auto p = makeProcessor (sr, 6);
        process (*p, s);
        return p->readings.outProgramme.load();
    };
    const float left = toneIn (0, -20.0f), centre = toneIn (2, -20.0f), surround = toneIn (4, -20.0f), lfe = toneIn (3, -20.0f);
    CHECK (std::abs (left - centre) < 0.05f, "L and C should weigh the same (%.2f vs %.2f)", left, centre);
    CHECK (std::abs ((surround - left) - 1.49f) < 0.05f, "a surround should read 1.5 dB above a front channel (%.2f vs %.2f)", surround, left);
    CHECK (std::isnan (lfe) || lfe < -69.0f, "the LFE shouldn't be measured (%.2f)", lfe);
    // Stereo reference: one channel of a stereo track at -20 dBFS reads the same as L of 5.1.
    {
        auto s = silence (2, (int) (sr * 10.0));
        const auto t = sine (sr, 1, 10.0, 1000.0, -20.0f);
        s.copyFrom (0, 0, t, 0, 0, t.getNumSamples());
        auto p = makeProcessor (sr, 2);
        process (*p, s);
        CHECK (std::abs (p->readings.outProgramme.load() - left) < 0.05f, "5.1 L should read like stereo L");
    }

    auto p = makeProcessor (sr, 6);
    const int lat = p->getLatencySamples();
    auto in = silence (6, (int) (sr * 3.0));
    for (int c = 0; c < 6; ++c)
    {
        const auto v = synthNarration (sr, 1, 3.0, -10.0f, 30 + c);
        in.copyFrom (c, 0, v, 0, 0, in.getNumSamples());
    }
    const auto out = process (*p, in);
    float err = 0;
    for (int c = 0; c < 6; ++c)
        for (int i = lat; i < in.getNumSamples(); ++i)
            err = std::max (err, std::abs (out.getSample (c, i) - in.getSample (c, i - lat)));
    CHECK (err < 1.0e-6f, "0 dB should be transparent on 5.1 (err %g)", err);

    auto hot = makeProcessor (sr, 6);
    setParam (*hot, VOMeterProcessor::matchId, 18.0f);
    const auto hotOut = process (*hot, in);
    const float tp = referenceTruePeakDb (hotOut, -6.0f);
    CHECK (tp <= -2.5f, "5.1 true peak %.3f dBTP above -2.5", tp);
}

void testLiveMeters()
{
    // Promise: the level meter shows each output channel's peak, the gain
    // reduction meter shows the limiter working (and nothing when it isn't),
    // and both run whether or not the transport plays.
    const double sr = 48000.0;
    for (int ch : { 1, 2, 6 })
    {
        auto p = makeProcessor (sr, ch);
        auto in = silence (ch, (int) sr);
        for (int c = 0; c < ch; ++c)
        {
            const auto t = sine (sr, 1, 1.0, 1000.0, -20.0f - 2.0f * c);
            in.copyFrom (c, 0, t, 0, 0, in.getNumSamples());
        }
        process (*p, in);
        auto& r = p->readings;
        CHECK (r.numChannels.load() == ch, "meter should know the %d channels", ch);
        for (int c = 0; c < ch; ++c)
        {
            const float db = juce::Decibels::gainToDecibels (r.takeOutPeak (c));
            CHECK (std::abs (db - (-20.0f - 2.0f * c)) < 0.1f, "channel %d of %d peaked %.2f dBFS", c + 1, ch, db);
        }
        CHECK (r.takeOutPeak (0) == 0.0f, "taking a peak should start the next one afresh");
        CHECK (r.takeGrPeak() < 0.01f, "no gain reduction expected on a -20 dBFS tone");

        setParam (*p, VOMeterProcessor::matchId, 24.0f);
        auto stopped = std::make_unique<TestPlayHead>();
        stopped->playing = false;
        p->setPlayHead (stopped.get());
        process (*p, in);
        CHECK (r.takeGrPeak() > 3.0f, "gain reduction should show while limiting, even when stopped");
        const float out = juce::Decibels::gainToDecibels (r.takeOutPeak (0));
        CHECK (out <= -2.5f && out > -4.0f, "limited output peaked %.2f dBFS", out);
        p->setPlayHead (nullptr);
    }
}

void testProgrammeRowOnlyOnMono()
{
    // Promise: the programme readout (and Use) shows only on a mono narration
    // track; a full mix (stereo / 5.1) never shows it, side chain or not.
   #if JUCE_LINUX
    if (juce::SystemStats::getEnvironmentVariable ("DISPLAY", {}).isEmpty())
        return;
   #endif
    const double sr = 48000.0;
    const auto programme = synthNarration (sr, 2, 10.0, -10.0f, 40);
    for (int ch : { 1, 2, 6 })
    {
        auto p = makeProcessor (sr, ch);
        process (*p, synthNarration (sr, ch, 10.0, -12.0f, 41), kBlock, &programme);
        std::unique_ptr<juce::AudioProcessorEditor> e (p->createEditor());
        bool useShown = false;
        for (auto* c : e->getChildren())
            if (auto* b = dynamic_cast<juce::TextButton*> (c); b != nullptr && b->getButtonText() == "Use")
                useShown = b->isVisible();
        CHECK (useShown == (ch == 1), "programme row on a %d-channel track should be %s", ch, ch == 1 ? "shown" : "hidden");
    }
}

void testBypass()
{
    // Promise: Bypass gives back the input bit for bit (after the reported
    // latency, even with the trim up), and it is the host's bypass too.
    auto p = makeProcessor (48000.0, 2);
    setParam (*p, VOMeterProcessor::matchId, 12.0f);
    setParam (*p, VOMeterProcessor::bypassId, 1.0f);
    p->prepareToPlay (48000.0, kBlock);
    const auto in = synthNarration (48000.0, 2, 2.0, -3.0f, 9);
    const auto out = process (*p, in);
    const int lat = p->getLatencySamples();
    bool exact = true;
    for (int c = 0; c < 2; ++c)
        for (int i = lat; i < in.getNumSamples(); ++i)
            exact = exact && out.getSample (c, i) == in.getSample (c, i - lat);
    CHECK (exact, "bypass should be bit-exact after latency");
    CHECK (p->getBypassParameter() == p->apvts.getParameter (VOMeterProcessor::bypassId),
           "host bypass should be the internal Bypass parameter");
}

void testStateRoundTrip()
{
    // Promise: trim, Match gain, target and bypass come back when a session is reopened.
    auto a = makeProcessor (48000.0, 2);
    setParam (*a, VOMeterProcessor::trimId, 7.5f);
    setParam (*a, VOMeterProcessor::targetId, -27.0f);
    setParam (*a, VOMeterProcessor::matchId, 6.3f);
    setParam (*a, VOMeterProcessor::mixId, 1.0f);
    setParam (*a, VOMeterProcessor::measureId, 1.0f);
    setParam (*a, VOMeterProcessor::bypassId, 1.0f);
    juce::MemoryBlock state;
    a->getStateInformation (state);

    auto b = makeProcessor (48000.0, 2);
    b->setStateInformation (state.getData(), (int) state.getSize());
    CHECK (std::abs (getParam (*b, VOMeterProcessor::trimId) - 7.5f) < 0.01f, "trim not restored");
    CHECK (std::abs (getParam (*b, VOMeterProcessor::targetId) + 27.0f) < 0.01f, "target not restored");
    CHECK (std::abs (getParam (*b, VOMeterProcessor::matchId) - 6.3f) < 0.01f, "Match gain not restored");
    CHECK (getParam (*b, VOMeterProcessor::mixId) > 0.5f, "Mix (5.1) not restored");
    CHECK (getParam (*b, VOMeterProcessor::measureId) > 0.5f, "Measure (integrated) not restored");
    CHECK (getParam (*b, VOMeterProcessor::bypassId) > 0.5f, "bypass not restored");
}

void testVersionCompare()
{
    CHECK (UpdateChecker::compareVersions ("v1.2.0", "1.1.9") > 0, "v1.2.0 > 1.1.9");
    CHECK (UpdateChecker::compareVersions ("1.0", "1.0.0") == 0, "1.0 == 1.0.0");
    CHECK (UpdateChecker::compareVersions ("0.9.10", "0.10.0") < 0, "0.9.10 < 0.10.0");
    CHECK (UpdateChecker::compareVersions ("1.0.0-beta", "1.0.0") == 0, "pre-release suffix ignored");
}

void testEditorAndAltClick()
{
    // Promise: the editor opens, and alt-clicking the trim knob returns it to 0 dB.
   #if JUCE_LINUX
    if (juce::SystemStats::getEnvironmentVariable ("DISPLAY", {}).isEmpty())
    {
        std::printf ("Skipping editor test: no X display.\n");
        return;
    }
   #endif
    auto p = makeProcessor (48000.0, 1);   // the trim is on mono (narration) tracks
    p->setMixFormat (false);
    setParam (*p, VOMeterProcessor::trimId, 9.0f);
    std::unique_ptr<juce::AudioProcessorEditor> editor (p->createEditor());
    CHECK (editor != nullptr && editor->getWidth() > 0, "editor failed to open");

    juce::Slider* knob = nullptr;
    for (auto* c : editor->getChildren())
        if (auto* s = dynamic_cast<juce::Slider*> (c))
            knob = s;
    CHECK (knob != nullptr, "no trim knob found");
    if (knob == nullptr) return;

    const auto centre = knob->getLocalBounds().getCentre().toFloat();
    const auto now = juce::Time::getCurrentTime();
    juce::MouseEvent e (juce::Desktop::getInstance().getMainMouseSource(), centre,
                        juce::ModifierKeys (juce::ModifierKeys::altModifier | juce::ModifierKeys::leftButtonModifier),
                        0.0f, 0.0f, 0.0f, 0.0f, 0.0f, knob, knob, now, centre, now, 1, false);
    knob->mouseDown (e);
    knob->mouseUp (e.withNewPosition (centre));
    CHECK (std::abs (getParam (*p, VOMeterProcessor::trimId)) < 0.01f, "alt-click left trim at %.2f dB", getParam (*p, VOMeterProcessor::trimId));
}

// --- --render ----------------------------------------------------------------
int renderFile (const juce::File& file, std::vector<float> targets)
{
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (file));
    if (reader == nullptr) { std::printf ("Can't read %s\n", file.getFullPathName().toRawUTF8()); return 1; }

    const int ch = (int) std::min (2u, reader->numChannels);
    const double sr = reader->sampleRate;
    Signal in (ch, (int) reader->lengthInSamples);
    reader->read (&in, 0, in.getNumSamples(), 0, true, ch > 1);
    std::printf ("%s: %.1f s, %d ch, %.0f Hz\n", file.getFileName().toRawUTF8(), in.getNumSamples() / sr, ch, sr);

    if (targets.empty()) targets = { -24.0f, -27.0f };
    for (float target : targets)
    {
        auto p = makeProcessor (sr, ch);
        setParam (*p, VOMeterProcessor::targetId, target);
        process (*p, in);
        auto& r = p->readings;
        if (ch == 1)
            std::printf ("  mono file, read as heard in a stereo mix (+3.01 dB)\n");
        std::printf ("  input : dialogue %.2f LUFS, programme %.2f LUFS (input meter), speech %.0f%%\n",
                     r.inDialogue.load(), p->getMeter().inputProgrammeLufs() + r.mixOffsetDb.load(), 100 * r.speechSeconds.load() / r.measuredSeconds.load());
        p->applySuggestedMatch();
        p->requestMeterReset();
        float maxGr = 0;
        Signal out (ch, in.getNumSamples());
        for (int start = 0; start < in.getNumSamples(); start += kBlock)   // block by block, to track the limiter
        {
            const int len = std::min (kBlock, in.getNumSamples() - start);
            Signal chunk (ch, len);
            for (int c = 0; c < ch; ++c) chunk.copyFrom (c, 0, in, c, start, len);
            const auto o = process (*p, chunk);
            for (int c = 0; c < ch; ++c) out.copyFrom (c, start, o, c, 0, len);
            maxGr = std::max (maxGr, r.limiterGr.load());
        }
        std::printf ("  target %.0f: match %+.1f dB -> dialogue %.2f LUFS, programme %.2f LUFS, max TP %.2f dBTP (ref %.2f), LRA %.1f LU, max GR %.1f dB\n",
                     target, getParam (*p, VOMeterProcessor::matchId), r.outDialogue.load(), r.outProgramme.load(),
                     r.maxTruePeak.load(), referenceTruePeakDb (out, -6.0f), r.lra.load(), maxGr);

        const auto dest = file.getSiblingFile (file.getFileNameWithoutExtension() + "_VOMeter" + juce::String ((int) target) + ".wav");
        dest.deleteFile();
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> os (dest.createOutputStream());
        const auto opts = juce::AudioFormatWriterOptions().withSampleRate (sr).withNumChannels (ch).withBitsPerSample (24);
        if (auto writer = wav.createWriterFor (os, opts))
        {
            const int lat = p->getLatencySamples();
            writer->writeFromAudioSampleBuffer (out, lat, out.getNumSamples() - lat);
            std::printf ("  wrote %s\n", dest.getFullPathName().toRawUTF8());
        }
    }
    return 0;
}
} // namespace

int main (int argc, char** argv)
{
    const juce::ScopedJuceInitialiser_GUI gui;

    if (argc >= 3 && juce::String (argv[1]) == "--render")
    {
        std::vector<float> targets;
        for (int i = 3; i < argc; ++i) targets.push_back (juce::String (argv[i]).getFloatValue());
        return renderFile (juce::File::getCurrentWorkingDirectory().getChildFile (argv[2]), targets);
    }

    testLatencyAndTransparency();
    testTrimGain();
    testTruePeakCeiling();
    testProgrammeMeterMatchesStandard();
    testSteadyToneIsNotDialogue();
    testDialogueMatchesProgrammeOnCleanVO();
    testNonSpeechBedIsIgnored();
    testMatchHitsTarget();
    testSuggestedMatchClamps();
    testTrimRidesOnTopOfMatch();
    testProgrammeSideChain();
    testResetClearsMeterNotTrim();
    testOnlyMeasuresWhilePlaying();
    testOfflineBounce();
    testMonoInStereoMix();
    testBypass();
    testStateRoundTrip();
    testVersionCompare();
    testEditorAndAltClick();
    testMixQuestion();
    testMeasureQuestion();
    testLiveMeters();
    testProgrammeRowOnlyOnMono();
    testIntegratedMode();
    testFiveOne();

    if (failures == 0)
        std::printf ("All VOMeter tests passed.\n");
    return failures == 0 ? 0 : 1;
}
