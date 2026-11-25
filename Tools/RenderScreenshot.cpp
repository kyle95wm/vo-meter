// Renders the plugin editor to a PNG without a host, for the README.
//   cmake -B build -DVO_METER_BUILD_SCREENSHOT_TOOL=ON && cmake --build build --target VOMeterScreenshot
//   ./build/VOMeterScreenshot_artefacts/Release/VOMeterScreenshot docs/screenshot.png [--ask] [--integrated]
// --ask shows the one-time question; --stereo renders a stereo mix (dialogue-gated);
// --integrated renders a stereo mix in Integrated mode; --surround makes the mix 5.1;
// --hot pushes the gain so the limiter works.
#include <juce_gui_basics/juce_gui_basics.h>
#include "PluginProcessor.h"
#include <cstdio>

int main (int argc, char** argv)
{
    const juce::ScopedJuceInitialiser_GUI gui;

    const juce::File out = juce::File::getCurrentWorkingDirectory()
                               .getChildFile (argc > 1 ? argv[1] : "docs/screenshot.png");

    VOMeterProcessor processor;
    bool showQuestion = false, integrated = false, stereoMix = false, surround = false, hot = false;
    for (int i = 2; i < argc; ++i)
    {
        showQuestion = showQuestion || juce::String (argv[i]) == "--ask";
        integrated = integrated || juce::String (argv[i]) == "--integrated";
        stereoMix = stereoMix || juce::String (argv[i]) == "--stereo";
        surround = surround || juce::String (argv[i]) == "--surround";
        hot = hot || juce::String (argv[i]) == "--hot";
    }

    // Play 20 s of a voice-like demo signal (syllables and pauses) through it,
    // set the target from the side chain, press Match, ride the trim a little,
    // then play it again, so the shot shows real readings.
    {
        const double sr = 48000.0;
        // A mono narration track (as in Logic), with the stereo programme side chain;
        // or a stereo track measured integrated.
        stereoMix = stereoMix || surround;
        const auto mainSet = surround ? juce::AudioChannelSet::create5point1()
                           : (integrated || stereoMix) ? juce::AudioChannelSet::stereo() : juce::AudioChannelSet::mono();
        juce::AudioProcessor::BusesLayout layout;
        layout.inputBuses.add (mainSet);
        layout.inputBuses.add (juce::AudioChannelSet::stereo());
        layout.outputBuses.add (mainSet);
        processor.setBusesLayout (layout);
        processor.prepareToPlay (sr, 512);
        if ((integrated || stereoMix) && ! showQuestion)
            processor.setMeasureMode (integrated);
        juce::AudioBuffer<float> demo (1, (int) (20.0 * sr));
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
            demo.setSample (0, i, (float) (0.12 * env * voice + 0.0005 * (rng.nextFloat() - 0.5f)));
        }
        // Narration on the main input; the same shape, 6 dB hotter, as "programme" on the side chain.
        const int total = std::max (processor.getTotalNumInputChannels(), processor.getTotalNumOutputChannels());
        const int mainCh = processor.getMainBusNumInputChannels();
        juce::MidiBuffer midi;
        auto play = [&]
        {
            juce::AudioBuffer<float> work (total, demo.getNumSamples());
            for (int c = 0; c < total; ++c)
            {
                work.copyFrom (c, 0, demo, 0, 0, demo.getNumSamples());
                if (c >= mainCh) work.applyGain (c, 0, demo.getNumSamples(), 2.0f);
            }
            for (int start = 0; start < work.getNumSamples(); start += 512)
            {
                juce::AudioBuffer<float> view (work.getArrayOfWritePointers(), total, start, std::min (512, work.getNumSamples() - start));
                processor.processBlock (view, midi);
            }
        };
        play();
        processor.useProgrammeAsTarget();
        processor.applySuggestedMatch();
        auto* trim = processor.apvts.getParameter (VOMeterProcessor::trimId);
        trim->setValueNotifyingHost (trim->convertTo0to1 (1.5f));   // a ride
        if (hot)
        {
            auto* match = processor.apvts.getParameter (VOMeterProcessor::matchId);
            match->setValueNotifyingHost (match->convertTo0to1 (20.0f));
        }
        processor.requestMeterReset();
        play();
    }

    // The README shot shows an answered "stereo or 5.1?" question; --ask shows the question.
    if (! showQuestion && ! integrated && ! stereoMix)
        processor.setMixFormat (false);

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
