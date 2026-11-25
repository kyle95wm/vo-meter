#include "PluginEditor.h"

namespace
{
juce::String utf8 (const char* s) { return juce::String (juce::CharPointer_UTF8 (s)); }
const char* kDash = "\xe2\x80\x94";   // em dash: "no reading yet"
constexpr int kTitleRowHeight = 28;     // brand::drawHeader's title row (no subtitle row here)
constexpr int kMeterStripWidth = 120;   // output level + gain reduction meters, right side
constexpr int kFrameRate = 30;          // editor refresh, Hz
constexpr float kMeterFloorDb = -60.0f, kGrRangeDb = 12.0f;
constexpr float kFallDbPerSec = 20.0f, kHoldSeconds = 1.5f;

juce::String fmt (float v, int decimals = 1)
{
    if (std::abs (v) < 0.5f * std::pow (10.0f, (float) -decimals)) v = 0.0f;  // never "-0.0"
    return juce::String (v, decimals);
}

juce::String signedDb (float v)
{
    if (std::abs (v) < 0.05f) v = 0.0f;
    return (v > 0.0f ? "+" : "") + juce::String (v, 1) + " dB";
}

void drawCaption (juce::Graphics& g, const juce::String& text, juce::Rectangle<int> r,
                  juce::Justification j = juce::Justification::centredLeft)
{
    g.setColour (brand::textDim);
    g.setFont (juce::FontOptions (brand::captionSize, juce::Font::bold));
    g.drawText (text, r, j);
}

void styleButton (juce::TextButton& b)
{
    b.setColour (juce::TextButton::buttonColourId, brand::track);
    b.setColour (juce::TextButton::textColourOffId, brand::textMain);
}
} // namespace

