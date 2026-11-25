#include "UpdateChecker.h"

// Shared by every kyle95wm plugin: keep this file identical across repos.
// The repo comes from CMake (PLUGIN_GITHUB_REPO), the settings file and
// User-Agent from the plugin name.
#ifndef PLUGIN_GITHUB_REPO
 #error "Define PLUGIN_GITHUB_REPO=\"owner/repo\" (CMakeLists.txt does this)"
#endif

namespace
{
    constexpr const char* kAutoCheckKey   = "autoCheckForUpdates";
    constexpr const char* kLastCheckKey   = "lastUpdateCheckMs";
    constexpr const char* kLatestKey      = "latestKnownVersion";
    constexpr const char* kLatestUrlKey   = "latestKnownReleaseUrl";
    constexpr juce::int64 kAutoIntervalMs = 24 * 60 * 60 * 1000;

    juce::PropertiesFile::Options settingsOptions()
    {
        juce::PropertiesFile::Options o;
        o.applicationName     = JucePlugin_Name;
        o.filenameSuffix      = ".settings";
        o.folderName          = JucePlugin_Name;
        o.osxLibrarySubFolder = "Application Support";
        o.processLock         = nullptr;
        return o;
    }

    juce::Array<int> parseVersion (juce::String v)
    {
        v = v.trim();
        if (v.startsWithIgnoreCase ("v"))
            v = v.substring (1);
        v = v.upToFirstOccurrenceOf ("-", false, false); // ignore pre-release suffixes

        juce::Array<int> parts;
        for (auto& p : juce::StringArray::fromTokens (v, ".", ""))
            parts.add (p.getIntValue());
        while (parts.size() < 3)
            parts.add (0);
        return parts;
    }
}

class UpdateChecker::Impl : private juce::Thread
{
public:
    Impl() : juce::Thread (juce::String (JucePlugin_Name) + " update check"), settings (settingsOptions())
    {
        // Show the last known result straight away (no network needed).
        const auto known = settings.getValue (kLatestKey);
        if (known.isNotEmpty() && compareVersions (known, getCurrentVersion()) > 0)
        {
            result.status = Status::updateAvailable;
            result.latestVersion = known;
            result.releaseUrl = settings.getValue (kLatestUrlKey, releasesPage());
        }
    }

    ~Impl() override { stopThread (10000); }

    void checkNow (bool manual)
    {
        if (! manual)
        {
            if (! isAutoCheckEnabled())
                return;
            const auto last = settings.getValue (kLastCheckKey).getLargeIntValue();
            if (juce::Time::currentTimeMillis() - last < kAutoIntervalMs)
                return;
        }

        if (isThreadRunning())
            return;

        {
            const juce::ScopedLock sl (lock);
            result.status = Status::checking;
            result.error.clear();
        }
        startThread();
    }

    Result getResult() const
    {
        const juce::ScopedLock sl (lock);
        return result;
    }

    bool isAutoCheckEnabled() const
    {
        const juce::ScopedLock sl (lock);
        return settings.getBoolValue (kAutoCheckKey, true);
    }

    void setAutoCheckEnabled (bool enabled)
    {
        const juce::ScopedLock sl (lock);
        settings.setValue (kAutoCheckKey, enabled);
        settings.saveIfNeeded();
    }

private:
    static juce::String releasesPage()
    {
        return juce::String ("https://github.com/") + PLUGIN_GITHUB_REPO + "/releases/latest";
    }

    void run() override
    {
        Result r;
        const juce::URL url (juce::String ("https://api.github.com/repos/") + PLUGIN_GITHUB_REPO + "/releases/latest");

        int statusCode = 0;
        auto stream = url.createInputStream (
            juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                .withConnectionTimeoutMs (8000)
                .withStatusCode (&statusCode)
                .withExtraHeaders ("Accept: application/vnd.github+json\r\n"
                                   "User-Agent: " JucePlugin_Name "-update-checker\r\n"));

        if (threadShouldExit())
            return;

        if (stream == nullptr)
        {
            r.status = Status::failed;
            r.error = "Couldn't reach GitHub";
        }
        else if (statusCode == 404)
        {
            // No published release yet.
            r.status = Status::upToDate;
        }
        else if (statusCode != 200)
        {
            r.status = Status::failed;
            r.error = "GitHub returned HTTP " + juce::String (statusCode);
        }
        else
        {
            const auto json = juce::JSON::parse (stream->readEntireStreamAsString());
            const auto tag = json.getProperty ("tag_name", {}).toString();
            const auto page = json.getProperty ("html_url", releasesPage()).toString();

            if (tag.isEmpty())
            {
                r.status = Status::failed;
                r.error = "Unexpected response from GitHub";
            }
            else
            {
                r.latestVersion = tag.startsWithIgnoreCase ("v") ? tag.substring (1) : tag;
                r.releaseUrl = page;
                r.status = compareVersions (tag, getCurrentVersion()) > 0 ? Status::updateAvailable
                                                                          : Status::upToDate;
            }
        }

        const juce::ScopedLock sl (lock);
        if (r.status != Status::failed)
        {
            settings.setValue (kLastCheckKey, juce::String (juce::Time::currentTimeMillis()));
            settings.setValue (kLatestKey, r.latestVersion);
            settings.setValue (kLatestUrlKey, r.releaseUrl);
            settings.saveIfNeeded();
        }
        result = r;
    }

    juce::CriticalSection lock;
    mutable juce::PropertiesFile settings;
    Result result;
};

UpdateChecker::UpdateChecker()  = default;
UpdateChecker::~UpdateChecker() = default;

void UpdateChecker::checkNow (bool manual)                 { impl->checkNow (manual); }
UpdateChecker::Result UpdateChecker::getResult() const     { return impl->getResult(); }
bool UpdateChecker::isAutoCheckEnabled() const             { return impl->isAutoCheckEnabled(); }
void UpdateChecker::setAutoCheckEnabled (bool e)           { impl->setAutoCheckEnabled (e); }

juce::String UpdateChecker::getCurrentVersion()
{
    return JucePlugin_VersionString;
}

int UpdateChecker::compareVersions (const juce::String& a, const juce::String& b)
{
    const auto va = parseVersion (a), vb = parseVersion (b);
    for (int i = 0; i < juce::jmax (va.size(), vb.size()); ++i)
    {
        const int x = va[i], y = vb[i];
        if (x != y)
            return x > y ? 1 : -1;
    }
    return 0;
}
