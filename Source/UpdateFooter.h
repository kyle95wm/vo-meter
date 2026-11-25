#pragma once

// Shared footer for every kyle95wm plugin: a divider, the update status
// ("Up to date" / "Update available: v1.2.0" / error), a "Check for updates"
// button that turns into "Download" when a newer release exists, and a
// "Check automatically" toggle. Keep this file identical across repos.
//
// Usage in the editor: addAndMakeVisible (footer);
//                      footer.setBounds (area.removeFromBottom (brand::footerHeight));

#include "BrandLookAndFeel.h"
#include "UpdateChecker.h"

class UpdateFooter : public juce::Component, private juce::Timer
{
public:
    explicit UpdateFooter (UpdateChecker& c) : checker (c)
    {
        statusLabel.setFont (juce::FontOptions (brand::captionSize));
        statusLabel.setColour (juce::Label::textColourId, brand::textDim);
        statusLabel.setJustificationType (juce::Justification::centredRight);
        addAndMakeVisible (statusLabel);

        checkButton.onClick = [this]
        {
            if (pendingReleaseUrl.isNotEmpty())
                juce::URL (pendingReleaseUrl).launchInDefaultBrowser();
            else
                checker.checkNow (true);
            refresh();
        };
        addAndMakeVisible (checkButton);

        autoToggle.setToggleState (checker.isAutoCheckEnabled(), juce::dontSendNotification);
        autoToggle.onClick = [this] { checker.setAutoCheckEnabled (autoToggle.getToggleState()); };
        addAndMakeVisible (autoToggle);

        checker.checkNow (false);  // throttled to once a day, respects the toggle
        refresh();
        startTimerHz (4);
    }

    void paint (juce::Graphics& g) override
    {
        g.setColour (brand::track);
        g.fillRect (0, 0, getWidth(), 1);
    }

    void resized() override
    {
        auto f = getLocalBounds().withTrimmedTop (10);
        statusLabel.setBounds (f.removeFromTop (22));
        f.removeFromTop (4);
        auto row = f.removeFromTop (24);
        checkButton.setBounds (row.removeFromRight (140));
        autoToggle.setBounds (row.withTrimmedRight (8));
    }

private:
    void timerCallback() override { refresh(); }

    void refresh()
    {
        using S = UpdateChecker::Status;
        const auto r = checker.getResult();

        pendingReleaseUrl.clear();
        juce::String text, button = "Check for updates";
        auto colour = brand::textDim;

        switch (r.status)
        {
            case S::idle:      break;
            case S::checking:  text = "Checking GitHub..."; break;
            case S::upToDate:  text = "Up to date"; colour = brand::good; break;
            case S::failed:    text = r.error; colour = brand::warn; break;
            case S::updateAvailable:
                text = "Update available: v" + r.latestVersion;
                colour = brand::accent;
                button = "Download";
                pendingReleaseUrl = r.releaseUrl;
                break;
        }

        if (statusLabel.getText() != text)
            statusLabel.setText (text, juce::dontSendNotification);
        statusLabel.setColour (juce::Label::textColourId, colour);
        if (checkButton.getButtonText() != button)
            checkButton.setButtonText (button);
    }

    UpdateChecker& checker;
    juce::Label statusLabel;
    juce::TextButton checkButton { "Check for updates" };
    juce::ToggleButton autoToggle { "Check automatically" };
    juce::String pendingReleaseUrl;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (UpdateFooter)
};
