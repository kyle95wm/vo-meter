#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "UpdateChecker.h"

// Skeleton processor: one "Gain" knob and an internal, automatable Bypass that
// is also exposed as the host's bypass parameter. Replace the DSP; keep the
// bypass parameter, the state handling and the shared UpdateChecker member.
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
    static constexpr const char* gainId   = "gain";
    static constexpr const char* bypassId = "bypass";

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

    std::atomic<float>* gainParam = nullptr;
    juce::AudioParameterBool* bypassParam = nullptr;
    juce::SmoothedValue<float> gain { 1.0f };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VOMeterProcessor)
};
