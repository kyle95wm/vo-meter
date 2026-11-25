#include "PluginEditor.h"

VOMeterEditor::VOMeterEditor (VOMeterProcessor& p)
    : AudioProcessorEditor (&p), audioProcessor (p),
      footer (p.updateChecker),
      knobAttachment (p.apvts, VOMeterProcessor::gainId, knob),
      bypassAttachment (p.apvts, VOMeterProcessor::bypassId, bypassButton)
{
    setLookAndFeel (&lnf);

    // Add controls *before* styling them: a slider builds its text box from the
    // look-and-feel it can see at that moment, and before it has a parent that
    // is the default one (you get a stray outline round the value).
    addAndMakeVisible (knob);
    knob.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    knob.setRotaryParameters (juce::MathConstants<float>::pi * 1.25f,
                              juce::MathConstants<float>::pi * 2.75f, true);
    knob.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 80, 24);
    knob.setDoubleClickReturnValue (true, 0.0);
    knob.setVelocityBasedMode (false);

    brand::makePill (bypassButton, "ACTIVE", "BYPASSED");
    bypassButton.setTooltip ("Bypass the processing (also linked to your DAW's bypass).");
    addAndMakeVisible (bypassButton);

    addAndMakeVisible (footer);

    setSize (340, 480);
}

VOMeterEditor::~VOMeterEditor()
{
    setLookAndFeel (nullptr);
}

void VOMeterEditor::paint (juce::Graphics& g)
{
    g.fillAll (brand::bg);
    // Non-ASCII subtitle text must go through CharPointer_UTF8 (see BrandLookAndFeel.h).
    brand::drawHeader (g, getLocalBounds(), JucePlugin_Name, UpdateChecker::getCurrentVersion(),
                       juce::String (juce::CharPointer_UTF8 ("One knob  \xc2\xb7  describe the plugin here")));
}

void VOMeterEditor::resized()
{
    auto area = getLocalBounds().reduced (brand::margin);
    area.removeFromTop (brand::headerHeight);

    footer.setBounds (area.removeFromBottom (brand::footerHeight));
    area.removeFromBottom (10);

    auto pills = area.removeFromBottom (36);
    bypassButton.setBounds (pills.withSizeKeepingCentre (130, 30));
    area.removeFromBottom (14);

    knob.setBounds (area.withSizeKeepingCentre (std::min (area.getWidth(), area.getHeight()),
                                                area.getHeight()));
}
