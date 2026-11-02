#include "DeckComponent.h"
#include "CassetteComponent.h"
#include "PluginScan.h"
#include "CrashHandler.h"
#include "InstrumentEditor.h"
#include "ElectricPianoPanel.h"
#include "TransistorPanel.h"
#include "PhraseSamplerPanel.h"
#include "SpringPanel.h"
#include "N5Panel.h"

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

    /** For the manual: --render-sampler <folder> fills a phrase sampler with made-up sounds and draws its window, tab by tab. */
    static bool renderSampler (const juce::File& folder)
    {
        folder.createDirectory();
        constexpr double sr = 48000.0;
        tf::sampler::Processor p;
        p.setRateAndBufferSizeDetails (sr, 512);
        p.prepareToPlay (sr, 512);
        auto run = [&p] (int blocks) { juce::AudioBuffer<float> b (2, 512); juce::MidiBuffer m; for (int k = 0; k < blocks; ++k) { b.clear(); p.processBlock (b, m); } };
        // made-up sounds, synthesised: a kick, a snare, a drum loop, a chord stab, a bass note
        juce::Random rnd (7);
        auto make = [&] (const char* name, double seconds, std::function<double (double)> f, int pad)
        {
            auto file = folder.getChildFile (juce::String (name) + ".wav");
            {
                std::unique_ptr<juce::OutputStream> os (file.createOutputStream());
                juce::WavAudioFormat wav;
                auto w = wav.createWriterFor (os, juce::AudioFormatWriterOptions().withSampleRate (44100.0).withNumChannels (1).withBitsPerSample (16));
                std::vector<float> v ((size_t) (seconds * 44100.0));
                for (size_t i = 0; i < v.size(); ++i) v[i] = (float) f ((double) i / 44100.0);
                const float* ch[] { v.data() };
                w->writeFromFloatArrays (ch, 1, (int) v.size());
            }
            juce::String e;
            p.importFile (file, pad, e);
            file.deleteFile();
        };
        auto kick = [] (double t) { return 0.9 * std::exp (-t * 9.0) * std::sin (2.0 * juce::MathConstants<double>::pi * (50.0 * t + 90.0 * (1.0 - std::exp (-t * 30.0)) / 30.0)); };
        auto snare = [&rnd] (double t) { return std::exp (-t * 18.0) * (0.5 * (rnd.nextDouble() * 2.0 - 1.0) + 0.4 * std::sin (2.0 * juce::MathConstants<double>::pi * 190.0 * t)); };
        const double beat = 60.0 / 94.0;
        auto loop = [&] (double t)
        {
            const double inBar = std::fmod (t, beat * 4.0);
            const double k = std::fmod (inBar, beat * 2.0), s = std::fmod (inBar + beat, beat * 2.0), h = std::fmod (inBar, beat * 0.5);
            return 0.8 * kick (k) + 0.6 * snare (s) + 0.15 * std::exp (-h * 60.0) * (rnd.nextDouble() * 2.0 - 1.0);
        };
        auto stab = [] (double t) { double v = 0; for (double f : { 261.6, 329.6, 392.0, 493.9 }) v += std::sin (2.0 * juce::MathConstants<double>::pi * f * t); return 0.2 * v * std::exp (-t * 4.0); };
        auto bass = [] (double t) { const double ph = std::fmod (55.0 * t, 1.0); return 0.6 * (2.0 * ph - 1.0) * std::exp (-t * 2.0); };
        make ("kick", 0.5, kick, 0);
        make ("snare", 0.4, snare, 1);
        make ("drums 94", beat * 8.0, loop, 2);
        make ("stab", 1.0, stab, 5);
        make ("bass", 1.2, bass, 6);
        make ("loop 2", beat * 4.0, loop, 3);
        auto q = p.pad (2);
        q.loop = tf::sampler::Loop::partialLoop; q.loopPoint = q.frames / 4; q.start = q.frames / 40; q.sounding = tf::sampler::Sounding::toggle;
        p.setPad (2, q);
        p.editorPad = 2;
        p.pressKey ("FILTER");
        p.pressPad (2, 1.0f);
        p.setRibbon (0.62f, false);
        run (20);
        auto rs = p.recordSettings(); rs.toPad = 7; p.setRecordSettings (rs);
        const char* names[] { "play", "sample", "edit", "songs", "setup" };
        for (int t = 0; t < 5; ++t)
        {
            p.editorTab = t;
            std::unique_ptr<juce::AudioProcessorEditor> ed (p.createEditor());
            if (ed == nullptr) return false;
            run (8);
            const auto img = ed->createComponentSnapshot (ed->getLocalBounds(), true, 2.0f);
            juce::PNGImageFormat png;
            auto f = folder.getChildFile (juce::String ("sampler_") + names[t] + ".png");
            f.deleteFile();
            std::unique_ptr<juce::OutputStream> os (f.createOutputStream());
            if (os == nullptr || ! png.writeImageToStream (img, *os)) return false;
        }
        // the front panel in its other modes: SHIFT held, EDIT on the end point (the loop's tempo), DELETE's question
        auto& front = p.front();
        p.pressKey ("EXIT");
        run (4);
        auto snap = [&] (const char* name)
        {
            p.editorTab = 0;
            std::unique_ptr<juce::AudioProcessorEditor> ed (p.createEditor());
            if (ed == nullptr) return false;
            run (4);
            const auto img = ed->createComponentSnapshot (ed->getLocalBounds(), true, 2.0f);
            juce::PNGImageFormat png;
            auto f = folder.getChildFile (juce::String ("sampler_") + name + ".png");
            f.deleteFile();
            std::unique_ptr<juce::OutputStream> os (f.createOutputStream());
            return os != nullptr && png.writeImageToStream (img, *os);
        };
        front.key ("SHIFT", true);
        run (2);
        if (! snap ("shift")) return false;
        front.key ("SHIFT", true);   // (closing the window lets SHIFT go)
        front.pad (4, true);
        front.key ("SHIFT", false);
        front.pad (2, true);
        front.key ("ENTER", true);
        front.key ("FWD", true);
        if (! snap ("tempo")) return false;
        for (int k = 0; k < 20 && front.function() != tf::sampler::Front::del; ++k) front.key ("<", true);
        front.key ("ENTER", true);
        if (! snap ("delete")) return false;
        front.key ("EXIT", true);
        return true;
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

        // the built-in instruments' editors, in windows of their own like plug-ins'
        tf::ep::Processor::editorFactory = [] (tf::ep::Processor& p) -> juce::AudioProcessorEditor*
        {
            return new tf::InstrumentEditor (p, p, "TF ELECTRIC PIANO", [&p] (tf::InstrumentEditor::Say say)
            {
                auto e = std::make_unique<tf::ElectricPianoPanel> (p, p.editorTab);
                e->onMessage = say;
                return std::unique_ptr<juce::Component> (std::move (e));
            });
        };
        tf::organ::Processor::editorFactory = [] (tf::organ::Processor& p) -> juce::AudioProcessorEditor*
        {
            return new tf::InstrumentEditor (p, p, "TF TRANSISTOR", [&p] (tf::InstrumentEditor::Say say)
            {
                auto e = std::make_unique<tf::TransistorPanel> (p, p.editorTab);
                e->onMessage = say;
                return std::unique_ptr<juce::Component> (std::move (e));
            });
        };
        tf::n5::Processor::editorFactory = [] (tf::n5::Processor& p) -> juce::AudioProcessorEditor*
        {
            return new tf::InstrumentEditor (p, p, "TF N5", [&p] (tf::InstrumentEditor::Say say)
            {
                auto e = std::make_unique<tf::N5Panel> (p);
                e->onMessage = say;
                return std::unique_ptr<juce::Component> (std::move (e));
            }, 640);
        };
        tf::spring::Processor::editorFactory = [] (tf::spring::Processor& p) -> juce::AudioProcessorEditor* { return new tf::SpringEditor (p); };
        tf::sampler::Processor::editorFactory = [] (tf::sampler::Processor& p) -> juce::AudioProcessorEditor*
        {
            return new tf::InstrumentEditor (p, p, "TF PHRASE SAMPLER", [&p] (tf::InstrumentEditor::Say say)
            {
                auto e = std::make_unique<tf::PhraseSamplerPanel> (p, p.editorTab);
                e->onMessage = say;
                return std::unique_ptr<juce::Component> (std::move (e));
            }, 640);
        };

        // A tool for making artwork: --render-cassettes <folder> draws each formulation's cassette, at full size, as a PNG.
        if (commandLine.contains ("--render-n5"))
        {
            // for the manual: TF N5's window, tab by tab
            auto args = juce::StringArray::fromTokens (commandLine, true);
            auto folder = juce::File::getCurrentWorkingDirectory().getChildFile (args[args.indexOf ("--render-n5") + 1].unquoted());
            folder.createDirectory();
            bool ok = true;
            const char* names[] { "osc", "pitch", "vdf", "vda", "control", "waves", "effects", "combi", "arp" };
            tf::n5::Processor p;
            p.setRateAndBufferSizeDetails (48000.0, 512);
            p.prepareToPlay (48000.0, 512);
            auto& b = p.bank();
            // something to look at: a double program, a filter sweep, a combination
            b.programs[0].global[tf::n5::gOscMode] = 1;
            b.programs[0].layer[0][tf::n5::feAttackLevel] = 90; b.programs[0].layer[0][tf::n5::feBreak] = 40; b.programs[0].layer[0][tf::n5::feSustain] = 20;
            b.programs[0].layer[0][tf::n5::feInt] = 60; b.programs[0].layer[0][tf::n5::fCutoff] = 64; b.programs[0].layer[0][tf::n5::fColor] = 40;
            b.programs[0].layer[0][tf::n5::peStart] = 30; b.programs[0].layer[0][tf::n5::peAttackTime] = 30; b.programs[0].layer[0][tf::n5::peAttackLevel] = -20;
            b.programs[0].layer[0][tf::n5::peDecayTime] = 50; b.programs[0].layer[0][tf::n5::peInt] = 20;
            b.programs[0].layer[0][tf::n5::oWave] = tf::n5::drawn; b.programs[0].layer[0][tf::n5::oDrawn] = 3;
            p.setProgramName (0, "GLASSPAD");
            p.setProgramName (1, "HOLLOW SAW");
            b.combis[0].timbre[0][tf::n5::tProgram] = 0; b.combis[0].timbre[0][tf::n5::tNoteHi] = 71;
            b.combis[0].timbre[1][tf::n5::tProgram] = 1; b.combis[0].timbre[1][tf::n5::tNoteLo] = 60; b.combis[0].timbre[1][tf::n5::tPan] = 44;
            p.setCombiName (0, "SPLIT PAD");
            b.arp[tf::n5::aOn] = 1; b.arp[tf::n5::aType] = 6;
            p.editorDrawn = 2;
            p.changed();
            for (int t = 0; t < 9; ++t)
            {
                p.editorTab = t;
                b.combiMode = t == 7 ? 1 : 0;
                std::unique_ptr<juce::AudioProcessorEditor> ed (p.createEditor());
                if (ed == nullptr) { ok = false; break; }
                const auto img = ed->createComponentSnapshot (ed->getLocalBounds(), true, 2.0f);
                juce::PNGImageFormat png;
                auto f = folder.getChildFile (juce::String ("n5_") + names[t] + ".png");
                f.deleteFile();
                std::unique_ptr<juce::OutputStream> os (f.createOutputStream());
                ok = ok && os != nullptr && png.writeImageToStream (img, *os);
            }
            setApplicationReturnValue (ok ? 0 : 1);
            quit();
            return;
        }
        if (commandLine.contains ("--render-spring"))
        {
            // for the manual: the spring reverb's window, one picture a tank
            auto args = juce::StringArray::fromTokens (commandLine, true);
            auto folder = juce::File::getCurrentWorkingDirectory().getChildFile (args[args.indexOf ("--render-spring") + 1].unquoted());
            folder.createDirectory();
            bool ok = true;
            for (int t = 0; t < tf::spring::numTanks; ++t)
            {
                tf::spring::Processor p;
                p.setRateAndBufferSizeDetails (48000.0, 512);
                p.prepareToPlay (48000.0, 512);
                auto* tp = p.param ("tank");
                tp->setValueNotifyingHost (tp->convertTo0to1 ((float) t));
                std::unique_ptr<juce::AudioProcessorEditor> ed (p.createEditor());
                if (auto* se = dynamic_cast<tf::SpringEditor*> (ed.get())) se->refreshForSnapshot();
                const auto img = ed->createComponentSnapshot (ed->getLocalBounds(), true, 2.0f);
                juce::PNGImageFormat png;
                auto f = folder.getChildFile ("spring_" + juce::String (t) + ".png");
                f.deleteFile();
                std::unique_ptr<juce::OutputStream> os (f.createOutputStream());
                ok = ok && os != nullptr && png.writeImageToStream (img, *os);
            }
            setApplicationReturnValue (ok ? 0 : 1);
            quit();
            return;
        }
        if (commandLine.contains ("--render-sampler"))
        {
            auto args = juce::StringArray::fromTokens (commandLine, true);
            auto i = args.indexOf ("--render-sampler");
            auto folder = juce::File::getCurrentWorkingDirectory().getChildFile (args[i + 1].unquoted());
            setApplicationReturnValue (renderSampler (folder) ? 0 : 1);
            quit();
            return;
        }
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

        tf::Theme::apply (tf::Theme::indexOf (appProperties.getUserSettings()->getValue ("theme", "teal")));
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
