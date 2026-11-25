#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "UpdateChecker.h"
#include "VOMeterDSP.h"

// VOMeter: Trim -> -2.5 dBTP true-peak limiter, with dialogue-gated loudness
// metering of the input (before trim, for the suggested trim) and the output.
// The meter only accumulates while the host's transport is playing.
class VOMeterProcessor : public juce::AudioProcessor
{
public:
    VOMeterProcessor();
    ~VOMeterProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorParameter* getBypassParameter() const override { return bypassParam; }

    juce::AudioProcessorValueTreeState apvts;
    UpdateChecker updateChecker;

    // Parameter IDs are part of saved sessions and automation: never rename
    // one after a release (add a new ID instead).
    static constexpr const char* trimId   = "trim";
    static constexpr const char* targetId = "target";
    static constexpr const char* bypassId = "bypass";

    // ---- Meter readings, written by the audio thread, read by the editor ----
    // NaN = no reading yet.
    struct Readings
    {
        std::atomic<float> outDialogue { std::numeric_limits<float>::quiet_NaN() };
        std::atomic<float> inDialogue  { std::numeric_limits<float>::quiet_NaN() };
        std::atomic<float> outProgramme { std::numeric_limits<float>::quiet_NaN() };
        std::atomic<float> lra { std::numeric_limits<float>::quiet_NaN() };
        std::atomic<float> maxTruePeak { -std::numeric_limits<float>::infinity() };
        std::atomic<float> limiterGr { 0.0f };       // max over the last block, dB
        std::atomic<float> speechSeconds { 0.0f };
        std::atomic<float> measuredSeconds { 0.0f };
        std::atomic<bool> measuring { false };
        std::atomic<bool> trimMovedSinceReset { false }; // the output reading mixes trim settings
    };
    Readings readings;

    // Clears the meter (not the trim). Safe from any thread; the audio thread
    // applies it at the start of its next block.
    void requestMeterReset() noexcept { resetRequested.store (true); }
    bool meterResetPending() const noexcept { return resetRequested.load(); }

    // The trim that would bring the dialogue loudness of the input to the
    // target (clamped to the trim range), or NaN before there's a reading.
    float getSuggestedTrimDb() const noexcept;
    // Sets the Trim parameter to the suggested value as one undoable gesture.
    bool applySuggestedTrim();

    // Direct access for the test tool (audio thread only).
    const vometer::LoudnessMeter& getMeter() const noexcept { return meter; }

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    bool hostIsPlaying();

    std::atomic<float>* trimParam = nullptr;
    std::atomic<float>* targetParam = nullptr;
    juce::AudioParameterBool* bypassParam = nullptr;

    juce::SmoothedValue<float> trimGain { 1.0f };
    juce::SmoothedValue<float> wetMix { 1.0f };   // 1 = processed, 0 = bypassed (dry, delayed)

    vometer::TruePeakLimiter limiter;
    vometer::LoudnessMeter meter;
    std::vector<std::vector<float>> dryDelay;     // latency compensation for the meter input and bypass
    int dryPos = 0, latency = 0;
    bool wasMeasuring = false;
    float trimMinSinceReset = 1.0e9f, trimMaxSinceReset = -1.0e9f;
    std::atomic<bool> resetRequested { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VOMeterProcessor)
};
