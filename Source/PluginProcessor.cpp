#include "PluginProcessor.h"
#include "PluginEditor.h"

VOMeterProcessor::VOMeterProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "VOMeter", createLayout())
{
    trimParam = apvts.getRawParameterValue (trimId);
    targetParam = apvts.getRawParameterValue (targetId);
    bypassParam = dynamic_cast<juce::AudioParameterBool*> (apvts.getParameter (bypassId));
    jassert (trimParam != nullptr && targetParam != nullptr && bypassParam != nullptr);
}

juce::AudioProcessorValueTreeState::ParameterLayout VOMeterProcessor::createLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    auto dbText = [] (float v, int)
    {
        if (std::abs (v) < 0.05f) v = 0.0f;  // never show "-0.0"
        return (v > 0.0f ? "+" : "") + juce::String (v, 1) + " dB";
    };

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { trimId, 1 }, "Trim",
        juce::NormalisableRange<float> (-vometer::kTrimRangeDb, vometer::kTrimRangeDb, 0.1f), 0.0f,
        juce::AudioParameterFloatAttributes()
            .withLabel ("dB")
            .withStringFromValueFunction (dbText)
            .withValueFromStringFunction ([] (const juce::String& s) { return s.retainCharacters ("+-.0123456789").getFloatValue(); })));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { targetId, 1 }, "Target",
        juce::NormalisableRange<float> (-36.0f, -10.0f, 0.1f), -24.0f,
        juce::AudioParameterFloatAttributes()
            .withLabel ("LUFS")
            .withAutomatable (false)
            .withStringFromValueFunction ([] (float v, int) { return juce::String (v, 1) + " LUFS"; })
            .withValueFromStringFunction ([] (const juce::String& s) { return s.retainCharacters ("+-.0123456789").getFloatValue(); })));

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
    const int numCh = std::max (1, std::min (2, getTotalNumOutputChannels()));
    limiter.prepare (sampleRate, numCh);
    meter.prepare (sampleRate, numCh);
    latency = limiter.getLatency();
    setLatencySamples (latency);

    dryDelay.assign ((size_t) numCh, std::vector<float> ((size_t) latency + 1, 0.0f));
    dryPos = 0;

    trimGain.reset (sampleRate, 0.02);  // 20 ms ramps: click-free trim moves
    trimGain.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (trimParam->load()));
    wetMix.reset (sampleRate, 0.02);
    wetMix.setCurrentAndTargetValue (bypassParam->get() ? 0.0f : 1.0f);
    wasMeasuring = false;
}

bool VOMeterProcessor::hostIsPlaying()
{
    // Hosts without a play head (and the test tool) always measure.
    if (auto* ph = getPlayHead())
        if (auto pos = ph->getPosition())
            return pos->getIsPlaying();
    return true;
}

void VOMeterProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    for (int ch = getTotalNumInputChannels(); ch < getTotalNumOutputChannels(); ++ch)
        buffer.clear (ch, 0, buffer.getNumSamples());

    if (resetRequested.exchange (false))
    {
        meter.reset();
        meter.restartBlocks();
        trimMinSinceReset = 1.0e9f; trimMaxSinceReset = -1.0e9f;
    }

    const bool measuring = hostIsPlaying();
    if (measuring && ! wasMeasuring)
        meter.restartBlocks();   // a new pass: don't join audio from before the stop
    wasMeasuring = measuring;

    const float trimDb = trimParam->load();
    trimGain.setTargetValue (juce::Decibels::decibelsToGain (trimDb));
    if (measuring)
    {
        trimMinSinceReset = std::min (trimMinSinceReset, trimDb);
        trimMaxSinceReset = std::max (trimMaxSinceReset, trimDb);
    }
    wetMix.setTargetValue (bypassParam->get() ? 0.0f : 1.0f);

    const int numCh = std::min (buffer.getNumChannels(), (int) dryDelay.size());
    const int n = buffer.getNumSamples();
    const int delayLen = latency + 1;
    float maxGr = 0.0f;

    for (int i = 0; i < n; ++i)
    {
        float in[2] {}, wet[2] {}, out[2] {};
        const float g = trimGain.getNextValue();
        const float mix = wetMix.getNextValue();

        for (int ch = 0; ch < numCh; ++ch)
        {
            const float x = buffer.getSample (ch, i);
            auto& d = dryDelay[(size_t) ch];
            d[(size_t) dryPos] = x;
            in[ch] = d[(size_t) ((dryPos + 1) % delayLen)];   // input delayed by `latency`
            wet[ch] = x * g;
        }
        dryPos = (dryPos + 1) % delayLen;

        limiter.processFrame (wet, numCh);
        maxGr = std::max (maxGr, limiter.getReductionDb());

        for (int ch = 0; ch < numCh; ++ch)
        {
            out[ch] = mix >= 1.0f ? wet[ch] : (mix <= 0.0f ? in[ch] : in[ch] + mix * (wet[ch] - in[ch]));
            buffer.setSample (ch, i, out[ch]);
        }

        meter.pushFrame (in, out, numCh, measuring);
    }

    readings.outDialogue.store ((float) meter.outputDialogueLufs());
    readings.inDialogue.store ((float) meter.inputDialogueLufs());
    readings.outProgramme.store ((float) meter.outputProgrammeLufs());
    readings.lra.store ((float) meter.loudnessRange());
    readings.maxTruePeak.store (meter.maxTruePeakDb());
    readings.limiterGr.store (maxGr);
    readings.speechSeconds.store ((float) meter.speechSeconds());
    readings.measuredSeconds.store ((float) meter.measuredSeconds());
    readings.measuring.store (measuring);
    readings.trimMovedSinceReset.store (trimMaxSinceReset - trimMinSinceReset > 0.05f);
}

float VOMeterProcessor::getSuggestedTrimDb() const noexcept
{
    const float in = readings.inDialogue.load();
    if (std::isnan (in))
        return std::numeric_limits<float>::quiet_NaN();
    return juce::jlimit (-vometer::kTrimRangeDb, vometer::kTrimRangeDb, targetParam->load() - in);
}

bool VOMeterProcessor::applySuggestedTrim()
{
    const float trim = getSuggestedTrimDb();
    if (std::isnan (trim))
        return false;
    auto* p = apvts.getParameter (trimId);
    p->beginChangeGesture();
    p->setValueNotifyingHost (p->convertTo0to1 (trim));
    p->endChangeGesture();
    return true;
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
