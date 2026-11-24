// Resizing the window, checked as a user does it: the editor on the desktop (a real window, real paints), the corner
// dragged with mouse events through the window, the host's way (setBoundsConstrained, as the VST3 wrapper calls it on
// onSize) and the size menu; on every tab, with LOOP and SLICE, with the tour, a tooltip and a menu open; with a host
// thread playing many notes and automating parameters meanwhile. Afterwards the controls must still answer the mouse,
// nothing that sounds may have changed and the audio must be the same bit for bit as without a window.
// Usage: ReanimaResizeCheck.exe <output folder> [rapido]

#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "Look.h"
#include "FxRack.h"
#include <atomic>
#include <cstdio>
#include <thread>
#define NOMINMAX
#include <windows.h>
#include <psapi.h>

namespace
{
int failures = 0;

void check (bool ok, const juce::String& what, const juce::String& detail = {})
{
    std::printf ("[%s] %s  %s\n", ok ? "PASS" : "FAIL", what.toRawUTF8(), detail.toRawUTF8());
    failures += ok ? 0 : 1;
}

void wait (int ms) { juce::MessageManager::getInstance()->runDispatchLoopUntil (ms); }

template <typename T>
T* findChild (juce::Component& root)
{
    if (auto* t = dynamic_cast<T*> (&root))
        return t;
    for (auto* c : root.getChildren())
        if (auto* t = findChild<T> (*c))
            return t;
    return nullptr;
}

juce::File writePluck (const juce::File& dir)
{
    constexpr double sr = 48000.0;
    juce::AudioBuffer<float> b (2, (int) (2.0 * sr));
    for (int i = 0; i < b.getNumSamples(); ++i)
    {
        const double t = i / sr;
        const double env = t < 0.003 ? t / 0.003 : 0.25 * std::exp (-(t - 0.003) * 1.2) + 0.75 * std::exp (-(t - 0.003) / 0.08);
        double v = 0.0;
        for (int h = 1; h <= 8; ++h)
            v += std::sin (2.0 * juce::MathConstants<double>::pi * 220.0 * h * t) / h;
        b.setSample (0, i, (float) (0.4 * env * v));
        b.setSample (1, i, (float) (0.4 * env * v));
    }
    auto file = dir.getChildFile ("Pluck A3.wav");
    file.deleteFile();
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
    juce::WavAudioFormat wav;
    if (auto w = wav.createWriterFor (stream, juce::AudioFormatWriterOptions().withSampleRate (sr).withNumChannels (2).withBitsPerSample (24)))
        w->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
    return file;
}

size_t privateBytes()
{
    PROCESS_MEMORY_COUNTERS_EX pmc {};
    GetProcessMemoryInfo (GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*) &pmc, sizeof (pmc));
    return pmc.PrivateUsage;
}

int guiObjects() { return (int) GetGuiResources (GetCurrentProcess(), GR_GDIOBJECTS) + (int) GetGuiResources (GetCurrentProcess(), GR_USEROBJECTS); }

void setParam (ReanimaProcessor& p, const char* id, float value)
{
    if (auto* prm = p.apvts.getParameter (id))
        prm->setValueNotifyingHost (prm->convertTo0to1 (value));
}

// Every parameter, as the host sees it.
std::vector<float> paramValues (ReanimaProcessor& p)
{
    std::vector<float> v;
    for (auto* prm : p.getParameters())
        v.push_back (prm->getValue());
    return v;
}

// The project's state without what belongs to the window (none expected, but this is what is compared).
juce::String stateText (ReanimaProcessor& p)
{
    juce::MemoryBlock mb;
    p.getStateInformation (mb);
    return juce::String ((juce::int64) mb.getSize()) + ":" + juce::String (juce::String::toHexString (mb.getData(), (int) mb.getSize(), 0).hashCode64());
}