VOMeterEditor::VOMeterEditor (VOMeterProcessor& p)
    : AudioProcessorEditor (&p), audioProcessor (p),
      mixQuestion ("mixQuestion", "Is this mix stereo or 5.1?",
                   "A mono narration measures 3 dB louder in a stereo mix than in a 5.1 centre channel. "
                   "VOMeter reads it the way your mix will, so you can use the programme's own target. "
                   "You can change this later in the Presets menu.",
                   "Stereo mix", "5.1 mix"),
      measureQuestion ("measureQuestion", "What should VOMeter measure?",
                       "Pick what your delivery spec measures. Dialogue-gated: only speech counts "
                       "(Netflix and other dialogue-based specs). Integrated: the whole mix, BS.1770 "
                       "(EBU R128, ATSC A/85). You can change this later in the Presets menu.",
                       "Dialogue-gated", "Integrated"),
      footer (p.updateChecker),
      targetAttachment (*p.apvts.getParameter (VOMeterProcessor::targetId),
                        [this] (float v) { targetLabel.setText (juce::String (v, 1), juce::dontSendNotification); repaint(); }),
      trimAttachment (p.apvts, VOMeterProcessor::trimId, trimKnob)
{
    setLookAndFeel (&lnf);

    // Target: click to type a value, or pick a preset.
    addAndMakeVisible (targetLabel);
    targetLabel.setEditable (true, true, false);
    targetLabel.setJustificationType (juce::Justification::centredLeft);
    targetLabel.setFont (juce::FontOptions (30.0f, juce::Font::bold));
    targetLabel.setColour (juce::Label::backgroundWhenEditingColourId, brand::bg);
    targetLabel.setColour (juce::Label::textWhenEditingColourId, brand::textMain);
    targetLabel.setColour (juce::Label::outlineWhenEditingColourId, brand::accent);
    targetLabel.setColour (juce::TextEditor::highlightColourId, brand::accent.withAlpha (0.4f));
    targetLabel.setTooltip ("Target loudness. Click and type a value (-36 to -10 LUFS).");
    targetLabel.onTextChange = [this] { setTargetFromText (targetLabel.getText()); };
    targetAttachment.sendInitialUpdate();

    addAndMakeVisible (presetButton);
    styleButton (presetButton);
    presetButton.setTooltip ("Common delivery targets");
    presetButton.onClick = [this] { showPresetMenu(); };

    // Trim knob. Alt-click (and double-click) returns it to 0 dB.
    addAndMakeVisible (trimKnob);
    trimKnob.getProperties().set ("bipolar", true);
    trimKnob.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    trimKnob.setRotaryParameters (juce::MathConstants<float>::pi * 1.25f,
                                  juce::MathConstants<float>::pi * 2.75f, true);
    trimKnob.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 90, 22);
    trimKnob.setDoubleClickReturnValue (true, 0.0, juce::ModifierKeys::altModifier);
    trimKnob.setVelocityBasedMode (false);
    trimKnob.setTooltip ("Your ride on top of the Match gain, -18 to +18 dB (automatable). "
                         "0 dB = matched to the target. Alt-click or double-click for 0 dB.");

    addAndMakeVisible (matchButton);
    styleButton (matchButton);
    matchButton.setTooltip ("Set the Match gain so the dialogue lands on the target with the trim at 0 dB. "
                            "Resets the meter; your trim and its automation aren't touched.");
    matchButton.onClick = [this] { if (audioProcessor.applySuggestedMatch()) { resetPending = true; repaint(); } };

    addAndMakeVisible (clearMatchButton);
    styleButton (clearMatchButton);
    clearMatchButton.setTooltip ("Set the Match gain back to 0 dB and reset the meter.");
    clearMatchButton.onClick = [this] { audioProcessor.clearMatch(); resetPending = true; repaint(); };

    addChildComponent (useProgrammeButton);
    styleButton (useProgrammeButton);
    useProgrammeButton.setTooltip ("Set the target to the programme's loudness (from the side chain).");
    useProgrammeButton.onClick = [this] { audioProcessor.useProgrammeAsTarget(); };

    addAndMakeVisible (resetButton);
    styleButton (resetButton);
    resetButton.setTooltip ("Clear all readings, including the programme's. The trim and Match gain are kept.");
    resetButton.onClick = [this] { audioProcessor.requestMeterReset(); resetPending = true; repaint(); };


    addChildComponent (mixQuestion);
    mixQuestion.onAnswer = [this] (bool fiveOne)
    {
        audioProcessor.setMixFormat (fiveOne);
        mixQuestion.setVisible (false);
    };
    addChildComponent (measureQuestion);
    measureQuestion.onAnswer = [this] (bool integratedChoice)
    {
        audioProcessor.setMeasureMode (integratedChoice);
        measureQuestion.setVisible (false);
        timerCallback();
    };

    addAndMakeVisible (footer);

    for (int c = 0; c < vometer::kMaxChannels; ++c)
        levelDb[c] = holdDb[c] = -100.0f;

    setSize (520 + kMeterStripWidth + 10, 600 - (brand::headerHeight - kTitleRowHeight) + 2 + 36 - 80 + 24 - 40);
    startTimerHz (kFrameRate);
    timerCallback();
}

VOMeterEditor::~VOMeterEditor()
{
    stopTimer();
    setLookAndFeel (nullptr);
}

void VOMeterEditor::setTargetFromText (const juce::String& text)
{
    const auto digits = text.retainCharacters ("+-.0123456789");
    if (digits.containsAnyOf ("0123456789"))
    {
        float v = digits.getFloatValue();
        if (v > 0.0f) v = -v;   // "24" means -24 LUFS
        targetAttachment.setValueAsCompleteGesture (juce::jlimit (-36.0f, -10.0f, v));
    }
    targetLabel.setText (juce::String (audioProcessor.apvts.getRawParameterValue (VOMeterProcessor::targetId)->load(), 1),
                         juce::dontSendNotification);
    repaint();
}

