#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginProcessor.h"
#include "BrandLookAndFeel.h"
#include "UpdateFooter.h"

// Window layout (see CONVENTIONS.md):
//   header   - plugin name and version (no subtitle)
//   loudness - big output LUFS (dialogue-gated, or integrated when mastering a mix),
//              editable target (green when on target), programme loudness from
//              the sidechain + Use (as target; mono narration tracks only)
//   stats    - LRA, speech share
//   meters   - strip down the right: output peak per channel (with the -2.5 dBTP
//              ceiling and a peak hold) and the limiter's gain reduction, with
//              the max true peak since reset and the gain reduction as numbers
//   gain     - Trim knob (your ride; alt-click = 0 dB), input loudness, Match gain,
//              Match / Clear / Reset Meter
//   pills    - Bypass
// A one-time question covers the body until it's answered: "stereo or 5.1 mix?"
// on a mono track, "dialogue-gated or integrated?" on a stereo or 5.1 track.
// Either answer can be changed later in the Presets menu.
//   footer   - shared UpdateFooter
// The shared look, plus a bipolar knob for the trim: sliders with the "bipolar"
// property draw their value arc from 12 o'clock (0 dB) instead of from the start.
// (The shared BrandLookAndFeel.h stays untouched.)
class VOMeterLookAndFeel : public brand::LookAndFeel
{
public:
    void drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h, float pos,
                           float startAngle, float endAngle, juce::Slider& s) override
    {
        if (! s.getProperties().getWithDefault ("bipolar", false))
        {
            brand::LookAndFeel::drawRotarySlider (g, x, y, w, h, pos, startAngle, endAngle, s);
            return;
        }

        // Same knob as the shared one, without its start-anchored value arc ...
        brand::LookAndFeel::drawRotarySlider (g, x, y, w, h, 0.0f, startAngle, endAngle, s);

        // ... then the value arc from the centre, and the pointer at the value.
        const auto bounds = juce::Rectangle<int> (x, y, w, h).toFloat().reduced (10.0f);
        const float radius = std::min (bounds.getWidth(), bounds.getHeight()) * 0.5f;
        const auto centre = bounds.getCentre();
        const float angle = startAngle + pos * (endAngle - startAngle);
        const float mid = 0.5f * (startAngle + endAngle);
        const float arcW = radius * 0.09f, arcR = radius - arcW * 0.5f;
        if (std::abs (angle - mid) > 0.01f)
        {
            juce::Path arc;
            arc.addCentredArc (centre.x, centre.y, arcR, arcR, 0.0f, std::min (mid, angle), std::max (mid, angle), true);
            g.setColour (brand::accent);
            g.strokePath (arc, juce::PathStrokeType (arcW, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }

        // The base call drew the pointer at the start; cover it with the body
        // colour and draw the real one (same geometry as the shared knob).
        const float bodyR = radius * 0.72f, pw = radius * 0.06f;
        g.saveState();
        g.reduceClipRegion (juce::Rectangle<float> (centre.x - bodyR + 2, centre.y - bodyR + 2, bodyR * 2 - 4, bodyR * 2 - 4).toNearestInt());
        juce::Path body;
        body.addEllipse (centre.x - bodyR + 1.5f, centre.y - bodyR + 1.5f, bodyR * 2 - 3, bodyR * 2 - 3);
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff3a404c), centre.x, centre.y - bodyR,
                                                 juce::Colour (0xff22262e), centre.x, centre.y + bodyR, false));
        g.fillPath (body);
        g.restoreState();

        juce::Path pointer;
        pointer.addRoundedRectangle (-pw * 0.5f, -bodyR * 0.92f, pw, bodyR * 0.38f, pw * 0.5f);
        pointer.applyTransform (juce::AffineTransform::rotation (angle).translated (centre));
        g.setColour (brand::textMain);
        g.fillPath (pointer);
    }
};

// A one-time two-choice question that covers the body until it's answered:
// "stereo or 5.1 mix?" on mono tracks, "dialogue-gated or integrated?" on
// stereo and 5.1 tracks. The first choice is the highlighted default.
class ChoiceQuestion : public juce::Component
{
public:
    std::function<void (bool second)> onAnswer;

