#include "PluginProcessor.h"
#include "PluginEditor.h"

VOMeterProcessor::VOMeterProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)
                          .withInput ("Programme", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "VOMeter", createLayout())
{
    trimParam = apvts.getRawParameterValue (trimId);
    matchParam = apvts.getRawParameterValue (matchId);
    mixParam = apvts.getRawParameterValue (mixId);
    targetParam = apvts.getRawParameterValue (targetId);
    bypassParam = dynamic_cast<juce::AudioParameterBool*> (apvts.getParameter (bypassId));
    jassert (trimParam != nullptr && matchParam != nullptr && mixParam != nullptr && targetParam != nullptr && bypassParam != nullptr);
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

    // Added in 0.2.0. A fixed gain under the trim, set by Match, so the trim is
    // free for riding: trim 0 dB = on target.
    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { matchId, 1 }, "Match Gain",
        juce::NormalisableRange<float> (-vometer::kMatchRangeDb, vometer::kMatchRangeDb, 0.1f), 0.0f,
        juce::AudioParameterFloatAttributes()
            .withLabel ("dB")
            .withAutomatable (false)
            .withStringFromValueFunction (dbText)
            .withValueFromStringFunction ([] (const juce::String& s) { return s.retainCharacters ("+-.0123456789").getFloatValue(); })));

    // Added in 0.2.0. On a mono track: how the narration will be heard. In a
    // stereo mix (centre-panned, both channels at unity) it measures 3 dB louder
    // than on its own; in 5.1 it sits in the centre channel alone. Readings are
    // given as heard in that mix, so the target is the programme's own target.
    layout.add (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID { mixId, 1 }, "Mix", juce::StringArray { "Stereo", "5.1" }, 0,
        juce::AudioParameterChoiceAttributes().withAutomatable (false)));

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
    if (out != layouts.getMainInputChannelSet())
        return false;
    if (layouts.inputBuses.size() > 1)
    {
        const auto sc = layouts.getChannelSet (true, 1);
        if (! sc.isDisabled() && sc != juce::AudioChannelSet::mono() && sc != juce::AudioChannelSet::stereo())
            return false;
    }
    return true;
}

void VOMeterProcessor::prepareToPlay (double sampleRate, int)
{
    const int numCh = std::max (1, std::min (2, getMainBusNumOutputChannels()));
    limiter.prepare (sampleRate, numCh);
    meter.prepare (sampleRate, numCh);
    programme.prepare (sampleRate, 2);
    sampleRateHz = sampleRate;
    latency = limiter.getLatency();
    setLatencySamples (latency);

    dryDelay.assign ((size_t) numCh, std::vector<float> ((size_t) latency + 1, 0.0f));
    dryPos = 0;

    trimGain.reset (sampleRate, 0.02);  // 20 ms ramps: click-free trim moves
    trimGain.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (matchParam->load() + trimParam->load()));
    wetMix.reset (sampleRate, 0.02);
    wetMix.setCurrentAndTargetValue (bypassParam->get() ? 0.0f : 1.0f);
    wasMeasuring = false;
}

bool VOMeterProcessor::hostIsPlaying()
{
    // Offline bounces always measure, whatever the transport reports.
    if (isNonRealtime())
        return true;
    // Hosts without a play head (and the test tool) always measure.
    if (auto* ph = getPlayHead())
        if (auto pos = ph->getPosition())
            return pos->getIsPlaying();
    return true;
}

void VOMeterProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    for (int ch = getMainBusNumInputChannels(); ch < getTotalNumOutputChannels(); ++ch)
        buffer.clear (ch, 0, buffer.getNumSamples());

    if (resetRequested.exchange (false))
    {
        meter.reset();
        meter.restartBlocks();
        programme.reset();
        programme.restartBlocks();
        trimMinSinceReset = matchMinSinceReset = 1.0e9f;
        trimMaxSinceReset = matchMaxSinceReset = -1.0e9f;
    }

    const bool measuring = hostIsPlaying();
    if (measuring && ! wasMeasuring)
    {
        meter.restartBlocks();   // a new pass: don't join audio from before the stop
        programme.restartBlocks();
    }
    wasMeasuring = measuring;

    const float trimDb = trimParam->load(), matchDb = matchParam->load();
    trimGain.setTargetValue (juce::Decibels::decibelsToGain (matchDb + trimDb));
    if (measuring)
    {
        trimMinSinceReset = std::min (trimMinSinceReset, trimDb);
        trimMaxSinceReset = std::max (trimMaxSinceReset, trimDb);
        matchMinSinceReset = std::min (matchMinSinceReset, matchDb);
        matchMaxSinceReset = std::max (matchMaxSinceReset, matchDb);
    }
    wetMix.setTargetValue (bypassParam->get() ? 0.0f : 1.0f);

    auto mainBus = getBusBuffer (buffer, true, 0);
    const int numCh = std::min (mainBus.getNumChannels(), (int) dryDelay.size());
    const int n = buffer.getNumSamples();

    // Programme audio on the sidechain (Logic sends silence when none is chosen).
    const float* scPtr[2] { nullptr, nullptr };
    int numSc = 0;
    if (getBusCount (true) > 1)
        if (auto* bus = getBus (true, 1); bus != nullptr && bus->isEnabled())
        {
            auto sc = getBusBuffer (buffer, true, 1);
            numSc = std::min (2, sc.getNumChannels());
            for (int c = 0; c < numSc; ++c)
                scPtr[c] = sc.getReadPointer (c);
        }
    // Logic feeds the track's own audio into the side chain when its Side
    // Chain menu says "None". A side chain that is a copy of our input isn't
    // programme audio: ignore it.
    if (numSc > 0)
    {
        bool copyOfInput = numCh > 0;
        for (int c = 0; c < numSc && copyOfInput; ++c)
        {
            const float* a = scPtr[c];
            const float* b = mainBus.getReadPointer (std::min (c, numCh - 1));
            for (int i = 0; i < n; ++i)
                if (! juce::exactlyEqual (a[i], b[i])) { copyOfInput = false; break; }
        }
        if (copyOfInput)
            numSc = 0;
    }

    if (numSc > 0)
    {
        float peak = 0.0f;
        for (int c = 0; c < numSc; ++c)
        {
            const auto r = juce::FloatVectorOperations::findMinAndMax (scPtr[c], n);
            peak = std::max ({ peak, -r.getStart(), r.getEnd() });
        }
        samplesSinceProgramme = peak > 1.0e-5f ? 0 : samplesSinceProgramme + n;
    }
    else
        samplesSinceProgramme += n;
    const int delayLen = latency + 1;
    float maxGr = 0.0f;

    for (int i = 0; i < n; ++i)
    {
        float in[2] {}, wet[2] {}, out[2] {};
        const float g = trimGain.getNextValue();
        const float mix = wetMix.getNextValue();

        for (int ch = 0; ch < numCh; ++ch)
        {
            const float x = mainBus.getSample (ch, i);
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
            mainBus.setSample (ch, i, out[ch]);
        }

        meter.pushFrame (in, out, numCh, measuring);

        if (numSc > 0)
        {
            const float sc[2] { scPtr[0][i], numSc > 1 ? scPtr[1][i] : 0.0f };
            programme.push (sc, numSc, measuring);
        }
    }

    // A mono track in a stereo mix plays in both channels: +3.01 dB by BS.1770.
    const float offset = (numCh == 1 && mixParam->load() < 0.5f) ? 3.0103f : 0.0f;
    readings.mixOffsetDb.store (offset);
    readings.outDialogue.store ((float) meter.outputDialogueLufs() + offset);
    readings.inDialogue.store ((float) meter.inputDialogueLufs() + offset);
    readings.outProgramme.store ((float) meter.outputProgrammeLufs() + offset);
    readings.lra.store ((float) meter.loudnessRange());
    readings.maxTruePeak.store (meter.maxTruePeakDb());
    readings.limiterGr.store (maxGr);
    readings.speechSeconds.store ((float) meter.speechSeconds());
    readings.measuredSeconds.store ((float) meter.measuredSeconds());
    readings.measuring.store (measuring);
    readings.trimMovedSinceReset.store (trimMaxSinceReset - trimMinSinceReset > 0.05f);
    readings.matchMovedSinceReset.store (matchMaxSinceReset - matchMinSinceReset > 0.05f);
    readings.programmeDialogue.store ((float) programme.dialogueLufs());
    readings.programmeSignal.store (samplesSinceProgramme < (long long) (2.0 * sampleRateHz));
}

float VOMeterProcessor::getSuggestedMatchDb() const noexcept
{
    const float in = readings.inDialogue.load();
    if (std::isnan (in))
        return std::numeric_limits<float>::quiet_NaN();
    return juce::jlimit (-vometer::kMatchRangeDb, vometer::kMatchRangeDb, targetParam->load() - in);
}

static void setAsGesture (juce::RangedAudioParameter* p, float plain)
{
    p->beginChangeGesture();
    p->setValueNotifyingHost (p->convertTo0to1 (plain));
    p->endChangeGesture();
}

bool VOMeterProcessor::applySuggestedMatch()
{
    const float g = getSuggestedMatchDb();
    if (std::isnan (g))
        return false;
    setAsGesture (apvts.getParameter (matchId), g);
    return true;
}

void VOMeterProcessor::clearMatch()
{
    setAsGesture (apvts.getParameter (matchId), 0.0f);
}

static const juce::Identifier mixChosenProp { "mixChosen" };

bool VOMeterProcessor::isMixFormatChosen() const
{
    return apvts.state.getProperty (mixChosenProp, false);
}

void VOMeterProcessor::setMixFormat (bool fiveOne)
{
    setAsGesture (apvts.getParameter (mixId), fiveOne ? 1.0f : 0.0f);
    apvts.state.setProperty (mixChosenProp, true, nullptr);
}

bool VOMeterProcessor::useProgrammeAsTarget()
{
    const float p = readings.programmeDialogue.load();
    if (std::isnan (p))
        return false;
    setAsGesture (apvts.getParameter (targetId), juce::jlimit (-36.0f, -10.0f, std::round (p * 10.0f) / 10.0f));
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
