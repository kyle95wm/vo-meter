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

std::unique_ptr<VOMeterProcessor> makeProcessor (double sr, int channels)
{
    auto p = std::make_unique<VOMeterProcessor>();
    const auto set = channels == 1 ? juce::AudioChannelSet::mono() : juce::AudioChannelSet::stereo();
    juce::AudioProcessor::BusesLayout layout;
    layout.inputBuses.add (set);
    layout.outputBuses.add (set);
    p->setBusesLayout (layout);
    p->setPlayConfigDetails (channels, channels, sr, kBlock);
    p->prepareToPlay (sr, kBlock);
    return p;
}

// Runs a copy of `in` through the processor and returns the output.
Signal process (VOMeterProcessor& p, const Signal& in, int block = kBlock)
{
    Signal out (in);
    juce::MidiBuffer midi;
    for (int start = 0; start < out.getNumSamples(); start += block)
    {
        const int len = std::min (block, out.getNumSamples() - start);
        juce::AudioBuffer<float> view (out.getArrayOfWritePointers(), out.getNumChannels(), start, len);
        p.processBlock (view, midi);
    }
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
    // Promise: the trim is a plain gain change, in dB.
    auto p = makeProcessor (48000.0, 2);
    setParam (*p, VOMeterProcessor::trimId, -6.0f);
    const auto in = sine (48000.0, 2, 1.0, 1000.0, -20.0f);
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
                    setParam (*p, VOMeterProcessor::trimId, trimDb);
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
    // Promise: play the take, press Match, reset, play again: the dialogue
    // loudness of the output lands on the target (within 0.5 LU), for quiet and
    // hot takes, at every sample rate.
    for (double sr : kRates)
        for (float target : { -24.0f, -27.0f, -23.0f })
            for (float peak : { -12.0f, -6.0f })
            {
                auto p = makeProcessor (sr, 2);
                setParam (*p, VOMeterProcessor::targetId, target);
                const auto take = synthNarration (sr, 2, 30.0, peak, 5);
                process (*p, take);
                CHECK (p->applySuggestedTrim(), "Match should work once dialogue was heard");
                p->requestMeterReset();
                process (*p, take);
                const float out = p->readings.outDialogue.load();
                CHECK (std::abs (out - target) < 0.5f, "after Match: %.2f LUFS for target %.0f (peak %.0f dBFS, %.0f Hz, trim %+.1f)",
                       out, target, peak, sr, getParam (*p, VOMeterProcessor::trimId));
                CHECK (! p->readings.trimMovedSinceReset.load(), "trim didn't move after the reset");
            }
}

void testSuggestedTrimClamps()
{
    // Promise: a very quiet take suggests at most +18 dB (the trim's range).
    auto p = makeProcessor (48000.0, 1);
    setParam (*p, VOMeterProcessor::targetId, -16.0f);
    process (*p, synthNarration (48000.0, 1, 15.0, -40.0f, 6));
    const float s = p->getSuggestedTrimDb();
    CHECK (std::abs (s - 18.0f) < 0.01f, "suggested trim %.2f should clamp to +18", s);
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

void testBypass()
{
    // Promise: Bypass gives back the input bit for bit (after the reported
    // latency, even with the trim up), and it is the host's bypass too.
    auto p = makeProcessor (48000.0, 2);
    setParam (*p, VOMeterProcessor::trimId, 12.0f);
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
    // Promise: trim, target and bypass come back when a session is reopened.
    auto a = makeProcessor (48000.0, 2);
    setParam (*a, VOMeterProcessor::trimId, 7.5f);
    setParam (*a, VOMeterProcessor::targetId, -27.0f);
    setParam (*a, VOMeterProcessor::bypassId, 1.0f);
    juce::MemoryBlock state;
    a->getStateInformation (state);

    auto b = makeProcessor (48000.0, 2);
    b->setStateInformation (state.getData(), (int) state.getSize());
    CHECK (std::abs (getParam (*b, VOMeterProcessor::trimId) - 7.5f) < 0.01f, "trim not restored");
    CHECK (std::abs (getParam (*b, VOMeterProcessor::targetId) + 27.0f) < 0.01f, "target not restored");
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
    auto p = makeProcessor (48000.0, 2);
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
        std::printf ("  input : dialogue %.2f LUFS, programme %.2f LUFS (input meter), speech %.0f%%\n",
                     r.inDialogue.load(), p->getMeter().inputProgrammeLufs(), 100 * r.speechSeconds.load() / r.measuredSeconds.load());
        p->applySuggestedTrim();
        p->requestMeterReset();
        float maxGr = 0;
        Signal out (in);
        juce::MidiBuffer midi;
        for (int start = 0; start < out.getNumSamples(); start += kBlock)
        {
            const int len = std::min (kBlock, out.getNumSamples() - start);
            juce::AudioBuffer<float> view (out.getArrayOfWritePointers(), ch, start, len);
            p->processBlock (view, midi);
            maxGr = std::max (maxGr, r.limiterGr.load());
        }
        std::printf ("  target %.0f: trim %+.1f dB -> dialogue %.2f LUFS, programme %.2f LUFS, max TP %.2f dBTP (ref %.2f), LRA %.1f LU, max GR %.1f dB\n",
                     target, getParam (*p, VOMeterProcessor::trimId), r.outDialogue.load(), r.outProgramme.load(),
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
    testSuggestedTrimClamps();
    testResetClearsMeterNotTrim();
    testOnlyMeasuresWhilePlaying();
    testOfflineBounce();
    testBypass();
    testStateRoundTrip();
    testVersionCompare();
    testEditorAndAltClick();

    if (failures == 0)
        std::printf ("All VOMeter tests passed.\n");
    return failures == 0 ? 0 : 1;
}