void VOMeterEditor::showPresetMenu()
{
    struct Preset { const char* section; float lufs; const char* name; };
    static const Preset presets[] = {
        { "Broadcast", -23.0f, "-23 LUFS  \xc2\xb7  EBU R128" },
        { "Broadcast", -24.0f, "-24 LKFS  \xc2\xb7  ATSC A/85" },
        { "Streaming", -27.0f, "-27 LKFS  \xc2\xb7  Netflix dialogue" },
        { "Online",    -16.0f, "-16 LUFS  \xc2\xb7  Apple Podcasts" },
        { "Online",    -14.0f, "-14 LUFS  \xc2\xb7  Spotify / YouTube" },
    };
    constexpr int numPresets = (int) std::size (presets);

    juce::PopupMenu menu;
    menu.setLookAndFeel (&lnf);
    const float current = audioProcessor.apvts.getRawParameterValue (VOMeterProcessor::targetId)->load();
    juce::String section;
    for (int i = 0; i < numPresets; ++i)
    {
        const auto& pr = presets[i];
        if (section != pr.section)
        {
            section = pr.section;
            menu.addSectionHeader (section);
        }
        menu.addItem (i + 1, utf8 (pr.name), true, std::abs (current - pr.lufs) < 0.05f);
    }

    // Mono tracks: how the narration is heard (asked once, changeable here).
    constexpr int stereoId = 101, fiveOneId = 102;
    if (audioProcessor.isMonoTrack())
    {
        const bool fiveOne = audioProcessor.isFiveOneMix();
        menu.addSectionHeader ("Mix");
        menu.addItem (stereoId, "Stereo mix (mono narration reads +3 dB)", true, ! fiveOne);
        menu.addItem (fiveOneId, "5.1 mix (centre channel, no offset)", true, fiveOne);
    }

    // Stereo and 5.1 tracks: what the target applies to (asked once, changeable here).
    constexpr int dialogueId = 103, integratedId = 104;
    if (audioProcessor.asksMeasureMode())
    {
        const bool integ = audioProcessor.isIntegratedMode();
        menu.addSectionHeader ("Measure");
        menu.addItem (dialogueId, "Dialogue-gated (Netflix, dialogue specs)", true, ! integ);
        menu.addItem (integratedId, "Integrated (EBU R128, ATSC A/85)", true, integ);
    }

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (presetButton),
                        [this] (int result)
                        {
                            if (result >= 1 && result <= (int) std::size (presets))
                                targetAttachment.setValueAsCompleteGesture (presets[result - 1].lufs);
                            else if (result == 101 || result == 102)
                                audioProcessor.setMixFormat (result == 102);
                            else if (result == 103 || result == 104)
                                audioProcessor.setMeasureMode (result == 104);
                        });
}

void VOMeterEditor::timerCallback()
{
    auto& r = audioProcessor.readings;
    if (resetPending && ! audioProcessor.meterResetPending())
        resetPending = false;

    integrated = audioProcessor.isIntegratedMode();
    if (const bool trim = audioProcessor.hasTrim(); trim != showTrim)
    {
        showTrim = trim;   // no trim on a full mix (stereo / 5.1): re-lay out the gain panel
        trimKnob.setVisible (showTrim);
        resized();
    }
    outLufs = r.outLoudness.load();
    inLufs = r.inLoudness.load();
    lra = r.lra.load();
    tp = r.maxTruePeak.load();
    speechSec = r.speechSeconds.load();
    measuredSec = r.measuredSeconds.load();
    measuring = r.measuring.load();
    trimMoved = r.trimMovedSinceReset.load() && showTrim;
    matchMoved = r.matchMovedSinceReset.load();
    progLufs = r.programmeLoudness.load();
    progSignal = r.programmeSignal.load();
    matchDb = audioProcessor.apvts.getRawParameterValue (VOMeterProcessor::matchId)->load();
    auto showIf = [] (ChoiceQuestion& q, bool ask)
    {
        if (q.isVisible() == ask) return;
        q.setVisible (ask);
        if (ask) q.toFront (false);
    };
    showIf (mixQuestion, audioProcessor.isMonoTrack() && ! audioProcessor.isMixFormatChosen());
    showIf (measureQuestion, audioProcessor.asksMeasureMode() && ! audioProcessor.isMeasureModeChosen());
    suggested = audioProcessor.getSuggestedMatchDb();
    // Live meters: instant rise, then fall at 20 dB/s; the hold line waits 1.5 s.
    {
        const float dt = 1.0f / kFrameRate, fall = kFallDbPerSec * dt;
        meterChannels = juce::jlimit (1, vometer::kMaxChannels, r.numChannels.load());
        for (int c = 0; c < meterChannels; ++c)
        {
            const float p = juce::Decibels::gainToDecibels (r.takeOutPeak (c), -100.0f);
            levelDb[c] = std::max (p, levelDb[c] - fall);
            if (p >= holdDb[c]) { holdDb[c] = p; holdAge[c] = 0.0f; }
            else if ((holdAge[c] += dt) > kHoldSeconds) holdDb[c] -= fall;
        }
        gr = std::max (r.takeGrPeak(), gr - fall);
    }

    if (resetPending)
    {
        outLufs = inLufs = lra = suggested = progLufs = NAN;
        tp = -INFINITY; speechSec = measuredSec = 0.0f; trimMoved = matchMoved = false;
    }

    const float target = audioProcessor.apvts.getRawParameterValue (VOMeterProcessor::targetId)->load();
    const bool onTarget = ! std::isnan (outLufs) && std::abs (outLufs - target) <= vometer::kOnTargetLU;
    targetLabel.setColour (juce::Label::textColourId, onTarget ? brand::good : brand::textMain);

    matchButton.setEnabled (! std::isnan (suggested));
    clearMatchButton.setEnabled (std::abs (matchDb) > 0.05f);
    // The programme row only appears once a side chain brings programme audio.
    // Mono narration tracks only: on a full mix there's no programme to set a target from.
    const bool showProgramme = audioProcessor.isMonoTrack() && (progSignal || ! std::isnan (progLufs));
    useProgrammeButton.setVisible (showProgramme);
    useProgrammeButton.setEnabled (! std::isnan (progLufs));
    repaint (loudPanel.getUnion (trimPanel).getUnion (meterPanel));
}

