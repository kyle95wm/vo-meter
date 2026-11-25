// Renders the plugin editor to a PNG without a host, for the README.
//   cmake -B build -DVO_METER_BUILD_SCREENSHOT_TOOL=ON && cmake --build build --target VOMeterScreenshot
//   ./build/VOMeterScreenshot_artefacts/Release/VOMeterScreenshot docs/screenshot.png
#include <juce_gui_basics/juce_gui_basics.h>
#include "PluginProcessor.h"
#include <cstdio>

int main (int argc, char** argv)
{
    const juce::ScopedJuceInitialiser_GUI gui;

    const juce::File out = juce::File::getCurrentWorkingDirectory()
                               .getChildFile (argc > 1 ? argv[1] : "docs/screenshot.png");

    VOMeterProcessor processor;

    // Play 20 s of a voice-like demo signal (syllables and pauses) through it,
    // press Match, then play it again, so the shot shows real readings.
    {
        const double sr = 48000.0;
        processor.setPlayConfigDetails (2, 2, sr, 512);
        processor.prepareToPlay (sr, 512);
        juce::AudioBuffer<float> demo (2, (int) (20.0 * sr));
        juce::Random rng (7);
        for (int i = 0; i < demo.getNumSamples(); ++i)
        {
            const double t = i / sr;
            const double syl = std::fmod (t, 0.25) / 0.25;                    // 4 syllables a second
            const bool pause = std::fmod (t, 3.0) > 2.2;                       // phrase gaps
            const double env = pause ? 0.0 : std::pow (std::sin (juce::MathConstants<double>::pi * syl), 2.0)
                                                * (0.6 + 0.4 * std::sin (t * 1.7));
            const double voice = 0.6 * std::sin (juce::MathConstants<double>::twoPi * 220.0 * t)
                               + 0.3 * std::sin (juce::MathConstants<double>::twoPi * 1100.0 * t);
            const float x = (float) (0.12 * env * voice + 0.0005 * (rng.nextFloat() - 0.5f));
            demo.setSample (0, i, x);
            demo.setSample (1, i, x);
        }
        juce::MidiBuffer midi;
        auto play = [&]
        {
            juce::AudioBuffer<float> copy (demo);
            for (int start = 0; start < copy.getNumSamples(); start += 512)
            {
                juce::AudioBuffer<float> view (copy.getArrayOfWritePointers(), 2, start, std::min (512, copy.getNumSamples() - start));
                processor.processBlock (view, midi);
            }
        };
        play();
        processor.applySuggestedTrim();
        auto* trim = processor.apvts.getParameter (VOMeterProcessor::trimId);
        trim->setValueNotifyingHost (trim->convertTo0to1 (processor.apvts.getRawParameterValue (VOMeterProcessor::trimId)->load() + 0.6f));
        processor.requestMeterReset();
        play();
    }

    std::unique_ptr<juce::AudioProcessorEditor> editor (processor.createEditor());
    editor->setVisible (true);

    // Let the editor's update check finish (up to 5 s) so the shot shows a settled state.
    for (int i = 0; i < 50 && processor.updateChecker.getResult().status == UpdateChecker::Status::checking; ++i)
        juce::MessageManager::getInstance()->runDispatchLoopUntil (100);
    juce::MessageManager::getInstance()->runDispatchLoopUntil (100); // one more UI refresh

    const auto image = editor->createComponentSnapshot (editor->getLocalBounds(), true, 2.0f);

    out.getParentDirectory().createDirectory();
    out.deleteFile();
    juce::FileOutputStream stream (out);
    juce::PNGImageFormat png;
    if (! stream.openedOk() || ! png.writeImageToStream (image, stream))
    {
        std::fprintf (stderr, "Couldn't write %s\n", out.getFullPathName().toRawUTF8());
        return 1;
    }
    std::printf ("Wrote %s (%dx%d)\n", out.getFullPathName().toRawUTF8(), image.getWidth(), image.getHeight());
    return 0;
}