// Any parameter that moves while nobody touches one: who and when.
struct ParamSpy : juce::AudioProcessorParameter::Listener
{
    ReanimaProcessor& proc;
    juce::String doing;
    bool on = false;
    int seen = 0;
    explicit ParamSpy (ReanimaProcessor& p) : proc (p) { for (auto* prm : proc.getParameters()) prm->addListener (this); }
    ~ParamSpy() override { for (auto* prm : proc.getParameters()) prm->removeListener (this); }
    void parameterValueChanged (int index, float v) override
    {
        if (on && seen++ < 12)
            std::printf ("  !! %s -> %.4f durante: %s\n", proc.getParameters()[index]->getName (40).toRawUTF8(), v, doing.toRawUTF8());
    }
    void parameterGestureChanged (int, bool) override {}
};

// How long the message thread went without a timer tick (a frozen window shows here).
struct Heartbeat : juce::Timer
{
    double last = 0.0, worst = 0.0;
    int ticks = 0;
    void start() { last = juce::Time::getMillisecondCounterHiRes(); worst = 0.0; ticks = 0; startTimer (10); }
    void timerCallback() override
    {
        const double now = juce::Time::getMillisecondCounterHiRes();
        worst = std::max (worst, now - last);
        last = now;
        ++ticks;
    }
};

// The host: plays blocks at the pace of real time on its own thread, with chords coming and going and two parameters
// automated, and measures each block.
struct HostThread
{
    ReanimaProcessor& proc;
    std::atomic<bool> run { false };
    std::atomic<int> blocks { 0 };
    std::atomic<double> worstMs { 0.0 }, totalMs { 0.0 };
    std::atomic<float> peak { 0.0f };
    std::thread thread;
    bool automate = true;

    explicit HostThread (ReanimaProcessor& p) : proc (p) {}
    void start()
    {
        run = true;
        thread = std::thread ([this]
        {
            juce::AudioBuffer<float> buf (2, 512);
            int b = 0;
            auto* cutoff = proc.apvts.getParameter ("fltFreq");
            auto* macro = proc.apvts.getParameter ("macro1");
            auto next = juce::Time::getMillisecondCounterHiRes();
            while (run)
            {
                juce::MidiBuffer midi;
                // 16-note chords every 8 blocks, released 4 blocks later: high polyphony all the time.
                if (b % 8 == 0)
                    for (int k = 0; k < 16; ++k)
                        midi.addEvent (juce::MidiMessage::noteOn (1, 36 + k * 3, (juce::uint8) (60 + k * 4)), k * 7);
                if (b % 8 == 4)
                    for (int k = 0; k < 16; ++k)
                        midi.addEvent (juce::MidiMessage::noteOff (1, 36 + k * 3), k * 5);
                if (automate && cutoff != nullptr && macro != nullptr)
                {
                    cutoff->setValueNotifyingHost (0.5f + 0.4f * std::sin ((float) b * 0.05f));
                    macro->setValueNotifyingHost (0.5f + 0.5f * std::sin ((float) b * 0.031f));
                }
                buf.clear();
                const double t0 = juce::Time::getMillisecondCounterHiRes();
                proc.processBlock (buf, midi);
                const double ms = juce::Time::getMillisecondCounterHiRes() - t0;
                worstMs = std::max (worstMs.load(), ms);
                totalMs = totalMs.load() + ms;
                peak = std::max (peak.load(), buf.getMagnitude (0, 512));
                ++blocks;
                ++b;
                next += 512.0 * 1000.0 / 48000.0;
                const double sleep = next - juce::Time::getMillisecondCounterHiRes();
                if (sleep > 0.5)
                    juce::Thread::sleep ((int) sleep);
            }
        });
    }
    void stop()
    {
        run = false;
        if (thread.joinable())
            thread.join();
    }
};

// The host's frame, as the VST3 wrapper makes it: a window on the desktop holding the editor, which follows the
// editor's size when the editor changes it (its own corner, its size menu).
struct Frame : juce::Component
{
    void childBoundsChanged (juce::Component* c) override
    {
        if (c != nullptr)
            setSize (c->getWidth(), c->getHeight());
    }
};