void VOMeterEditor::paint (juce::Graphics& g)
{
    g.fillAll (brand::bg);
    // No subtitle line (the user prefers the plain title); resized() closes up its row.
    brand::drawHeader (g, getLocalBounds(), JucePlugin_Name, UpdateChecker::getCurrentVersion(), {});

    for (auto panel : { loudPanel, trimPanel, meterPanel })
    {
        g.setColour (brand::panel);
        g.fillRoundedRectangle (panel.toFloat(), 8.0f);
    }

    // ---- Loudness panel -----------------------------------------------------
    const float target = audioProcessor.apvts.getRawParameterValue (VOMeterProcessor::targetId)->load();
    const bool valid = ! std::isnan (outLufs);
    const float diff = valid ? outLufs - target : 0.0f;
    const bool onTarget = valid && std::abs (diff) <= vometer::kOnTargetLU;

    drawCaption (g, integrated ? "OUTPUT INTEGRATED" : "OUTPUT DIALOGUE", bigReadout.withHeight (18));
    auto big = bigReadout.withTrimmedTop (18);
    const juce::Font bigFont (juce::FontOptions (54.0f, juce::Font::bold));
    const auto value = valid ? fmt (outLufs) : utf8 (kDash);
    g.setColour (valid ? brand::textMain : brand::textDim);
    g.setFont (bigFont);
    g.drawText (value, big, juce::Justification::centredLeft);
    const int valueW = juce::roundToInt (juce::GlyphArrangement::getStringWidth (bigFont, value));
    g.setFont (juce::FontOptions (16.0f, juce::Font::bold));
    g.setColour (brand::textDim);
    g.drawText ("LUFS", big.withTrimmedLeft (valueW + 8).withTrimmedTop (20), juce::Justification::centredLeft);

    drawCaption (g, "TARGET", targetCaption);
    if (! targetLabel.isBeingEdited())
    {
        const auto tb = targetLabel.getBounds();
        const int textW = juce::roundToInt (juce::GlyphArrangement::getStringWidth (targetLabel.getFont(), targetLabel.getText()));
        const int unitX = tb.getX() + targetLabel.getBorderSize().getLeft() + textW + 6;
        g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
        g.setColour (brand::textDim);
        g.drawText ("LUFS", juce::Rectangle<int> (unitX, tb.getY() + 6, 40, tb.getHeight() - 6), juce::Justification::centredLeft);
    }

    // Status chip under the target.
    {
        juce::String text;
        juce::Colour fill = brand::track, ink = brand::textMain;
        if (! valid)
        {
            text = ! measuring ? "PRESS PLAY TO MEASURE" : (integrated ? "WAITING FOR AUDIO" : "WAITING FOR DIALOGUE");
            ink = brand::textDim;
        }
        else if (onTarget)
        {
            text = "ON TARGET";
            fill = brand::good; ink = brand::bg;
        }
        else
        {
            text = fmt (std::abs (diff)) + (diff > 0 ? " LU HIGH" : " LU LOW");
        }
        auto chip = statusArea.toFloat();
        g.setColour (fill);
        g.fillRoundedRectangle (chip, chip.getHeight() * 0.5f);
        g.setColour (ink);
        g.setFont (juce::FontOptions (brand::pillSize, juce::Font::bold));
        g.drawText (text, chip, juce::Justification::centred);
    }

    // Programme (side-chain) row under the status chip, once there is programme audio.
    if (useProgrammeButton.isVisible())
    {
        auto row = programmeArea;
        drawCaption (g, "PROGRAMME", row.removeFromLeft (78));
        row.removeFromRight (useProgrammeButton.getWidth() + 8);
        const bool has = ! std::isnan (progLufs);
        g.setColour (has ? brand::textMain : brand::textDim);
        g.setFont (juce::FontOptions (has ? 15.0f : brand::captionSize, juce::Font::bold));
        g.drawText (has ? fmt (progLufs) : juce::String ("listening..."),
                    row, juce::Justification::centredLeft);
    }

    {
        juce::String hint;
        if (matchMoved)
            hint = utf8 ("Match gain changed during this reading  \xc2\xb7  reset and replay to re-check");
        else if (valid)
            hint = utf8 ("Green = within \xc2\xb1") + fmt (vometer::kOnTargetLU) + " LU of the target"
                   + (trimMoved ? utf8 ("  \xc2\xb7  includes your trim rides") : juce::String())
                   + (measuring ? "" : utf8 ("  \xc2\xb7  paused"));
        else if (integrated)
            hint = utf8 ("Measures only while playing  \xc2\xb7  integrated loudness (BS.1770)");
        else
            hint = utf8 ("Measures only while playing  \xc2\xb7  ignores pauses, breaths and room tone");
        g.setColour (matchMoved ? brand::accent : brand::textDim);
        g.setFont (juce::FontOptions (brand::captionSize));
        g.drawText (hint, hintArea, juce::Justification::centredLeft);
    }

    // LRA and speech share, under the big readout.
    {
        auto row = statsArea;
        auto tile = [&] (const juce::String& caption, const juce::String& text, juce::Colour ink)
        {
            auto t = row.removeFromLeft (110);
            drawCaption (g, caption, t.removeFromTop (16));
            g.setColour (ink);
            g.setFont (juce::FontOptions (17.0f, juce::Font::bold));
            g.drawText (text, t, juce::Justification::centredLeft);
            return t;
        };

        tile ("LRA", std::isnan (lra) ? utf8 (kDash) : fmt (lra) + " LU", std::isnan (lra) ? brand::textDim : brand::textMain);

        const bool hasTime = measuredSec > 0.05f;
        tile ("SPEECH", hasTime ? juce::String (juce::roundToInt (100.0f * speechSec / measuredSec)) + "%" : utf8 (kDash),
              hasTime ? brand::textMain : brand::textDim);
    }

    drawMeters (g);

    // ---- Trim panel ---------------------------------------------------------
    if (showTrim)
        drawCaption (g, "TRIM", trimCaption, juce::Justification::centred);

    drawCaption (g, integrated ? "INPUT INTEGRATED  (BEFORE GAIN)" : "INPUT DIALOGUE  (BEFORE GAIN)", inputArea.withHeight (18));
    g.setColour (std::isnan (inLufs) ? brand::textDim : brand::textMain);
    g.setFont (juce::FontOptions (24.0f, juce::Font::bold));
    g.drawText (std::isnan (inLufs) ? utf8 (kDash) : fmt (inLufs) + " LUFS", inputArea.withTrimmedTop (18), juce::Justification::centredLeft);

    drawCaption (g, "MATCH GAIN", suggestArea.withHeight (18));
    {
        auto row = suggestArea.withTrimmedTop (18);
        const juce::Font f (juce::FontOptions (24.0f, juce::Font::bold));
        const auto text = signedDb (matchDb);
        g.setColour (std::abs (matchDb) > 0.05f ? brand::accent : brand::textDim);
        g.setFont (f);
        g.drawText (text, row, juce::Justification::centredLeft);

        // The suggestion, when Match would change it.
        if (! std::isnan (suggested) && std::abs (suggested - matchDb) > 0.05f)
        {
            juce::String sug = "Match: " + signedDb (suggested);
            if (std::abs (suggested) >= vometer::kMatchRangeDb - 0.01f)
                sug += " (limit)";
            g.setColour (brand::textDim);
            g.setFont (juce::FontOptions (brand::captionSize, juce::Font::bold));
            g.drawText (sug, row.withTrimmedLeft (juce::roundToInt (juce::GlyphArrangement::getStringWidth (f, text)) + 12),
                        juce::Justification::centredLeft);
        }
    }
}

