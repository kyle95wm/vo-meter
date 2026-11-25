#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginProcessor.h"
#include "BrandLookAndFeel.h"
#include "UpdateFooter.h"

// Window layout (see CONVENTIONS.md):
//   header   - plugin name, version, subtitle
//   loudness - big dialogue-gated output LUFS, editable target (green when on target)
//   stats    - max true peak, limiter gain reduction, LRA, speech share
//   trim     - Trim knob (alt-click = 0 dB), input loudness, suggested trim, Match, Reset Meter
//   pills    - Bypass
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

    VOMeterProcessor& audioProcessor;
    VOMeterLookAndFeel lnf;

    juce::Label targetLabel;
    juce::TextButton presetButton { "Presets" };
    juce::Slider trimKnob;
    juce::TextButton matchButton { "Match" }, resetButton { "Reset Meter" };
    juce::ToggleButton bypassButton { "Bypass" };
    UpdateFooter footer;

    juce::ParameterAttachment targetAttachment;
    juce::AudioProcessorValueTreeState::SliderAttachment trimAttachment;
    juce::AudioProcessorValueTreeState::ButtonAttachment bypassAttachment;

    // Areas laid out in resized(), painted in paint().
    juce::Rectangle<int> loudPanel, bigReadout, targetCaption, statusArea, hintArea;
    juce::Rectangle<int> statsPanel, trimPanel, trimCaption, inputArea, suggestArea;

    // Snapshot of the processor's readings, refreshed by the timer.
    float outLufs = NAN, inLufs = NAN, lra = NAN, tp = -INFINITY, gr = 0.0f, suggested = NAN;
    float speechSec = 0.0f, measuredSec = 0.0f;
    bool measuring = false, trimMoved = false, resetPending = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VOMeterEditor)
};