struct Window
{
    ReanimaProcessor& proc;
    Frame frame;
    std::unique_ptr<juce::AudioProcessorEditor> editor;
    juce::ComponentPeer* peer = nullptr;
    juce::TabbedComponent* tabs = nullptr;

    explicit Window (ReanimaProcessor& p) : proc (p) { open(); }
    ~Window() { close(); }

    void open()
    {
        editor.reset (proc.createEditor());
        frame.setOpaque (true);
        frame.addAndMakeVisible (*editor);
        editor->setTopLeftPosition (0, 0);
        frame.setBounds (40, 40, editor->getWidth(), editor->getHeight());
        if (! frame.isOnDesktop())
            frame.addToDesktop (0);
        frame.setVisible (true);
        peer = frame.getPeer();
        tabs = findChild<juce::TabbedComponent> (*editor);
        wait (100);
    }
    void close()
    {
        if (editor != nullptr)
        {
            juce::PopupMenu::dismissAllActiveMenus();
            proc.editorBeingDeleted (editor.get());
            editor = nullptr;
        }
    }

    void paintNow() { if (peer != nullptr) peer->performAnyPendingRepaintsNow(); }

    void mouse (juce::Point<float> p, bool down)
    {
        peer->handleMouseEvent (juce::MouseInputSource::InputSourceType::mouse, p,
                                down ? juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier) : juce::ModifierKeys(),
                                juce::MouseInputSource::defaultPressure, juce::MouseInputSource::defaultOrientation, juce::Time::currentTimeMillis());
    }

    // A mouse event for a component, from points in the window; sent to it straight (the real cursor stays out of it:
    // JUCE would otherwise mix in where the user's own mouse is).
    juce::MouseEvent event (juce::Component& c, juce::Point<float> p, juce::Point<float> down, bool button)
    {
        const auto now = juce::Time::getCurrentTime();
        return juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), c.getLocalPoint (&frame, p),
                                 button ? juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier) : juce::ModifierKeys(), 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                 &c, &c, now, c.getLocalPoint (&frame, down), now, 1, p != down);
    }

    // The corner dragged from where it is to a width, in a number of mouse moves; each move painted as the OS would.
    // Returns the time of the worst step (event + paint), in ms.
    double dragCornerTo (int width, int moves, double* total = nullptr)
    {
        const auto start = juce::Point<float> ((float) editor->getWidth() - 4.0f, (float) editor->getHeight() - 4.0f);
        const float k = (float) width / (float) editor->getWidth();
        const auto end = juce::Point<float> (start.x * k, start.y * k);
        juce::Component* corner = dynamic_cast<juce::ResizableCornerComponent*> (frame.getComponentAt (start.toInt()));
        if (corner == nullptr)
        {
            auto* at = frame.getComponentAt (start.toInt());
            std::printf ("  !! la esquina no está bajo el ratón: %s\n", at != nullptr ? typeid (*at).name() : "nada");
            ++failures;
            return 0.0;
        }
        corner->mouseDown (event (*corner, start, start, true));
        double worst = 0.0;
        for (int i = 1; i <= moves; ++i)
        {
            const double t0 = juce::Time::getMillisecondCounterHiRes();
            corner->mouseDrag (event (*corner, start + (end - start) * ((float) i / (float) moves), start, true));
            paintNow();
            const double ms = juce::Time::getMillisecondCounterHiRes() - t0;
            worst = std::max (worst, ms);
            if (total != nullptr)
                *total += ms;
            juce::MessageManager::getInstance()->runDispatchLoopUntil (1);
        }
        corner->mouseUp (event (*corner, end, start, false));
        return worst;
    }

    // What the VST3 wrapper does on the host's onSize: the host's frame, fitted by the editor's constrainer.
    double hostResize (int w, int h)
    {
        const double t0 = juce::Time::getMillisecondCounterHiRes();
        editor->setBoundsConstrained ({ 0, 0, w, h });
        paintNow();
        return juce::Time::getMillisecondCounterHiRes() - t0;
    }

    // A knob, slider or button still answers a real click or drag at the point where it is drawn now.
    bool controlAnswers (const juce::String& paramId)
    {
        auto* prm = proc.apvts.getParameter (paramId);
        if (prm == nullptr)
            return false;
        juce::Slider* slider = nullptr;
        std::function<void (juce::Component&)> find = [&] (juce::Component& c)
        {
            for (auto* child : c.getChildren())
            {
                if (slider != nullptr)
                    return;
                if (auto* s = dynamic_cast<juce::Slider*> (child))
                    if (s->isShowing() && s->getProperties()["paramId"].toString() == paramId)
                        slider = s;
                find (*child);
            }
        };
        find (*editor);
        if (slider == nullptr)
        {
            // Knobs are found by the parameter they are attached to (through their tooltip-free name if no property).
            std::function<void (juce::Component&)> any = [&] (juce::Component& c)
            {
                for (auto* child : c.getChildren())
                {
                    if (slider != nullptr)
                        return;
                    if (auto* knob = dynamic_cast<Knob*> (child))
                        if (knob->isShowing() && knob->parameterIndex() >= 0 && juce::String (P::defs()[(size_t) knob->parameterIndex()].id) == paramId)
                            slider = &knob->slider;
                    any (*child);
                }
            };
            any (*editor);
        }
        if (slider == nullptr)
            return false;
        const float before = prm->getValue();
        const auto centre = frame.getLocalPoint (slider, slider->getLocalBounds().getCentre()).toFloat();
        // What the mouse finds there must be the knob (no invisible overlay, no stale transform), and nothing modal
        // may be holding the window.
        auto* hit = frame.getComponentAt (centre.toInt());
        const bool reaches = (hit == slider || (hit != nullptr && slider->isParentOf (hit))) && slider->isEnabled()
                          && ! slider->isCurrentlyBlockedByAnotherModalComponent();
        if (! reaches)
            std::printf ("  !! en el centro de %s está %s\n", paramId.toRawUTF8(), hit != nullptr ? typeid (*hit).name() : "nada");
        auto& target = hit != nullptr ? *hit : static_cast<juce::Component&> (*slider);
        target.mouseDown (event (target, centre, centre, true));
        for (int i = 1; i <= 10; ++i)
            target.mouseDrag (event (target, centre - juce::Point<float> (0.0f, (float) i * 6.0f), centre, true));
        target.mouseUp (event (target, centre - juce::Point<float> (0.0f, 60.0f), centre, false));
        wait (30);
        const float after = prm->getValue();
        prm->setValueNotifyingHost (before);
        wait (30);
        return reaches && std::abs (after - before) > 1.0e-4f;
    }
};

