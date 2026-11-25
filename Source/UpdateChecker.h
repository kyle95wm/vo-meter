#pragma once

#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>
#include <juce_events/juce_events.h>

// Checks GitHub Releases (repos/<PLUGIN_GITHUB_REPO>/releases/latest) for a
// newer version than the one this binary was built as.
//
// - One shared checker per process, so ten plugin instances = one request.
// - Automatic checks run at most once every 24 hours and can be turned off;
//   the setting and the last result are stored in the user's app-data folder
//   (<JucePlugin_Name>.settings), so it applies to every instance and every host.
// - Never touches the network on the audio thread.
class UpdateChecker
{
public:
    enum class Status { idle, checking, upToDate, updateAvailable, failed };

    struct Result
    {
        Status status = Status::idle;
        juce::String latestVersion;   // e.g. "1.2.0"
        juce::String releaseUrl;      // release page to open in the browser
        juce::String error;
    };

    UpdateChecker();
    ~UpdateChecker();

    // Runs a check now (manual = ignore the 24 h throttle and the auto-check setting).
    void checkNow (bool manual);

    Result getResult() const;

    bool isAutoCheckEnabled() const;
    void setAutoCheckEnabled (bool enabled);

    static juce::String getCurrentVersion();
    // Returns >0 if a is newer than b, <0 if older, 0 if equal. Accepts a leading "v".
    static int compareVersions (const juce::String& a, const juce::String& b);

    class Impl;

private:
    juce::SharedResourcePointer<Impl> impl;
};
