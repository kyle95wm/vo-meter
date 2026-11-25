// Headless tests for VOMeter (run by ctest on every platform in CI).
// Drives the plugin's processor directly, no plugin host needed.
// Add a test function per behaviour you care about and call it from main().

#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginProcessor.h"
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

constexpr double kSampleRate = 48000.0;
constexpr int kBlock = 512;

void setParam (VOMeterProcessor& p, const char* id, float plainValue)
{
    auto* param = p.apvts.getParameter (id);
    param->setValueNotifyingHost (param->convertTo0to1 (plainValue));
}

// Runs `seconds` of a 1 kHz sine through the processor and returns the output.
juce::AudioBuffer<float> render (VOMeterProcessor& p, double seconds, float amplitude = 0.25f)
{
    const int n = (int) (seconds * kSampleRate);
    juce::AudioBuffer<float> out (2, n);
    for (int i = 0; i < n; ++i)
        for (int ch = 0; ch < 2; ++ch)
            out.setSample (ch, i, amplitude * (float) std::sin (juce::MathConstants<double>::twoPi * 1000.0 * i / kSampleRate));

    juce::MidiBuffer midi;
    for (int start = 0; start < n; start += kBlock)
    {
        const int len = std::min (kBlock, n - start);
        juce::AudioBuffer<float> view (out.getArrayOfWritePointers(), 2, start, len);
        p.processBlock (view, midi);
    }
    return out;
}

std::unique_ptr<VOMeterProcessor> makeProcessor()
{
    auto p = std::make_unique<VOMeterProcessor>();
    p->setPlayConfigDetails (2, 2, kSampleRate, kBlock);
    p->prepareToPlay (kSampleRate, kBlock);
    return p;
}

void testUnityIsTransparent()
{
    auto p = makeProcessor();
    const auto out = render (*p, 0.5);
    CHECK (std::abs (out.getMagnitude (0, out.getNumSamples()) - 0.25f) < 1.0e-6f,
           "0 dB gain should leave the signal untouched (peak %f)", out.getMagnitude (0, out.getNumSamples()));
}

void testGainAndBypass()
{
    auto p = makeProcessor();
    setParam (*p, VOMeterProcessor::gainId, -6.0f);
    auto out = render (*p, 0.5);
    const float tail = out.getMagnitude (0, out.getNumSamples() / 2, out.getNumSamples() / 2);
    CHECK (std::abs (juce::Decibels::gainToDecibels (tail / 0.25f) + 6.0f) < 0.05f, "-6 dB gain gave %f", tail);

    setParam (*p, VOMeterProcessor::bypassId, 1.0f);
    out = render (*p, 0.5);
    const float bypassed = out.getMagnitude (0, out.getNumSamples() / 2, out.getNumSamples() / 2);
    CHECK (std::abs (bypassed - 0.25f) < 1.0e-5f, "bypass should restore unity (peak %f)", bypassed);
    CHECK (p->getBypassParameter() == p->apvts.getParameter (VOMeterProcessor::bypassId),
           "host bypass should be the internal Bypass parameter");
}

void testStateRoundTrip()
{
    auto a = makeProcessor();
    setParam (*a, VOMeterProcessor::gainId, 7.5f);
    setParam (*a, VOMeterProcessor::bypassId, 1.0f);
    juce::MemoryBlock state;
    a->getStateInformation (state);

    auto b = makeProcessor();
    b->setStateInformation (state.getData(), (int) state.getSize());
    const float gain = b->apvts.getRawParameterValue (VOMeterProcessor::gainId)->load();
    CHECK (std::abs (gain - 7.5f) < 0.01f, "gain not restored (%f)", gain);
    CHECK (b->apvts.getRawParameterValue (VOMeterProcessor::bypassId)->load() > 0.5f, "bypass not restored");
}

void testVersionCompare()
{
    CHECK (UpdateChecker::compareVersions ("v1.2.0", "1.1.9") > 0, "v1.2.0 > 1.1.9");
    CHECK (UpdateChecker::compareVersions ("1.0", "1.0.0") == 0, "1.0 == 1.0.0");
    CHECK (UpdateChecker::compareVersions ("0.9.10", "0.10.0") < 0, "0.9.10 < 0.10.0");
    CHECK (UpdateChecker::compareVersions ("1.0.0-beta", "1.0.0") == 0, "pre-release suffix ignored");
}

void testEditorOpens()
{
    // Constructs the editor (and the shared footer) without showing it.
   #if JUCE_LINUX
    if (juce::SystemStats::getEnvironmentVariable ("DISPLAY", {}).isEmpty())
    {
        std::printf ("Skipping editor test: no X display.\n");
        return;
    }
   #endif
    auto p = makeProcessor();
    std::unique_ptr<juce::AudioProcessorEditor> editor (p->createEditor());
    CHECK (editor != nullptr && editor->getWidth() > 0, "editor failed to open");
}
} // namespace

int main()
{
    const juce::ScopedJuceInitialiser_GUI gui;

    testUnityIsTransparent();
    testGainAndBypass();
    testStateRoundTrip();
    testVersionCompare();
    testEditorOpens();

    if (failures == 0)
        std::printf ("All VOMeter tests passed.\n");
    return failures == 0 ? 0 : 1;
}
