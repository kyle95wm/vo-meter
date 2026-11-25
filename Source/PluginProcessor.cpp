#include "PluginProcessor.h"
#include "PluginEditor.h"

VOMeterProcessor::VOMeterProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "VOMeter", createLayout())
{
    gainParam = apvts.getRawParameterValue (gainId);
    bypassParam = dynamic_cast<juce::AudioParameterBool*> (apvts.getParameter (bypassId));
    jassert (gainParam != nullptr && bypassParam != nullptr);
}

juce::AudioProcessorValueTreeState::ParameterLayout VOMeterProcessor::createLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { gainId, 1 }, "Gain",
        juce::NormalisableRange<float> (-24.0f, 24.0f, 0.1f), 0.0f,
        juce::AudioParameterFloatAttributes()
            .withLabel ("dB")
            .withStringFromValueFunction ([] (float v, int) { return juce::String (v, 1) + " dB"; })));

    layout.add (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { bypassId, 1 }, "Bypass", false));

    return layout;
}

bool VOMeterProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;
    return out == layouts.getMainInputChannelSet();
}

void VOMeterProcessor::prepareToPlay (double sampleRate, int)
{
    gain.reset (sampleRate, 0.02);  // 20 ms ramps: click-free knob moves and bypass
    gain.setCurrentAndTargetValue (bypassParam->get() ? 1.0f : juce::Decibels::decibelsToGain (gainParam->load()));
}

void VOMeterProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    for (int ch = getTotalNumInputChannels(); ch < getTotalNumOutputChannels(); ++ch)
        buffer.clear (ch, 0, buffer.getNumSamples());

    gain.setTargetValue (bypassParam->get() ? 1.0f : juce::Decibels::decibelsToGain (gainParam->load()));

    if (! gain.isSmoothing())
    {
        if (gain.getTargetValue() != 1.0f)
            buffer.applyGain (gain.getTargetValue());
        return;
    }

    for (int i = 0; i < buffer.getNumSamples(); ++i)
    {
        const float g = gain.getNextValue();
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            buffer.setSample (ch, i, buffer.getSample (ch, i) * g);
    }
}

void VOMeterProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, destData);
}

void VOMeterProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessorEditor* VOMeterProcessor::createEditor()
{
    return new VOMeterEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new VOMeterProcessor();
}