void VOMeterEditor::resized()
{
    auto area = getLocalBounds().reduced (brand::margin);
    area.removeFromTop (kTitleRowHeight + 10);

    footer.setBounds (area.removeFromBottom (brand::footerHeight));
    mixQuestion.setBounds (getLocalBounds().withBottom (footer.getY() - 6));
    measureQuestion.setBounds (mixQuestion.getBounds());
    area.removeFromBottom (10);   // no Bypass pill: the DAW's bypass drives the bypass parameter

    meterPanel = area.removeFromRight (kMeterStripWidth);
    area.removeFromRight (10);

    // Loudness panel
    loudPanel = area.removeFromTop (210);
    {
        auto inner = loudPanel.reduced (16, 12);
        hintArea = inner.removeFromBottom (18);
        inner.removeFromBottom (8);
        auto right = inner.removeFromRight (204);
        bigReadout = inner;
        statsArea = bigReadout.removeFromBottom (38);
        targetCaption = right.removeFromTop (18);
        auto targetRow = right.removeFromTop (44);
        presetButton.setBounds (targetRow.removeFromRight (64).withSizeKeepingCentre (64, 26));
        targetLabel.setBounds (targetRow.withWidth (90));
        right.removeFromTop (6);
        statusArea = right.removeFromTop (28);
        right.removeFromTop (10);
        programmeArea = right.removeFromTop (26);
        useProgrammeButton.setBounds (programmeArea.withLeft (programmeArea.getRight() - 52));
    }
    area.removeFromTop (10);


    trimPanel = area;
    {
        auto inner = trimPanel.reduced (16, 10);
        if (showTrim)   // a full mix (stereo / 5.1) has no trim
        {
            auto knobArea = inner.removeFromLeft (170);
            trimCaption = knobArea.removeFromTop (16);
            trimKnob.setBounds (knobArea);
            inner.removeFromLeft (20);
        }
        inputArea = inner.removeFromTop (48);
        inner.removeFromTop (6);
        suggestArea = inner.removeFromTop (48);
        auto buttons = inner.removeFromBottom (30);
        matchButton.setBounds (buttons.removeFromLeft (76));
        buttons.removeFromLeft (8);
        clearMatchButton.setBounds (buttons.removeFromLeft (64));
        buttons.removeFromLeft (8);
        resetButton.setBounds (buttons.removeFromLeft (112));
    }
}

