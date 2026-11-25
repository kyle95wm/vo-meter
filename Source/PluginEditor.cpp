#include "PluginEditor.h"

namespace
{
juce::String utf8 (const char* s) { return juce::String (juce::CharPointer_UTF8 (s)); }
const char* kDash = "\xe2\x80\x94";   // em dash: "no reading yet"
constexpr int kTitleRowHeight = 28;     // brand::drawHeader's title row (no subtitle row here)

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
      trimAttachment (p.apvts, VOMeterProcessor::trimId, trimKnob),
      bypassAttachment (p.apvts, VOMeterProcessor::bypassId, bypassButton)
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

    brand::makePill (bypassButton, "ACTIVE", "BYPASSED");
    bypassButton.setTooltip ("Bypass the trim and limiter (also linked to your DAW's bypass).");
    addAndMakeVisible (bypassButton);

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

    setSize (520, 600 - (brand::headerHeight - kTitleRowHeight) + 2 + 36);
    startTimerHz (20);
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
    gr = std::max (r.limiterGr.load(), gr - 1.0f);   // hold, then fall 20 dB/s

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
    const bool showProgramme = progSignal || ! std::isnan (progLufs);
    useProgrammeButton.setVisible (showProgramme);
    useProgrammeButton.setEnabled (! std::isnan (progLufs));
    repaint (loudPanel.getUnion (statsPanel).getUnion (trimPanel));
}

void VOMeterEditor::paint (juce::Graphics& g)
{
    g.fillAll (brand::bg);
    // No subtitle line (the user prefers the plain title); resized() closes up its row.
    brand::drawHeader (g, getLocalBounds(), JucePlugin_Name, UpdateChecker::getCurrentVersion(), {});

    for (auto panel : { loudPanel, statsPanel, trimPanel })
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

    // ---- Stats row ----------------------------------------------------------
    {
        auto row = statsPanel.reduced (14, 10);
        const int w = row.getWidth() / 4;
        auto tile = [&] (const juce::String& caption, const juce::String& text, juce::Colour ink)
        {
            auto t = row.removeFromLeft (w);
            drawCaption (g, caption, t.removeFromTop (18));
            g.setColour (ink);
            g.setFont (juce::FontOptions (20.0f, juce::Font::bold));
            g.drawText (text, t, juce::Justification::centredLeft);
            return t;
        };

        const bool tpValid = std::isfinite (tp);
        const bool tpOver = tpValid && tp > vometer::kCeilingDbTP + 0.05f;
        tile ("MAX TRUE PEAK", tpValid ? fmt (tp) + " dBTP" : utf8 (kDash), tpOver ? brand::warn : (tpValid ? brand::textMain : brand::textDim));

        const bool limiting = gr > 0.1f;
        // The -2.5 dBTP limiter's live gain reduction (held, then falling 20 dB/s).
        auto grTile = tile ("GAIN REDUCTION", limiting ? "-" + fmt (gr) + " dB" : juce::String ("None"), limiting ? brand::warn : brand::textDim);
        auto bar = grTile.removeFromBottom (4).withTrimmedRight (16).toFloat();
        g.setColour (brand::track);
        g.fillRoundedRectangle (bar, 2.0f);
        g.setColour (brand::warn);
        g.fillRoundedRectangle (bar.withWidth (bar.getWidth() * juce::jlimit (0.0f, 1.0f, gr / 12.0f)), 2.0f);

        tile ("LRA", std::isnan (lra) ? utf8 (kDash) : fmt (lra) + " LU", std::isnan (lra) ? brand::textDim : brand::textMain);

        const bool hasTime = measuredSec > 0.05f;
        tile ("SPEECH", hasTime ? juce::String (juce::roundToInt (100.0f * speechSec / measuredSec)) + "%" : utf8 (kDash),
              hasTime ? brand::textMain : brand::textDim);
    }

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
    area.removeFromBottom (6);
    bypassButton.setBounds (area.removeFromBottom (32).withSizeKeepingCentre (130, 30));
    area.removeFromBottom (12);

    // Loudness panel
    loudPanel = area.removeFromTop (186);
    {
        auto inner = loudPanel.reduced (16, 12);
        hintArea = inner.removeFromBottom (18);
        auto right = inner.removeFromRight (204);
        bigReadout = inner;
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

    statsPanel = area.removeFromTop (70);
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