// Renders a fixed scene of notes on its own, block by block; between blocks the window may be resized.
juce::AudioBuffer<float> render (ReanimaProcessor& proc, int blocks, const std::function<void (int)>& between)
{
    juce::AudioBuffer<float> out (2, blocks * 512), buf (2, 512);
    for (int b = 0; b < blocks; ++b)
    {
        juce::MidiBuffer midi;
        if (b % 40 == 0)
            for (int k = 0; k < 12; ++k)
                midi.addEvent (juce::MidiMessage::noteOn (1, 40 + k * 4, (juce::uint8) (50 + k * 6)), k * 11);
        if (b % 40 == 25)
            for (int k = 0; k < 12; ++k)
                midi.addEvent (juce::MidiMessage::noteOff (1, 40 + k * 4), k * 9);
        buf.clear();
        proc.processBlock (buf, midi);
        for (int c = 0; c < 2; ++c)
            out.copyFrom (c, b * 512, buf, c, 0, 512);
        if (between)
            between (b);
    }
    return out;
}

void prepare (ReanimaProcessor& p, const juce::File& sample)
{
    ReanimaProcessor::offerDemo = false;
    p.prepareToPlay (48000.0, 512);
    p.loadSampleNow (sample);
    for (int k = 0; k < 600 && p.isLoading(); ++k)
    {
        juce::Thread::sleep (10);
        p.serviceMessageThread();
    }
    p.serviceMessageThread();
    setParam (p, "fltOn", 1.0f);
    setParam (p, "unison", 4.0f);
    p.setFxSlotType (0, Fx::Reverb);
    p.setFxSlotType (1, Fx::Delay);
    p.serviceMessageThread();
}
} // namespace

