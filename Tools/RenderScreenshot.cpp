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
