#include "PlayerComponent.h"
#include "SystemVolume.h"
#include "Launch.h"

#ifndef JUCE_APPLICATION_VERSION_STRING
 #define JUCE_APPLICATION_VERSION_STRING "0.2"   // (a Projucer build: Android)
#endif
#include "../Source/CassetteComponent.h"

namespace tf::pocket
{
// A phone or tablet: the keys come up when you tap the cassette. --touch shows that layout on a computer.
static bool touchLayout (const juce::String& commandLine)
{
   #if JUCE_IOS || JUCE_ANDROID
    juce::ignoreUnused (commandLine);
    return true;
   #else
    return commandLine.contains ("--touch");
   #endif
}

class MainWindow : public juce::DocumentWindow
{
public:
    MainWindow (PlayerEngine& engine, Library& library, bool touch)
        : juce::DocumentWindow ("TF-P1 Pocket", juce::Colour (0xff0a0a0a), juce::DocumentWindow::allButtons)
    {
        setUsingNativeTitleBar (true);
        player = new PlayerComponent (engine, library, touch);
        setContentOwned (player, false);
       #if JUCE_IOS || JUCE_ANDROID
        setFullScreen (true);
       #else
        setResizable (true, true);
        setResizeLimits (touch ? 560 : 860, 380, 4000, 3000);
        centreWithSize (touch ? 844 : 920, touch ? 390 : 620);
       #endif
        setVisible (true);
        player->grabKeyboardFocus();
    }

    void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }

    PlayerComponent* player = nullptr;
};
} // namespace tf::pocket

class PocketApplication : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override    { return "TF-P1 Pocket"; }
    const juce::String getApplicationVersion() override { return JUCE_APPLICATION_VERSION_STRING; }
    bool moreThanOneInstanceAllowed() override          { return false; }

    void initialise (const juce::String& commandLine) override
    {
        lookAndFeel = std::make_unique<tf::DeckLookAndFeel>();
        juce::LookAndFeel::setDefaultLookAndFeel (lookAndFeel.get());
       #if JUCE_IOS || JUCE_ANDROID
        juce::Desktop::getInstance().setOrientationsEnabled (juce::Desktop::rotatedClockwise | juce::Desktop::rotatedAntiClockwise);
       #endif

        juce::PropertiesFile::Options o;
        o.applicationName = "Pocket";
        o.filenameSuffix = ".settings";
       #if JUCE_LINUX || JUCE_BSD
        o.folderName = ".config/TapeFour";
       #else
        o.folderName = "TapeFour";
       #endif
        o.osxLibrarySubFolder = "Application Support";
        settings.setStorageParameters (o);

        engine = std::make_unique<tf::pocket::PlayerEngine>();
        library = std::make_unique<tf::pocket::Library> (*settings.getUserSettings());
        tf::Theme::apply (tf::Theme::indexOf (library->theme()));
        auto audioError = engine->start();
        window = std::make_unique<tf::pocket::MainWindow> (*engine, *library, tf::pocket::touchLayout (commandLine));

        // A phone: its own volume is the player's (the engine plays at full level under it),
        // and cassettes shared to the player while it was closed join the others.
        if (tf::pocket::SystemVolume::available())
        {
            engine->setVolume (10);
            juce::MessageManager::callAsync ([] { tf::pocket::SystemVolume::attach(); });
        }
       #if JUCE_IOS || JUCE_ANDROID
        library->takeInNewArrivals();
        // Opened with the player from another app (Android hands it over as the launch Intent).
        if (auto url = tf::pocket::documentOpenedWith(); ! url.isEmpty())
        {
            window->player->importAndOpen (url);
            return;
        }
       #endif

        // The cassette it was playing last, or one it was asked to open.
        if (! openFrom (commandLine))
        {
            auto recent = library->recent();
            if (! recent.empty() && recent.front().file.existsAsFile())
                window->player->openCassette (recent.front().file);
        }
        if (audioError.isNotEmpty())
            juce::Logger::writeToLog ("No audio output: " + audioError);
    }

    void shutdown() override
    {
        window = nullptr;   // remembers where the cassette was
        engine = nullptr;
        library = nullptr;
        settings.closeFiles();
        juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
    }

    void systemRequestedQuit() override { quit(); }

    void anotherInstanceStarted (const juce::String& commandLine) override
    {
        // Opening a .cass while the player runs (double-clicked, dropped on its icon, sent from another app).
        if (window != nullptr && openFrom (commandLine))
            window->toFront (true);
    }

    void suspended() override { if (window != nullptr && window->player != nullptr) window->player->rememberPosition(); }

    void resumed() override
    {
       #if JUCE_IOS || JUCE_ANDROID
        // Back from the share sheet or the Files app: a cassette sent to the player is put in.
        if (library != nullptr && window != nullptr)
        {
            auto arrived = library->takeInNewArrivals();
            if (! arrived.isEmpty()) window->player->openCassette (arrived.getFirst());
        }
        tf::pocket::SystemVolume::attach();
       #endif
    }

private:
    bool openFrom (const juce::String& commandLine)
    {
        for (auto arg : juce::StringArray::fromTokens (commandLine, true))
        {
            arg = arg.unquoted();
            if (arg.startsWith ("file://")) arg = juce::URL (arg).getLocalFile().getFullPathName();
            if (arg.endsWithIgnoreCase (".cass"))
            {
                juce::File f (juce::File::isAbsolutePath (arg) ? arg : juce::File::getCurrentWorkingDirectory().getChildFile (arg).getFullPathName());
                if (f.existsAsFile()) { window->player->openCassette (f); return true; }
            }
        }
        return false;
    }

    std::unique_ptr<tf::DeckLookAndFeel> lookAndFeel;
    juce::ApplicationProperties settings;
    std::unique_ptr<tf::pocket::PlayerEngine> engine;
    std::unique_ptr<tf::pocket::Library> library;
    std::unique_ptr<tf::pocket::MainWindow> window;
};

START_JUCE_APPLICATION (PocketApplication)
