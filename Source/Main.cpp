#include "DeckComponent.h"
#include "PluginScan.h"

namespace tf
{
/** Holds the deck at its design size and scales it to whatever the window is. */
class ScaledDeck : public juce::Component
{
public:
    ScaledDeck (AudioEngine& engine, juce::PropertiesFile& settings) : deck (engine, settings)
    {
        addAndMakeVisible (deck);
        setOpaque (true);
    }

    void paint (juce::Graphics& g) override { g.fillAll (juce::Colour (0xff141311)); }

    void resized() override
    {
        auto s = std::min ((float) getWidth() / DeckComponent::designWidth, (float) getHeight() / DeckComponent::designHeight);
        deck.setBounds (0, 0, DeckComponent::designWidth, DeckComponent::designHeight);
        deck.setTransform (juce::AffineTransform::scale (s).translated ((getWidth() - DeckComponent::designWidth * s) * 0.5f,
                                                                        (getHeight() - DeckComponent::designHeight * s) * 0.5f));
    }

    DeckComponent deck;
};

class MainWindow : public juce::DocumentWindow
{
public:
    MainWindow (AudioEngine& engine, juce::PropertiesFile& settings)
        : juce::DocumentWindow (utf ("TAPE\xc2\xb7" "FOUR"), juce::Colour (0xff141311), juce::DocumentWindow::allButtons)
    {
        setUsingNativeTitleBar (true);
        content = new ScaledDeck (engine, settings);
        setContentOwned (content, false);
        setResizable (true, true);
        setResizeLimits (800, 450, 4800, 2700);

        auto area = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay() != nullptr
                        ? juce::Desktop::getInstance().getDisplays().getPrimaryDisplay()->userArea
                        : juce::Rectangle<int> (0, 0, 1440, 1000);
        auto s = std::min ({ 1.0f, (area.getWidth() - 40.0f) / DeckComponent::designWidth,
                             (area.getHeight() - 60.0f) / DeckComponent::designHeight });
        centreWithSize ((int) (DeckComponent::designWidth * s), (int) (DeckComponent::designHeight * s));
        setVisible (true);
        content->deck.grabKeyboardFocus();
    }

    void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }

    ScaledDeck* content = nullptr;
};
} // namespace tf

//==============================================================================
class TapeFourApplication : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override    { return JUCE_APPLICATION_NAME_STRING; }
    const juce::String getApplicationVersion() override { return JUCE_APPLICATION_VERSION_STRING; }
    bool moreThanOneInstanceAllowed() override
    {
        // Plug-in scan helpers are extra copies of TapeFour started by the first one.
        return tf::PluginScan::isHelperCommandLine (getCommandLineParameters());
    }

    void initialise (const juce::String& commandLine) override
    {
        if (tf::PluginScan::isHelperCommandLine (commandLine))
        {
            setApplicationReturnValue (tf::PluginScan::runHelper (commandLine));
            quit();
            return;
        }

        lookAndFeel = std::make_unique<tf::DeckLookAndFeel>();
        juce::LookAndFeel::setDefaultLookAndFeel (lookAndFeel.get());

        juce::PropertiesFile::Options o;
        o.applicationName = "TapeFour";
        o.filenameSuffix = ".settings";
        o.folderName = "TapeFour";
        o.osxLibrarySubFolder = "Application Support";
        appProperties.setStorageParameters (o);

        engine = std::make_unique<tf::AudioEngine> (*appProperties.getUserSettings());
        mainWindow = std::make_unique<tf::MainWindow> (*engine, *appProperties.getUserSettings());
    }

    void shutdown() override
    {
        if (mainWindow != nullptr)
            mainWindow->content->deck.prepareToQuit();
        mainWindow.reset();

        if (engine != nullptr)
            engine->saveSettings();
        engine.reset();

        appProperties.closeFiles();
        juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
        lookAndFeel.reset();
    }

    void systemRequestedQuit() override { quit(); }
    void anotherInstanceStarted (const juce::String&) override {}

private:
    std::unique_ptr<tf::DeckLookAndFeel> lookAndFeel;
    juce::ApplicationProperties appProperties;
    std::unique_ptr<tf::AudioEngine> engine;
    std::unique_ptr<tf::MainWindow> mainWindow;
};

START_JUCE_APPLICATION (TapeFourApplication)
