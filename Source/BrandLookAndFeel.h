#pragma once

// Shared look and feel for every kyle95wm plugin. Keep this file identical
// across repos; the only per-plugin value is the accent colour, which comes
// from CMake (BRAND_ACCENT_ARGB, set from the accent line
// in the identity block of CMakeLists.txt).
//
// TEXT GOTCHA: a plain C++ string literal is read by juce::String as Latin-1,
// so non-ASCII UI text (·, –, →, µ, °) comes out garbled. Wrap it:
//     juce::String (juce::CharPointer_UTF8 ("Snappy VO  \xc2\xb7  de-ess"))
// (use \x escapes, or a UTF-8 source file plus CharPointer_UTF8).

#include <juce_gui_basics/juce_gui_basics.h>

#ifndef BRAND_ACCENT_ARGB
 #error "Define BRAND_ACCENT_ARGB=0xffRRGGBB (CMakeLists.txt does this)"
#endif

namespace brand
{
// ---- Family palette ---------------------------------------------------------
const juce::Colour bg       { 0xff16181d };  // window background
const juce::Colour panel    { 0xff1f232b };  // header strips, meter panels, text boxes
const juce::Colour track    { 0xff2d323c };  // knob tracks, dividers, off-state pills, buttons
const juce::Colour textMain { 0xffe8eaf0 };
const juce::Colour textDim  { 0xff8a91a0 };  // labels, captions, version, update status
const juce::Colour good     { 0xff59d18c };  // "Up to date", healthy status
const juce::Colour warn     { 0xffff5f5f };  // errors, limiter / clip indicators
const juce::Colour accent   { (juce::uint32) (BRAND_ACCENT_ARGB) };  // one per plugin

// ---- Type sizes -------------------------------------------------------------
constexpr float titleSize   = 22.0f;  // plugin name, bold, top-left
constexpr float captionSize = 12.0f;  // version, subtitle, footer, meter labels
constexpr float pillSize    = 13.0f;  // pill toggle text, bold

// ---- Layout -----------------------------------------------------------------
constexpr int margin       = 16;   // window edge to content
constexpr int headerHeight = 46;   // title row (28) + subtitle row (18)
constexpr int footerHeight = 58;   // UpdateFooter, incl. its divider gap

// Pill toggle: call once per ToggleButton, then it draws as a rounded pill
// that is filled with onColour (default: accent) when on. Use for Bypass and
// any other on/off parameter shown as a switch.
inline void makePill (juce::ToggleButton& b, const juce::String& offText, const juce::String& onText,
                      juce::Colour onColour = accent)
{
    auto& p = b.getProperties();
    p.set ("pill", true);
    p.set ("offText", offText);
    p.set ("onText", onText);
    p.set ("onColour", (juce::int64) onColour.getARGB());
}

// Title row + subtitle, drawn by the editor's paint(). Returns the area below.
inline juce::Rectangle<int> drawHeader (juce::Graphics& g, juce::Rectangle<int> bounds,
                                        const juce::String& name, const juce::String& version,
                                        const juce::String& subtitle)
{
    auto area = bounds.reduced (margin);
    auto titleRow = area.removeFromTop (28);
    g.setColour (textMain);
    g.setFont (juce::FontOptions (titleSize, juce::Font::bold));
    g.drawText (name, titleRow, juce::Justification::centredLeft);
    g.setColour (textDim);
    g.setFont (juce::FontOptions (captionSize));
    g.drawText ("v" + version, titleRow, juce::Justification::centredRight);
    g.drawText (subtitle, area.removeFromTop (18), juce::Justification::centredLeft);
    return area;
}

// =============================================================================
class LookAndFeel : public juce::LookAndFeel_V4
{
public:
    LookAndFeel()
    {
        setColour (juce::ResizableWindow::backgroundColourId, bg);
        setColour (juce::Slider::textBoxTextColourId, textMain);
        setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
        setColour (juce::Label::textColourId, textMain);
        setColour (juce::TextButton::buttonColourId, track);
        setColour (juce::TextButton::textColourOffId, textMain);
        setColour (juce::ToggleButton::textColourId, textDim);
        setColour (juce::ToggleButton::tickColourId, accent);
        setColour (juce::ToggleButton::tickDisabledColourId, textDim);
        setColour (juce::TooltipWindow::backgroundColourId, panel);
        setColour (juce::TooltipWindow::textColourId, textMain);
    }

    void drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h, float pos,
                           float startAngle, float endAngle, juce::Slider&) override
    {
        const auto bounds = juce::Rectangle<int> (x, y, w, h).toFloat().reduced (10.0f);
        const float radius = std::min (bounds.getWidth(), bounds.getHeight()) * 0.5f;
        const auto centre = bounds.getCentre();
        const float angle = startAngle + pos * (endAngle - startAngle);
        const float arcW = radius * 0.09f;
        const float arcR = radius - arcW * 0.5f;
        const juce::PathStrokeType stroke (arcW, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);

        juce::Path bgArc;
        bgArc.addCentredArc (centre.x, centre.y, arcR, arcR, 0.0f, startAngle, endAngle, true);
        g.setColour (track);
        g.strokePath (bgArc, stroke);

        if (pos > 0.0f)
        {
            juce::Path valueArc;
            valueArc.addCentredArc (centre.x, centre.y, arcR, arcR, 0.0f, startAngle, angle, true);
            g.setColour (accent);
            g.strokePath (valueArc, stroke);
        }

        const float bodyR = radius * 0.72f;
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff3a404c), centre.x, centre.y - bodyR,
                                                 juce::Colour (0xff22262e), centre.x, centre.y + bodyR, false));
        g.fillEllipse (centre.x - bodyR, centre.y - bodyR, bodyR * 2, bodyR * 2);
        g.setColour (juce::Colours::black.withAlpha (0.5f));
        g.drawEllipse (centre.x - bodyR, centre.y - bodyR, bodyR * 2, bodyR * 2, 1.5f);

        juce::Path pointer;
        const float pw = radius * 0.06f;
        pointer.addRoundedRectangle (-pw * 0.5f, -bodyR * 0.92f, pw, bodyR * 0.38f, pw * 0.5f);
        pointer.applyTransform (juce::AffineTransform::rotation (angle).translated (centre));
        g.setColour (textMain);
        g.fillPath (pointer);
    }

    void drawToggleButton (juce::Graphics& g, juce::ToggleButton& b, bool highlighted, bool down) override
    {
        const auto& props = b.getProperties();
        if (! props.getWithDefault ("pill", false))
        {
            LookAndFeel_V4::drawToggleButton (g, b, highlighted, down);  // ordinary tick box
            return;
        }

        const auto r = b.getLocalBounds().toFloat().reduced (1.0f);
        const bool on = b.getToggleState();
        const auto onColour = juce::Colour ((juce::uint32) (juce::int64) props.getWithDefault ("onColour", (juce::int64) accent.getARGB()));

        g.setColour (on ? onColour : (highlighted ? track.brighter (0.15f) : track));
        g.fillRoundedRectangle (r, r.getHeight() * 0.5f);
        g.setColour (on ? bg : textMain);
        g.setFont (juce::FontOptions (pillSize, juce::Font::bold));
        g.drawText (props[on ? "onText" : "offText"].toString(), r, juce::Justification::centred);
    }
};
} // namespace brand