// The meter strip: output sample peak per channel (dBFS, -60 to 0, with the
// limiter's ceiling and a peak-hold line) and the limiter's gain reduction
// (0 to 12 dB, growing down from the top). Numbers underneath: the max true
// peak since Reset Meter, and the gain reduction right now.
void VOMeterEditor::drawMeters (juce::Graphics& g)
{
    auto inner = meterPanel.reduced (10, 12);
    auto captions = inner.removeFromTop (16);
    auto numbers = inner.removeFromBottom (40);
    inner.removeFromBottom (8);

    auto scale = inner.removeFromLeft (24);
    auto grBar = inner.removeFromRight (16);
    inner.removeFromRight (8);
    auto levels = inner;

    drawCaption (g, "OUT", captions.withX (levels.getX()).withWidth (levels.getWidth()), juce::Justification::centred);
    drawCaption (g, "GR", captions.withX (grBar.getX() - 6).withWidth (grBar.getWidth() + 12), juce::Justification::centred);

    auto yFor = [&] (float db)
    {
        const float t = juce::jlimit (0.0f, 1.0f, (db - kMeterFloorDb) / -kMeterFloorDb);
        return (float) levels.getBottom() - t * (float) levels.getHeight();
    };

    // Scale.
    g.setFont (juce::FontOptions (10.0f));
    for (float db : { 0.0f, -6.0f, -12.0f, -18.0f, -24.0f, -36.0f, -48.0f, -60.0f })
    {
        const float y = yFor (db);
        g.setColour (brand::textDim);
        g.drawText (juce::String ((int) db), juce::Rectangle<float> ((float) scale.getX(), y - 6.0f, (float) scale.getWidth() - 4.0f, 12.0f),
                    juce::Justification::centredRight);
        g.setColour (brand::track);
        g.drawHorizontalLine (juce::roundToInt (y), (float) levels.getX(), (float) levels.getRight());
    }

    // Level bars.
    const int gap = meterChannels > 2 ? 2 : 4;
    const float barW = (float) (levels.getWidth() - gap * (meterChannels - 1)) / (float) meterChannels;
    for (int c = 0; c < meterChannels; ++c)
    {
        const auto bar = juce::Rectangle<float> ((float) levels.getX() + c * (barW + gap), (float) levels.getY(), barW, (float) levels.getHeight());
        g.setColour (brand::track.withAlpha (0.6f));
        g.fillRect (bar);
        if (levelDb[c] > kMeterFloorDb)
        {
            g.setColour (brand::accent);
            g.fillRect (bar.withTop (yFor (levelDb[c])));
        }
        if (holdDb[c] > kMeterFloorDb)
        {
            g.setColour (brand::textMain);
            g.fillRect (bar.withTop (yFor (holdDb[c]) - 1.0f).withHeight (2.0f));
        }
    }

    // The limiter's ceiling.
    g.setColour (brand::warn.withAlpha (0.8f));
    g.drawHorizontalLine (juce::roundToInt (yFor (vometer::kCeilingDbTP)), (float) levels.getX() - 3.0f, (float) levels.getRight() + 3.0f);

    // Gain reduction bar.
    {
        const auto bar = grBar.toFloat();
        g.setColour (brand::track.withAlpha (0.6f));
        g.fillRect (bar);
        g.setColour (brand::track);
        for (float db : { 3.0f, 6.0f, 9.0f })
            g.drawHorizontalLine (juce::roundToInt (bar.getY() + bar.getHeight() * db / kGrRangeDb), bar.getX(), bar.getRight());
        if (gr > 0.05f)
        {
            g.setColour (brand::warn);
            g.fillRect (bar.withHeight (bar.getHeight() * juce::jlimit (0.0f, 1.0f, gr / kGrRangeDb)));
        }
    }

    // Numbers.
    auto line = [&] (const juce::String& caption, const juce::String& value, juce::Colour ink)
    {
        auto row = numbers.removeFromTop (20);
        drawCaption (g, caption, row);
        g.setColour (ink);
        g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
        g.drawText (value, row, juce::Justification::centredRight);
    };
    // Whole-pass max true peak since Reset Meter (the deliverable figure).
    const bool tpValid = std::isfinite (tp);
    const bool tpOver = tpValid && tp > vometer::kCeilingDbTP + 0.05f;
    line ("TP MAX", tpValid ? fmt (tp) : utf8 (kDash), tpOver ? brand::warn : (tpValid ? brand::textMain : brand::textDim));
    line ("GR", gr > 0.05f ? "-" + fmt (gr) : juce::String ("0.0"), gr > 0.05f ? brand::warn : brand::textDim);
}