int main (int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI init;
    std::setvbuf (stdout, nullptr, _IONBF, 0);
    const juce::File out (argc > 1 ? juce::String (argv[1]) : juce::File::getCurrentWorkingDirectory().getFullPathName());
    out.createDirectory();
    const bool quick = argc > 2 && juce::String (argv[2]) == "rapido";
    ReanimaEditor::offerWelcome = false;
    const auto sample = writePluck (out);
    const int W = ReanimaEditor::designW;

    ReanimaProcessor proc;
    prepare (proc, sample);

    // 1. Cost of a drag on every tab: slow (many small steps) and fast (big jumps), from the smallest to the largest.
    {
        Window w (proc);
        const auto mem0 = privateBytes();
        const int gui0 = guiObjects();
        Heartbeat beat;
        beat.start();
        std::printf ("\n== arrastre de la esquina, por pestaña (ms por paso: evento + pintado) ==\n");
        for (int t = 0; t < w.tabs->getNumTabs(); ++t)
        {
            w.tabs->setCurrentTabIndex (t);
            wait (60);
            w.hostResize (W, ReanimaEditor::designH);
            double total = 0.0;
            const int steps = quick ? 40 : 160;
            const double slowUp = w.dragCornerTo ((int) (W * 1.6), steps, &total);
            const double slowDown = w.dragCornerTo ((int) (W * 0.6), steps, &total);
            double fast = 0.0;
            for (int j = 0; j < 6; ++j)
                fast = std::max (fast, w.dragCornerTo (j % 2 == 0 ? (int) (W * 2.0) : (int) (W * 0.6), 2));
            std::printf ("  %-14s lento: media %.1f, peor %.1f   rápido (saltos 60-200 %%): peor %.1f\n",
                         w.tabs->getTabNames()[t].toRawUTF8(), total / (2.0 * steps), std::max (slowUp, slowDown), fast);
            // A full paint of the tab, done here (the window's own paint may happen later, on the screen's refresh).
            double paint100 = 0.0, paint200 = 0.0;
            for (const float s : { 1.0f, 2.0f })
            {
                w.hostResize ((int) (W * s), (int) (ReanimaEditor::designH * s));
                const double t0 = juce::Time::getMillisecondCounterHiRes();
                juce::Image img (juce::Image::ARGB, w.editor->getWidth(), w.editor->getHeight(), true, juce::SoftwareImageType());
                juce::Graphics g (img);
                w.editor->paintEntireComponent (g, true);
                (s == 1.0f ? paint100 : paint200) = juce::Time::getMillisecondCounterHiRes() - t0;
            }
            std::printf ("  %-14s pintado completo (software): %.1f ms al 100 %%, %.1f ms al 200 %%\n", "", paint100, paint200);
            check (std::max (slowUp, slowDown) < 250.0 && fast < 600.0, "arrastre fluido en " + w.tabs->getTabNames()[t]);
        }
        wait (700);       // the size is remembered once the drag stops
        check (beat.worst < 1500.0, "la ventana nunca se queda congelada", juce::String (beat.worst, 0) + " ms sin latido como mucho");
        // The first sizes fill caches (glyphs at each size, the waveform's image): after them, a thousand more steps
        // over the same range must leave the memory where it was.
        const auto warm = privateBytes();
        const int guiWarm = guiObjects();
        for (int j = 0; j < (quick ? 8 : 24); ++j)
            w.dragCornerTo (j % 2 == 0 ? (int) (W * 1.5) : (int) (W * 0.7), 40);
        wait (700);
        const double first = ((double) warm - (double) mem0) / 1048576.0, grown = ((double) privateBytes() - (double) warm) / 1048576.0;
        std::printf ("  memoria privada: %+.1f MB en los primeros tamaños (cachés), %+.1f MB en %d pasos más; GDI/USER %d -> %d -> %d\n",
                     first, grown, quick ? 320 : 960, gui0, guiWarm, guiObjects());
        check (grown < 8.0 && guiObjects() - guiWarm < 10, "sin fugas al redimensionar", juce::String (grown, 1) + " MB");
    }

    // 2. The controls answer straight after every kind of resize, on every tab, with LOOP and SLICE.
    {
        Window w (proc);
        std::printf ("\n== controles tras redimensionar ==\n");
        struct Case { int tab; const char* param; };
        const Case cases[] = { { 0, "volume" }, { 0, "ampA" }, { 2, "fltFreq" }, { 3, "scSens" }, { 4, "volume" }, { 5, "volume" } };
        for (const float s : { 0.6f, 0.83f, 1.0f, 1.37f, 2.0f })
        {
            w.dragCornerTo ((int) (W * s), 8);
            for (const auto& c : cases)
            {
                w.tabs->setCurrentTabIndex (c.tab);
                wait (40);
                check (w.controlAnswers (c.param), juce::String ("responde ") + c.param + " en " + w.tabs->getTabNames()[c.tab] + " al "
                                                     + juce::String (juce::roundToInt (100.0f * w.editor->getWidth() / W)) + " %");
            }
        }
        // the host's way, with fractional and odd sizes it may hand over
        for (const auto& [hw, hh] : { std::pair { 1001, 700 }, { 1500, 1200 }, { 700, 300 }, { 2600, 1700 }, { 1180, 760 } })
        {
            w.hostResize (hw, hh);
            w.tabs->setCurrentTabIndex (0);
            wait (40);
            check (w.controlAnswers ("ampD"), "responde tras el tamaño del host " + juce::String (hw) + "x" + juce::String (hh),
                   juce::String (w.editor->getWidth()) + "x" + juce::String (w.editor->getHeight()));
        }
        // A host that ignores the proportion: the whole design must still be inside the window (the size button, the
        // last thing at the bottom right, included) and answer.
        for (const auto& [hw, hh] : { std::pair { 1400, 600 }, { 800, 900 }, { 1181, 759 } })
        {
            w.editor->setSize (hw, hh);
            wait (40);
            juce::TextButton* sizeButton = nullptr;
            std::function<void (juce::Component&)> find = [&] (juce::Component& c)
            {
                for (auto* child : c.getChildren())
                {
                    if (auto* b = dynamic_cast<juce::TextButton*> (child))
                        if (b->getButtonText().endsWith (" %"))
                            sizeButton = b;
                    find (*child);
                }
            };
            find (*w.editor);
            const bool inside = sizeButton != nullptr
                             && w.editor->getLocalBounds().contains (w.editor->getLocalArea (sizeButton, sizeButton->getLocalBounds()));
            check (inside && w.controlAnswers ("ampD"), "con un tamaño sin proporción todo queda dentro y responde",
                   juce::String (hw) + "x" + juce::String (hh));
        }
        w.hostResize (W, ReanimaEditor::designH);
        // LOOP and SLICE, every tab
        for (const float kind : { 0.0f, 1.0f })
        {
            setParam (proc, "playMode", 1.0f);
            setParam (proc, "loopKind", kind == 0.0f ? 0.0f : (float) P::LoopSlice);
            wait (200);
            for (int t = 0; t < w.tabs->getNumTabs(); ++t)
            {
                w.tabs->setCurrentTabIndex (t);
                w.dragCornerTo ((int) (W * (t % 2 == 0 ? 1.3f : 0.75f)), 10);
            }
            w.tabs->setCurrentTabIndex (0);
            wait (60);
            check (w.controlAnswers ("volume"), juce::String ("responde con ") + (kind == 0.0f ? "LOOP" : "SLICE"));
        }
        setParam (proc, "playMode", 0.0f);
        wait (100);

        // The tour, a tooltip and a menu open while the window changes size.
        auto& ed = dynamic_cast<ReanimaEditor&> (*w.editor);
        ed.startQuickGuide();
        wait (100);
        for (int k = 0; k < 6; ++k)
            w.dragCornerTo ((int) (W * (k % 2 == 0 ? 1.4f : 0.7f)), 6);
        check (ed.getGuide().isRunning(), "el tour sigue abierto tras redimensionar");
        ed.getGuide().setVisible (false);
        if (auto* tip = findChild<juce::TooltipWindow> (*w.editor))
        {
            tip->displayTip (w.editor->getScreenBounds().getCentre(), "prueba");
            w.dragCornerTo ((int) (W * 1.2f), 6);
            tip->hideTip();
        }
        juce::PopupMenu menu;
        menu.addItem (1, "uno");
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (w.editor.get()));
        wait (50);
        w.dragCornerTo ((int) (W * 0.9f), 6);
        juce::PopupMenu::dismissAllActiveMenus();
        wait (80);
        w.tabs->setCurrentTabIndex (0);
        wait (60);
        check (w.controlAnswers ("volume"), "responde tras redimensionar con el tour, un tooltip y un menú abiertos");
    }

    // 3. Resizing while the host plays: 16-note chords, automation, every tab; the audio thread is never held up.
    {
        Window w (proc);
        HostThread host (proc);
        Heartbeat beat;
        beat.start();
        host.start();
        wait (300);
        const int before = host.blocks.load();
        const double t0 = juce::Time::getMillisecondCounterHiRes();
        for (int j = 0; j < (quick ? 12 : 40); ++j)
        {
            w.tabs->setCurrentTabIndex (j % w.tabs->getNumTabs());
            w.dragCornerTo ((int) (W * (j % 2 == 0 ? 1.8f : 0.65f)), j % 3 == 0 ? 2 : 20);
        }
        const double seconds = (juce::Time::getMillisecondCounterHiRes() - t0) / 1000.0;
        wait (200);
        host.stop();
        const int played = host.blocks.load() - before;
        const double expected = seconds * 48000.0 / 512.0;
        std::printf ("\n== redimensionar mientras suena ==\n  %d bloques en %.1f s (%.0f esperados), bloque peor %.2f ms (límite 10.67), medio %.3f ms, pico %.2f\n",
                     played, seconds, expected, host.worstMs.load(), host.totalMs.load() / std::max (1, host.blocks.load()), host.peak.load());
        check (played > expected * 0.9, "el audio no se para mientras se redimensiona");
        check (host.worstMs.load() < 10.67, "ningún bloque de audio llega tarde por la ventana", juce::String (host.worstMs.load(), 2) + " ms");
        check (beat.worst < 1500.0, "la ventana sigue viva con audio y automatización", juce::String (beat.worst, 0) + " ms sin latido");
        w.tabs->setCurrentTabIndex (0);
        wait (60);
        check (w.controlAnswers ("ampS"), "los controles responden justo después");
    }

    // 4. Nothing that sounds changes: every parameter and the project's state, before and after (no automation now).
    {
        Window w (proc);
        wait (200);
        const auto params = paramValues (proc);
        const auto state = stateText (proc);
        ParamSpy spy (proc);
        spy.on = true;
        for (int j = 0; j < 20; ++j)
        {
            spy.doing = "pestaña " + juce::String (j % w.tabs->getNumTabs());
            w.tabs->setCurrentTabIndex (j % w.tabs->getNumTabs());
            spy.doing = "esquina " + juce::String (j);
            w.dragCornerTo ((int) (W * (j % 2 == 0 ? 1.9f : 0.62f)), 8);
            spy.doing = "host " + juce::String (j);
            w.hostResize (900 + j * 37, 600 + j * 23);
        }
        spy.doing = "espera";
        wait (800);
        const auto paramsAfter = paramValues (proc);
        int changed = 0;
        for (size_t i = 0; i < params.size(); ++i)
            if (params[i] != paramsAfter[i])
            {
                ++changed;
                std::printf ("  cambió: %s %.6f -> %.6f\n", proc.getParameters()[(int) i]->getName (40).toRawUTF8(), params[i], paramsAfter[i]);
            }
        check (changed == 0, "redimensionar no cambia ningún parámetro", juce::String ((int) params.size()) + " comparados");
        check (stateText (proc) == state, "el estado del proyecto es idéntico byte a byte");
        // close and open the window again, twenty times
        const auto mem0 = privateBytes();
        const int gui0 = guiObjects();
        double worstOpen = 0.0;
        for (int k = 0; k < (quick ? 6 : 20); ++k)
        {
            w.close();
            const double t0 = juce::Time::getMillisecondCounterHiRes();
            w.open();
            worstOpen = std::max (worstOpen, juce::Time::getMillisecondCounterHiRes() - t0);
            w.dragCornerTo ((int) (W * (k % 2 == 0 ? 1.3f : 0.8f)), 5);
        }
        wait (800);
        const double grown = ((double) privateBytes() - (double) mem0) / 1048576.0;
        std::printf ("  cerrar y abrir: peor %.0f ms; memoria %+.1f MB; GDI/USER %d -> %d\n", worstOpen, grown, gui0, guiObjects());
        check (grown < 60.0 && guiObjects() - gui0 < 60, "cerrar y abrir el editor no deja fugas", juce::String (grown, 1) + " MB");
        check (paramValues (proc) == params && stateText (proc) == state, "cerrar y abrir tampoco cambia nada");
    }

    // 5. The audio, bit for bit: the same scene rendered with no window and with one being resized between blocks.
    {
        const int blocks = quick ? 200 : 600;
        ReanimaProcessor a, b, c;
        prepare (a, sample);
        prepare (b, sample);
        prepare (c, sample);
        const auto ref = render (a, blocks, {});
        const auto again = render (c, blocks, {});
        Window w (b);
        const auto resized = render (b, blocks, [&] (int k)
        {
            if (k % 3 == 0)
            {
                w.tabs->setCurrentTabIndex ((k / 3) % w.tabs->getNumTabs());
                w.dragCornerTo ((int) (W * (0.6f + 0.07f * (float) (k % 21))), 2);
            }
            if (k % 10 == 0)
                wait (5);
        });
        const auto diff = [&] (const juce::AudioBuffer<float>& x, const juce::AudioBuffer<float>& y)
        {
            float d = 0.0f;
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < x.getNumSamples(); ++i)
                    d = std::max (d, std::abs (x.getSample (ch, i) - y.getSample (ch, i)));
            return d;
        };
        const float determinism = diff (ref, again), withWindow = diff (ref, resized);
        std::printf ("\n== render ==\n  pico %.3f; sin ventana dos veces: dif %.3g; con ventana redimensionándose: dif %.3g\n", ref.getMagnitude (0, ref.getNumSamples()), determinism, withWindow);
        check (withWindow <= determinism, "el audio es idéntico con y sin redimensionar", juce::String (withWindow, 9));
    }

    std::printf ("\n%s: %d fallos\n", failures == 0 ? "REDIMENSIONADO OK" : "REDIMENSIONADO CON FALLOS", failures);
    return failures == 0 ? 0 : 1;
}
