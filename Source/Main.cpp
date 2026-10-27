#include "DeckComponent.h"
#include "CassetteComponent.h"
#include "PluginScan.h"
#include "CrashHandler.h"

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
        deck.setTransform (juce::AffineTransform::scale (s).translated (((float) getWidth() - (float) DeckComponent::designWidth * s) * 0.5f,
                                                                        ((float) getHeight() - (float) DeckComponent::designHeight * s) * 0.5f));
    }

    DeckComponent deck;
};

class MainWindow : public juce::DocumentWindow
{
public:
    MainWindow (AudioEngine& engine, juce::PropertiesFile& settings)
        : juce::DocumentWindow (utf ("TAPE\xc2\xb7" "FOUR  ") + JUCE_APPLICATION_VERSION_STRING, juce::Colour (0xff141311), juce::DocumentWindow::allButtons)
    {
        setUsingNativeTitleBar (true);
        content = new ScaledDeck (engine, settings);
        setContentOwned (content, false);
        setResizable (true, true);
        setResizeLimits (800, 450, 4800, 2700);

        auto area = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay() != nullptr
                        ? juce::Desktop::getInstance().getDisplays().getPrimaryDisplay()->userArea
                        : juce::Rectangle<int> (0, 0, 1440, 1000);
        auto s = std::min ({ 1.0f, ((float) area.getWidth() - 40.0f) / (float) DeckComponent::designWidth,
                             ((float) area.getHeight() - 60.0f) / (float) DeckComponent::designHeight });
        centreWithSize ((int) (DeckComponent::designWidth * s), (int) (DeckComponent::designHeight * s));

        // Where it was last time, if that's still on a screen (JUCE checks).
        if (auto saved = settings.getValue (windowKey); saved.isNotEmpty())
            restoreWindowStateFromString (saved);
        setVisible (true);
        content->deck.grabKeyboardFocus();
    }

    void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }

    void saveWindowState (juce::PropertiesFile& settings) { settings.setValue (windowKey, getWindowStateAsString()); }

    /** Brings the window back to the front (when TapeFour is started again while running). */
    void bringToFront()
    {
        if (isMinimised()) setMinimised (false);
        toFront (true);
    }

    ScaledDeck* content = nullptr;

private:
    static constexpr const char* windowKey = "windowState";
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

    static bool renderCassettes (const juce::File& folder)
    {
        folder.createDirectory();
        bool ok = true;
        for (int model = 0; model < tf::numTapeModels; ++model)
            for (int side = 0; side < 2; ++side)
            {
                tf::Tape tape (1, model);
                tape.name = "four track demos";
                tf::CassetteComponent c;
                c.setBounds (0, 0, 1004, 638);
                c.setTape (&tape);
                c.setSide (side);
                c.update (&tape, (double) tape.getLengthCells() * 0.35);
                auto image = c.createComponentSnapshot (c.getLocalBounds(), true, 1.0f);
                auto file = folder.getChildFile (juce::String (tf::tapeModels[model].name) + (side == 0 ? "-A" : "-B") + ".png");
                file.deleteFile();
                juce::FileOutputStream out (file);
                ok = out.openedOk() && juce::PNGImageFormat().writeImageToStream (image, out) && ok;
                // ...and the look a .cass carries for it, to check that a player can draw it the same
                if (side == 0)
                    ok = tf::CassetteComponent::writeArt (tf::CassetteComponent::ArtSpec::of (tape),
                                                         folder.getChildFile (juce::String (tf::tapeModels[model].name) + "-look")) && ok;
            }
        return ok;
    }

    void initialise (const juce::String& commandLine) override
    {
        if (tf::PluginScan::isHelperCommandLine (commandLine))
        {
            setApplicationReturnValue (tf::PluginScan::runHelper (commandLine));
            quit();
            return;
        }

        tf::CrashHandler::install();

        lookAndFeel = std::make_unique<tf::DeckLookAndFeel>();
        juce::LookAndFeel::setDefaultLookAndFeel (lookAndFeel.get());

        // A tool for making artwork: --render-cassettes <folder> draws each formulation's cassette, at full size, as a PNG.
        if (commandLine.contains ("--render-cassettes"))
        {
            auto args = juce::StringArray::fromTokens (commandLine, true);
            auto i = args.indexOf ("--render-cassettes");
            auto folder = juce::File::getCurrentWorkingDirectory().getChildFile (args[i + 1].unquoted());
            setApplicationReturnValue (renderCassettes (folder) ? 0 : 1);
            quit();
            return;
        }

        juce::PropertiesFile::Options o;
        o.applicationName = "TapeFour";
        o.filenameSuffix = ".settings";
       #if JUCE_LINUX || JUCE_BSD
        o.folderName = ".config/TapeFour";   // the usual place on Linux, not a folder in your home
       #else
        o.folderName = "TapeFour";
       #endif
        o.osxLibrarySubFolder = "Application Support";
        appProperties.setStorageParameters (o);

        engine = std::make_unique<tf::AudioEngine> (*appProperties.getUserSettings());
        mainWindow = std::make_unique<tf::MainWindow> (*engine, *appProperties.getUserSettings());

        if (auto crashes = tf::CrashHandler::takeNewReportCount (*appProperties.getUserSettings()); crashes > 0)
            mainWindow->content->deck.notice ("LAST SESSION CRASHED - REPORT SAVED IN TAPEFOUR/CRASHES", 8.0);

        openCassettesIn (commandLine);
    }

    void shutdown() override
    {
        if (mainWindow != nullptr)
        {
            mainWindow->saveWindowState (*appProperties.getUserSettings());
            mainWindow->content->deck.prepareToQuit();
        }
        mainWindow.reset();

        if (engine != nullptr)
            engine->saveSettings();
        engine.reset();

        appProperties.closeFiles();
        juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
        lookAndFeel.reset();
    }

    void systemRequestedQuit() override { quit(); }
    void anotherInstanceStarted (const juce::String& commandLine) override
    {
        // Only one TapeFour at a time (both would want the audio device and the tape); a second start
        // brings this one forward instead, with any cassette it was asked to open.
        if (mainWindow != nullptr) mainWindow->bringToFront();
        openCassettesIn (commandLine);
    }

private:
    /** A .cass file named on the command line (or double-clicked) goes in the deck. */
    void openCassettesIn (const juce::String& commandLine)
    {
        if (mainWindow == nullptr) return;
        for (auto& arg : juce::StringArray::fromTokens (commandLine, true))
        {
            juce::File f (arg.unquoted());
            if (juce::File::isAbsolutePath (arg.unquoted()) && f.hasFileExtension ("cass") && f.existsAsFile())
            {
                mainWindow->content->deck.openCassetteFile (f);   // waits for a tape that's still loading
                return;
            }
        }
    }

    std::unique_ptr<tf::DeckLookAndFeel> lookAndFeel;
    juce::ApplicationProperties appProperties;
    std::unique_ptr<tf::AudioEngine> engine;
    std::unique_ptr<tf::MainWindow> mainWindow;
};

START_JUCE_APPLICATION (TapeFourApplication)
