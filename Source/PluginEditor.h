#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginProcessor.h"
#include "BrandLookAndFeel.h"
#include "UpdateFooter.h"

// Window layout (see CONVENTIONS.md):
//   header  - plugin name top-left (bold 22), version top-right, one-line subtitle
//   body    - the controls, accent colour for values/arcs
//   pills   - Bypass (and other switches) as a row of pill toggles above the footer
//   footer  - shared UpdateFooter: divider, update status, Check/Download, auto toggle
class VOMeterEditor : public juce::AudioProcessorEditor
{
public:
    explicit VOMeterEditor (VOMeterProcessor&);
    ~VOMeterEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    [[maybe_unused]] VOMeterProcessor& audioProcessor;  // for meters, parameters, etc.
    brand::LookAndFeel lnf;

    juce::Slider knob;
    juce::ToggleButton bypassButton { "Bypass" };
    UpdateFooter footer;

    juce::AudioProcessorValueTreeState::SliderAttachment knobAttachment;
    juce::AudioProcessorValueTreeState::ButtonAttachment bypassAttachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VOMeterEditor)
};