    ChoiceQuestion (const juce::String& id, const juce::String& titleText, const juce::String& bodyText,
                    const juce::String& firstText, const juce::String& secondText)
        : title (titleText), body (bodyText)
    {
        setComponentID (id);
        first.setButtonText (firstText);
        second.setButtonText (secondText);
        for (auto* b : { &first, &second })
        {
            b->setColour (juce::TextButton::buttonColourId, brand::track);
            b->setColour (juce::TextButton::textColourOffId, brand::textMain);
            addAndMakeVisible (*b);
        }
        first.setColour (juce::TextButton::buttonColourId, brand::accent);
        first.setColour (juce::TextButton::textColourOffId, brand::bg);
        first.onClick = [this] { if (onAnswer) onAnswer (false); };
        second.onClick = [this] { if (onAnswer) onAnswer (true); };
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (brand::bg.withAlpha (0.9f));
        g.setColour (brand::panel);
        g.fillRoundedRectangle (card.toFloat(), 10.0f);
        auto r = card.reduced (24, 22);
        g.setColour (brand::textMain);
        g.setFont (juce::FontOptions (20.0f, juce::Font::bold));
        g.drawText (title, r.removeFromTop (28), juce::Justification::centred);
        r.removeFromTop (8);
        g.setColour (brand::textDim);
        g.setFont (juce::FontOptions (13.0f));
        g.drawFittedText (body, r.removeFromTop (76), juce::Justification::centredTop, 5);
    }

    void resized() override
    {
        card = getLocalBounds().withSizeKeepingCentre (std::min (420, getWidth() - 20), 210);
        auto row = card.reduced (24, 22).removeFromBottom (36).withSizeKeepingCentre (2 * 150 + 16, 36);
        first.setBounds (row.removeFromLeft (150));
        second.setBounds (row.removeFromRight (150));
    }

private:
    juce::String title, body;
    juce::TextButton first, second;
    juce::Rectangle<int> card;
};

class VOMeterEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit VOMeterEditor (VOMeterProcessor&);
    ~VOMeterEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void showPresetMenu();
    void setTargetFromText (const juce::String& text);
    void drawMeters (juce::Graphics&);

    VOMeterProcessor& audioProcessor;
    VOMeterLookAndFeel lnf;

    juce::Label targetLabel;
    juce::TextButton presetButton { "Presets" };
    juce::Slider trimKnob;
    juce::TextButton matchButton { "Match" }, clearMatchButton { "Clear" }, resetButton { "Reset Meter" };
    juce::TextButton useProgrammeButton { "Use" };
    juce::ToggleButton bypassButton { "Bypass" };
    ChoiceQuestion mixQuestion, measureQuestion;
    UpdateFooter footer;

    juce::ParameterAttachment targetAttachment;
    juce::AudioProcessorValueTreeState::SliderAttachment trimAttachment;
    juce::AudioProcessorValueTreeState::ButtonAttachment bypassAttachment;

    // Areas laid out in resized(), painted in paint().
    juce::Rectangle<int> loudPanel, bigReadout, targetCaption, statusArea, programmeArea, hintArea;
    juce::Rectangle<int> statsPanel, trimPanel, trimCaption, inputArea, suggestArea, meterPanel;

    // Meter ballistics (dB): instant rise, then fall; the hold line waits, then falls.
    float levelDb[vometer::kMaxChannels], holdDb[vometer::kMaxChannels], holdAge[vometer::kMaxChannels] {};
    int meterChannels = 2;

    // Snapshot of the processor's readings, refreshed by the timer.
    float outLufs = NAN, inLufs = NAN, lra = NAN, tp = -INFINITY, gr = 0.0f, suggested = NAN;
    float speechSec = 0.0f, measuredSec = 0.0f, matchDb = 0.0f, progLufs = NAN;
    bool integrated = false, showTrim = true, measuring = false, trimMoved = false, matchMoved = false, progSignal = false, resetPending = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VOMeterEditor)
};
