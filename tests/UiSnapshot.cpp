// Opens the plugin editor offscreen with a synthetic sample loaded, saves a PNG of each
// tab and checks what only exists with a real processor and editor: the mouse on the
// waveform, the tooltips, saving and loading state, background loads.
// Usage: ReanimaSnapshot.exe <output folder>

#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "EditPage.h"
#include "MasterStage.h"
#include "FxRack.h"
#include "FactoryPresets.h"
#include <cstdio>
#include <functional>
#include <set>

namespace
{
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

int failures = 0;

void check (bool ok, const juce::String& what, const juce::String& detail = {})
{
    std::printf ("[%s] %s  %s\n", ok ? "PASS" : "FAIL", what.toRawUTF8(), detail.toRawUTF8());
    failures += ok ? 0 : 1;
}

void wait (int ms) { juce::MessageManager::getInstance()->runDispatchLoopUntil (ms); }

template <typename Condition>
bool waitFor (Condition condition, int ms = 15000)
{
    for (int elapsed = 0; elapsed < ms && ! condition(); elapsed += 20)
        wait (20);
    return condition();
}

juce::MouseEvent mouseAt (juce::Component& c, juce::Point<float> pos, juce::Point<float> down, juce::ModifierKeys mods, int clicks = 1)
{
    const auto now = juce::Time::getCurrentTime();
    return juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), pos, mods, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                             &c, &c, now, down, now, clicks, pos != down);
}

// Controls the mouse can reach that would show no explanation.
void listUntipped (juce::Component& root, juce::StringArray& missing)
{
    for (auto* c : root.getChildren())
    {
        const bool control = dynamic_cast<juce::Slider*> (c) != nullptr || dynamic_cast<juce::ComboBox*> (c) != nullptr
                          || dynamic_cast<juce::Button*> (c) != nullptr;
        if (control && c->isVisible())
            if (auto* tip = dynamic_cast<juce::TooltipClient*> (c))
                if (tip->getTooltip().isEmpty())
                    missing.addIfNotAlreadyThere (c->getName().isNotEmpty() ? c->getName() : juce::String (typeid (*c).name()));
        listUntipped (*c, missing);
    }
}

void checkWaveform (ReanimaProcessor& proc, juce::AudioProcessorEditor& editor)
{
    auto* view = findChild<WaveformView> (editor);
    check (view != nullptr, "la forma de onda existe");
    const auto sample = proc.getSample();
    const float onsetValue = (float) (sample->onset / sample->length);
    const auto noKeys = juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier);

    // The area of the waveform, as WaveformView draws it.
    const auto area = view->getLocalBounds().toFloat().withTrimmedTop (26.0f).withTrimmedBottom (18.0f).reduced (10.0f, 4.0f);
    const float y = area.getCentreY();

    // Grab the marker where it is, away from the envelope handles, and drag it to the right.
    const float start = area.getX();
    const juce::Point<float> grab (start + 1.0f, y), target (start + 200.0f, y);
    view->mouseDown (mouseAt (*view, grab, grab, noKeys));
    view->mouseDrag (mouseAt (*view, target, grab, noKeys));
    view->mouseUp (mouseAt (*view, target, grab, juce::ModifierKeys()));
    const float dragged = proc.paramValue (P::StartPos);
    check (dragged > 0.02f, "arrastrar INICIO mueve el punto de inicio", juce::String (dragged * 100.0f, 2) + " %");

    // Double click on empty waveform places it; on the marker it goes back to the detected onset.
    const juce::Point<float> elsewhere (start + 100.0f, y);
    view->mouseDoubleClick (mouseAt (*view, elsewhere, elsewhere, noKeys, 2));
    const float placed = proc.paramValue (P::StartPos);
    check (placed > 0.0f && placed < dragged, "doble clic pone ahí el inicio", juce::String (placed * 100.0f, 2) + " %");

    view->mouseDoubleClick (mouseAt (*view, elsewhere, elsewhere, noKeys, 2));
    check (std::abs (proc.paramValue (P::StartPos) - onsetValue) < 1.0e-4f, "doble clic sobre INICIO vuelve al detectado",
           juce::String (proc.paramValue (P::StartPos) * 100.0f, 2) + " %");

    // The wheel zooms around the mouse; the start still lands where it is dropped.
    juce::MouseWheelDetails wheel {};
    wheel.deltaY = 0.6f;
    const juce::Point<float> near (start, y);      // zooming around the line keeps it in place
    view->mouseWheelMove (mouseAt (*view, near, near, juce::ModifierKeys()), wheel);
    check (view->getZoom() > 3.0f, "la rueda acerca la forma de onda", "x" + juce::String (view->getZoom(), 1));

    view->mouseDown (mouseAt (*view, grab, grab, noKeys));
    view->mouseDrag (mouseAt (*view, target, grab, noKeys));
    view->mouseUp (mouseAt (*view, target, grab, juce::ModifierKeys()));
    const float zoomed = proc.paramValue (P::StartPos);
    check (zoomed > onsetValue && zoomed < dragged * 0.5f, "con zoom, INICIO se mueve con más precisión",
           juce::String (zoomed * 100.0f, 3) + " % frente a " + juce::String (dragged * 100.0f, 2) + " %");

    view->zoomAround (0.0, 1.0f);
    view->mouseDoubleClick (mouseAt (*view, grab, grab, noKeys, 2));
}

// Controls that stick out of their panel or sit on top of each other.
void listLayoutFaults (juce::Component& root, juce::StringArray& faults)
{
    juce::Array<juce::Component*> shown;
    for (auto* c : root.getChildren())
    {
        if (! c->isVisible())
            continue;
        if (dynamic_cast<Knob*> (c) != nullptr || dynamic_cast<Choice*> (c) != nullptr || dynamic_cast<Toggle*> (c) != nullptr)
        {
            if (c->getWidth() < 30 || c->getHeight() < 20 || ! root.getLocalBounds().contains (c->getBounds()))
                faults.add ("tamaño " + c->getBounds().toString());
            for (auto* other : shown)
                if (c->getBounds().intersects (other->getBounds()))
                    faults.add ("solapados " + c->getBounds().toString() + " y " + other->getBounds().toString());
            shown.add (c);
        }
        listLayoutFaults (*c, faults);
    }
}

void checkLayout (ReanimaProcessor& proc, juce::AudioProcessorEditor& editor, juce::TabbedComponent& tabs)
{
    auto* engine = proc.apvts.getParameter ("engine");
    juce::StringArray faults;
    for (const float value : { 1.0f, 0.0f })
    {
        engine->setValueNotifyingHost (value);
        wait (100);
        for (int t = 0; t < tabs.getNumTabs(); ++t)
        {
            tabs.setCurrentTabIndex (t);
            wait (50);
            listLayoutFaults (editor, faults);
        }
    }
    engine->setValueNotifyingHost (1.0f);
    tabs.setCurrentTabIndex (0);
    wait (100);
    check (faults.isEmpty(), "ningún control se sale de su panel ni se solapa", faults.joinIntoString (", "));
}

void checkMeter (ReanimaProcessor& proc, juce::AudioProcessorEditor& editor)
{
    auto* meter = findChild<OutputMeter> (editor);
    check (meter != nullptr, "el vúmetro existe");

    juce::AudioBuffer<float> buffer (2, 512);
    float peak = 0.0f;
    proc.takeMeterReading();
    for (int b = 0; b < 40; ++b)
    {
        juce::MidiBuffer midi;
        if (b == 0)
            midi.addEvent (juce::MidiMessage::noteOn (1, 57, 1.0f), 0);
        proc.processBlock (buffer, midi);
        peak = std::max (peak, buffer.getMagnitude (0, 0, 512));     // left channel only
    }

    const auto reading = proc.takeMeterReading();
    // The meter reads the signal before the output stage; under its 0 dB the stage is clean and gives it out
    // 4 dB lower (MasterStage::knee), with the room for the 6 dB of drive.
    check (std::abs (reading.peak[0] * MasterStage::knee() - peak) < 0.02f * peak, "el vúmetro lee el nivel antes de la etapa de salida (la parte limpia sale 4 dB más abajo)",
           juce::String (reading.peak[0], 4) + " x " + juce::String (MasterStage::knee(), 3) + " / " + juce::String (peak, 4));
    check (reading.meanSquare[0] > 0.0f && std::sqrt (reading.meanSquare[0]) <= reading.peak[0], "el nivel medio está por debajo del pico");
    check (proc.takeMeterReading().peak[0] == 0.0f, "una lectura vacía la medida");

    wait (1500);
    const float idle = meter->vuDb (0);
    check (idle <= -59.0f, "sin audio el vúmetro baja", juce::String (idle, 1) + " dB");
}

void checkTooltips (ReanimaProcessor& proc, juce::AudioProcessorEditor& editor, juce::TabbedComponent& tabs)
{
    juce::StringArray missing;
    auto* engine = proc.apvts.getParameter ("engine");
    for (const float value : { 1.0f, 0.0f })
    {
        engine->setValueNotifyingHost (value);
        wait (100);
        for (int t = 0; t < tabs.getNumTabs(); ++t)
        {
            tabs.setCurrentTabIndex (t);
            listUntipped (editor, missing);
        }
    }
    engine->setValueNotifyingHost (1.0f);

    // Every effect shows different controls (the FX page is there while its tab is open).
    tabs.setCurrentTabIndex (4);
    wait (100);
    auto* page = findChild<FxPage> (editor);
    if (page == nullptr)
    {
        check (false, "la página FX existe");
        return;
    }
    for (int type = 1; type < Fx::NumTypes; ++type)
    {
        proc.setFxSlotType (2, type);
        page->select (2);
        wait (150);
        listUntipped (*page, missing);
    }
    proc.setFxSlotType (2, Fx::Empty);
    check (missing.isEmpty(), "todos los controles tienen tooltip", missing.joinIntoString (", "));
}

void checkState (ReanimaProcessor& proc, const juce::File& sampleFile)
{
    juce::MemoryBlock state;
    auto t0 = juce::Time::getMillisecondCounterHiRes();
    proc.getStateInformation (state);
    const double saveMs = juce::Time::getMillisecondCounterHiRes() - t0;

    ReanimaProcessor second;
    second.prepareToPlay (96000.0, 256);
    t0 = juce::Time::getMillisecondCounterHiRes();
    second.setStateInformation (state.getData(), (int) state.getSize());
    const double openMs = juce::Time::getMillisecondCounterHiRes() - t0;
    check (waitFor ([&] { return second.getSample() != nullptr && ! second.isLoading(); }), "el estado guardado recupera el sample");
    const double readyMs = juce::Time::getMillisecondCounterHiRes() - t0;
    check (saveMs < 50.0 && openMs < 50.0, "guardar y abrir no bloquean",
           juce::String (saveMs, 1) + " ms al guardar, " + juce::String (openMs, 1) + " ms al abrir, sonando en " + juce::String (readyMs, 0) + " ms ("
           + juce::String ((int) state.getSize() / 1024) + " KB)");

    juce::String different;
    for (int i = 0; i < P::Count; ++i)
        if (std::abs (proc.paramValue (i) - second.paramValue (i)) > 1.0e-6f)
            different << P::defs()[(size_t) i].id << " ";
    check (different.isEmpty(), "el estado guardado recupera los parámetros", different);

    // A project from an earlier version: "Inicio" counted from the onset to the end of the sound.
    {
        auto xml = juce::AudioProcessor::getXmlFromBinary (state.getData(), (int) state.getSize());
        xml->removeAttribute ("stateVersion");
        for (auto* e : xml->getChildIterator())
            if (e->getStringAttribute ("id") == "startPos")
                e->setAttribute ("value", 0.5);
        juce::MemoryBlock old;
        juce::AudioProcessor::copyXmlToBinary (*xml, old);

        ReanimaProcessor third;
        third.prepareToPlay (44100.0, 512);
        third.setStateInformation (old.getData(), (int) old.getSize());
        check (waitFor ([&] { return third.getSample() != nullptr && ! third.isLoading(); }), "abre un proyecto antiguo");
        const auto s = third.getSample();
        const float expected = (float) ((s->onset + 0.5 * (s->usableEnd - s->onset)) / s->length);
        check (std::abs (third.paramValue (P::StartPos) - expected) < 1.0e-3f, "convierte el inicio de un proyecto antiguo",
               juce::String (third.paramValue (P::StartPos), 3) + " (esperado " + juce::String (expected, 3) + ")");
    }

    // Up to 0.5 the engines had other letters: each old project keeps the engine it had.
    {
        const int oldOrder[] = { P::EngineReenvelope, P::EngineHybrid, P::EngineResynth, P::EngineRebuild, P::EngineGranular };
        bool all = true;
        for (int old = 0; old < 5; ++old)
        {
            auto xml = juce::AudioProcessor::getXmlFromBinary (state.getData(), (int) state.getSize());
            xml->setAttribute ("stateVersion", 2);
            for (auto* e : xml->getChildIterator())
                if (e->getStringAttribute ("id") == "engine")
                    e->setAttribute ("value", old);
            juce::MemoryBlock block;
            juce::AudioProcessor::copyXmlToBinary (*xml, block);
            ReanimaProcessor fourth;
            fourth.setStateInformation (block.getData(), (int) block.getSize());
            all = all && (int) fourth.paramValue (P::Engine) == oldOrder[old];
        }
        check (all, "los proyectos de la 0.5 conservan su motor con las letras nuevas");
    }

    // On another computer the file is not there: the sample inside the project plays the same.
    {
        auto xml = juce::AudioProcessor::getXmlFromBinary (state.getData(), (int) state.getSize());
        check (xml->getStringAttribute ("sampleAudio").length() > 1000, "el proyecto lleva el sample dentro",
               juce::String (xml->getStringAttribute ("sampleAudio").length() / 1024) + " KB");
        xml->setAttribute ("samplePath", sampleFile.getSiblingFile ("en otro ordenador.wav").getFullPathName());
        juce::MemoryBlock block;
        juce::AudioProcessor::copyXmlToBinary (*xml, block);
        ReanimaProcessor elsewhere;
        elsewhere.prepareToPlay (44100.0, 512);
        elsewhere.setStateInformation (block.getData(), (int) block.getSize());
        check (waitFor ([&] { return elsewhere.getSample() != nullptr && ! elsewhere.isLoading(); }) && ! elsewhere.sampleIsMissing()
                   && elsewhere.getSample()->length == proc.getSample()->length && elsewhere.getSample()->name == proc.getSample()->name,
               "sin el archivo original, el proyecto abre con su sample");
    }

    // An old project without the sample inside, whose file was moved: keeps the path, says so, and survives.
    {
        const auto moved = sampleFile.getSiblingFile ("no existe.wav");
        auto xml = juce::AudioProcessor::getXmlFromBinary (state.getData(), (int) state.getSize());
        xml->removeAttribute ("sampleAudio");
        xml->setAttribute ("samplePath", moved.getFullPathName());
        juce::MemoryBlock block;
        juce::AudioProcessor::copyXmlToBinary (*xml, block);

        ReanimaProcessor fourth;
        fourth.prepareToPlay (48000.0, 512);
        fourth.setStateInformation (block.getData(), (int) block.getSize());
        check (waitFor ([&] { return ! fourth.isLoading(); }), "un sample que falta no bloquea el proyecto");
        check (fourth.sampleIsMissing(), "avisa de que falta el archivo", fourth.sampleFileName());

        juce::MemoryBlock again;
        fourth.getStateInformation (again);
        auto saved = juce::AudioProcessor::getXmlFromBinary (again.getData(), (int) again.getSize());
        check (saved->getStringAttribute ("samplePath") == moved.getFullPathName(), "al volver a guardar conserva la ruta");
    }
}

void checkPresets (ReanimaProcessor& proc, const juce::File& folder)
{
    folder.deleteRecursively();
    proc.setPresetFolder (folder);
    auto* gain = proc.apvts.getParameter ("sampleGain");
    gain->setValueNotifyingHost (gain->convertTo0to1 (6.0f));
    check (proc.savePreset ("Prueba / 1"), "guarda un preset");
    check (proc.getPresets().size() == 1 && proc.getPresetName() == "Prueba 1", "aparece en la lista con un nombre válido", proc.getPresetName());

    gain->setValueNotifyingHost (gain->convertTo0to1 (-12.0f));
    check (proc.loadPreset (proc.getPresets()[0]), "abre el preset");
    check (std::abs (proc.paramValue (P::SampleGain) - 6.0f) < 0.01f, "el preset recupera los mandos", juce::String (proc.paramValue (P::SampleGain), 2));
    check (waitFor ([&] { return proc.getSample() != nullptr && ! proc.isLoading(); }), "el preset recupera el sample");
    gain->setValueNotifyingHost (gain->convertTo0to1 (0.0f));
    folder.deleteRecursively();
}

// Every factory template keeps the sample, sounds, and stays at a sane level.
void checkFactory (const juce::File& sampleFile)
{
    ReanimaProcessor p;
    p.prepareToPlay (48000.0, 512);
    p.loadSampleNow (sampleFile);
    const auto sample = p.getSample();
    const int count = (int) Factory::presets().size();
    juce::String report;
    bool allGood = true;

    for (int i = 0; i < count; ++i)
    {
        p.applyFactoryPreset (i);
        wait (30);
        // A preset that needs the sample analysed again (another engine, or the other way round) is played
        // once that is done, as one does after choosing it.
        waitFor ([&] { return ! p.isLoading(); });
        wait (30);
        juce::AudioBuffer<float> buf (2, 512);
        float peak = 0.0f;
        bool finite = true;
        // Some presets take seconds to rise (slow attacks, drones): a note of six seconds, nine of sound.
        for (int b = 0; b < (int) (9.0 * 48000 / 512); ++b)
        {
            juce::MidiBuffer midi;
            if (b == 0)
                midi.addEvent (juce::MidiMessage::noteOn (1, 60, 0.9f), 0);
            if (b == (int) (6.0 * 48000 / 512))
                midi.addEvent (juce::MidiMessage::noteOff (1, 60), 0);
            p.processBlock (buf, midi);
            for (int ch = 0; ch < 2; ++ch)
                for (int s = 0; s < 512; ++s)
                    finite = finite && std::isfinite (buf.getSample (ch, s));
            peak = std::max (peak, buf.getMagnitude (0, 512));
        }
        // Another engine may analyse the sample again: the same sample, a new object.
        waitFor ([&] { return ! p.isLoading(); });
        bool good = finite && peak > 0.003f && peak < 1.0f && p.getSample()->length == sample->length && p.getSample()->name == sample->name
                       && p.getPresetName() == ReanimaProcessor::factoryPresetName (i);
        // Saved with a project and opened again: every parameter (effects and buses too) comes back.
        bool restored = true;
        {
            juce::MemoryBlock saved;
            p.getStateInformation (saved);
            ReanimaProcessor q;
            q.setStateInformation (saved.getData(), (int) saved.getSize());
            for (int k = 0; k < P::Count; ++k)
                if (std::abs (q.paramValue (k) - p.paramValue (k)) > 1.0e-5f * std::max (1.0f, std::abs (p.paramValue (k))))
                {
                    if (restored)
                        std::printf ("   %s: %s %.6f -> %.6f\n", Factory::presets()[(size_t) i].name, P::defs()[(size_t) k].id,
                                     p.paramValue (k), q.paramValue (k));
                    restored = false;
                }
        }
        good = good && restored;
        allGood = allGood && good;
        report << P::u8 (Factory::presets()[(size_t) i].name) << " " << juce::String (juce::Decibels::gainToDecibels (peak), 1) << " dB"
               << (good ? "" : (restored ? " (MAL)" : " (NO SE RECUPERA)")) << ";  ";
    }
    check (allGood, "los presets de fábrica conservan el sample, suenan y se recuperan al guardar", report);
}

// Undo and redo: a knob turned by hand, a factory preset, a sample loaded; automation is not a step.
void checkHistory (const juce::File& sampleFile)
{
    ReanimaProcessor p;
    p.prepareToPlay (48000.0, 512);
    p.loadSampleNow (sampleFile);
    wait (400);
    auto* cutoff = p.apvts.getParameter ("fltFreq");
    const float before = p.paramValue (P::FltFreq);

    cutoff->beginChangeGesture();
    cutoff->setValueNotifyingHost (0.8f);
    cutoff->endChangeGesture();
    wait (500);
    const float turned = p.paramValue (P::FltFreq);
    check (p.canUndo() && p.undo() && std::abs (p.paramValue (P::FltFreq) - before) < 0.01f, "Deshacer devuelve un mando movido a mano");
    check (p.redo() && std::abs (p.paramValue (P::FltFreq) - turned) < 0.01f, "Rehacer lo vuelve a mover");

    p.apvts.getParameter ("pan")->setValueNotifyingHost (0.9f);     // automation: no gesture
    wait (500);
    p.undo();
    check (std::abs (p.paramValue (P::FltFreq) - before) < 0.01f && p.paramValue (P::Pan) > 0.7f,
           "la automatización del host no es un paso de deshacer");

    const int engineBefore = (int) p.paramValue (P::Engine);
    int drone = 0;
    for (int i = 0; i < (int) Factory::presets().size(); ++i)
        if (juce::String (Factory::presets()[(size_t) i].name) == "Drone eterno")
            drone = i;
    p.applyFactoryPreset (drone);
    wait (500);
    waitFor ([&] { return ! p.isLoading(); });
    wait (300);
    check ((int) p.paramValue (P::Engine) == P::EngineDrone && p.undo() && (int) p.paramValue (P::Engine) == engineBefore,
           "un preset de fábrica se deshace de una vez");

    const int length = p.getSample()->length;
    p.edit (ReanimaProcessor::Edit::Trim, 0, length / 2);
    waitFor ([&] { return ! p.isLoading(); });
    wait (400);
    check (p.getSample()->length == length / 2 && p.undo() && waitFor ([&] { return ! p.isLoading() && p.getSample()->length == length; }),
           "una edición del sample está en el mismo historial");
}

void checkEditing (const juce::File& sampleFile)
{
    using E = ReanimaProcessor::Edit;
    ReanimaProcessor p;
    p.prepareToPlay (48000.0, 512);
    p.loadSampleNow (sampleFile);
    const auto settle = [&] { return waitFor ([&] { return ! p.isLoading() && p.getSample() != nullptr; }); };
    const int n = p.getSample()->length;
    const auto original = p.getSample();

    check (p.edit (E::Trim, n / 4, n / 2) && settle() && p.getSample()->length == n / 4, "Recortar deja sólo la selección",
           juce::String (p.getSample()->length) + " muestras");
    check (std::abs (p.getSample()->audio.getSample (0, 0) - original->audio.getSample (0, n / 4)) < 1.0e-6f, "lo recortado es lo seleccionado");
    check (p.isEdited() && p.canUndo(), "el sample queda editado y se puede deshacer");

    check (p.undo() && settle() && p.getSample()->length == n, "Deshacer vuelve al sample entero");
    check (p.redo() && settle() && p.getSample()->length == n / 4, "Rehacer repite el recorte");
    p.undo();
    settle();

    p.copyAudio (0, 1000);
    check (p.edit (E::Paste, n, n) && settle() && p.getSample()->length == n + 1000, "Pegar inserta lo copiado al final");
    check (std::abs (p.getSample()->audio.getSample (0, n + 10) - original->audio.getSample (0, 10)) < 1.0e-6f, "lo pegado es lo copiado");
    check (p.edit (E::Delete, n, n + 1000) && settle() && p.getSample()->length == n, "Borrar lo quita");

    check (p.edit (E::Silence, 0, 2000) && settle() && p.getSample()->audio.getMagnitude (0, 0, 2000) == 0.0f, "Silencio deja ceros");
    check (p.edit (E::Gain, 2000, n, -6.0f) && settle(), "Ganancia sobre la selección");
    const float ratio = p.getSample()->audio.getSample (0, 5000) / original->audio.getSample (0, 5000);
    check (std::abs (ratio - 0.501f) < 0.01f, "-6 dB es la mitad", juce::String (ratio, 3));

    // The edited audio goes with the project and comes back without the original file.
    juce::MemoryBlock state;
    p.getStateInformation (state);
    ReanimaProcessor second;
    second.prepareToPlay (48000.0, 512);
    second.setStateInformation (state.getData(), (int) state.getSize());
    check (waitFor ([&] { return second.getSample() != nullptr && ! second.isLoading(); }), "el proyecto recupera el sample editado");
    const auto back = second.getSample();
    float worst = 0.0f;
    for (int i = 0; i < n; ++i)
        worst = std::max (worst, std::abs (back->audio.getSample (0, i) - p.getSample()->audio.getSample (0, i)));
    check (back->length == n && worst < 1.0e-5f && second.isEdited(), "el audio editado vuelve igual (24 bit)",
           juce::String (state.getSize() / 1024) + " KB de estado, error " + juce::String (worst, 7));
}

// The demonstration sample: a new, empty instance gets it; a project being opened, with or without a
// sample of its own, never does; neither does an instance whose state comes while the demo is analysed.
void checkDemo (const juce::File& sampleFile)
{
    const auto demo = juce::String::fromUTF8 (ReanimaProcessor::demoName);
    const auto settle = [] (ReanimaProcessor& p, int ms)
    {
        for (int t = 0; t < ms; t += 50)
        {
            wait (50);
            if (t > 600 && ! p.isLoading())
                break;
        }
    };
    {
        ReanimaProcessor p;
        p.prepareToPlay (48000.0, 512);
        settle (p, 8000);
        const auto s = p.getSample();
        check (s != nullptr && s->name == demo, "una instancia nueva y vacía empieza con el sample de demostración",
               s != nullptr ? s->name : juce::String ("sin sample"));
        // It plays.
        juce::AudioBuffer<float> b (2, 512);
        float peak = 0.0f;
        for (int k = 0; k < 40; ++k)
        {
            juce::MidiBuffer midi;
            if (k == 0)
                midi.addEvent (juce::MidiMessage::noteOn (1, 60, 0.8f), 0);
            p.processBlock (b, midi);
            peak = std::max (peak, b.getMagnitude (0, 512));
        }
        check (peak > 0.01f, "el sample de demostración suena", juce::String (peak, 3));
        // Saved and opened again: the demo comes back from the project, as any sample would.
        juce::MemoryBlock saved;
        p.getStateInformation (saved);
        ReanimaProcessor q;
        q.setStateInformation (saved.getData(), (int) saved.getSize());
        settle (q, 8000);
        check (q.getSample() != nullptr && q.getSample()->name == demo, "un proyecto guardado con la demo la recupera");
    }
    {
        ReanimaProcessor source;
        source.loadSampleNow (sampleFile);
        juce::MemoryBlock saved;
        source.getStateInformation (saved);
        ReanimaProcessor p;
        p.setStateInformation (saved.getData(), (int) saved.getSize());
        settle (p, 8000);
        wait (1500);
        const auto s = p.getSample();
        check (s != nullptr && s->name != demo, "un proyecto con su sample se abre con el suyo, no con la demo",
               s != nullptr ? s->name : juce::String ("sin sample"));
    }
    juce::MemoryBlock empty;
    {
        ReanimaProcessor e;
        e.getStateInformation (empty);
    }
    {
        ReanimaProcessor p;
        p.setStateInformation (empty.getData(), (int) empty.getSize());
        wait (2500);
        check (p.getSample() == nullptr && ! p.isLoading(), "un proyecto guardado sin sample sigue sin sample");
    }
    {
        // The state arrives while the demo is being analysed.
        ReanimaProcessor p;
        for (int t = 0; t < 3000 && ! p.isLoading(); t += 10)
            wait (10);
        const bool wasLoading = p.isLoading();
        p.setStateInformation (empty.getData(), (int) empty.getSize());
        wait (3000);
        check (wasLoading && p.getSample() == nullptr && ! p.isLoading(), "un proyecto que llega mientras se analiza la demo la descarta");
    }
}

// Changing from engine A to B, C or D completes the analysis in the background; when it arrives, the
// notes that sound go on (they finish with the analysis they started with) and the old one is let go.
void checkEngineSwitch (const juce::File& sampleFile)
{
    for (const int target : { P::EngineReenvelope, P::EngineResynth, P::EngineHybrid })
    {
        ReanimaProcessor p;
        p.prepareToPlay (48000.0, 256);
        p.loadSampleNow (sampleFile);
        const auto before = p.getSample();
        const auto set = [&] (const char* id, float v) { auto* q = p.apvts.getParameter (id); q->setValueNotifyingHost (q->convertTo0to1 (v)); };
        set ("sustainType", 1.0f);
        set ("ampR", 3000.0f);
        juce::AudioBuffer<float> b (2, 256);
        float quietest = 1.0f, beforeArrival = 0.0f;
        bool arrived = false;
        int afterArrival = 0;
        for (int k = 0; k < 3000 && afterArrival < 60; ++k)
        {
            juce::MidiBuffer midi;
            if (k == 0)
                midi.addEvent (juce::MidiMessage::noteOn (1, 60, 0.9f), 0);
            if (k == 20)
                set ("engine", (float) target);
            p.processBlock (b, midi);
            const float level = b.getMagnitude (0, 256);
            if (arrived)
            {
                quietest = std::min (quietest, level);
                ++afterArrival;
            }
            else
                beforeArrival = level;
            // Time for the analysis to finish while the note still sounds (in a host it takes a second or two
            // of playing).
            wait (k > 20 && ! arrived ? 15 : 0);
            if (! arrived && p.getSample() != before && p.getSample() != nullptr && p.getSample()->forAllEngines)
            {
                arrived = true;
                std::printf ("   análisis completo en el bloque %d, nivel %.1f dB\n", k, juce::Decibels::gainToDecibels (beforeArrival));
            }
        }
        quietest = beforeArrival > 1.0e-3f ? quietest / beforeArrival : 0.0f;
        // The note ends; the old analysis is let go.
        {
            juce::MidiBuffer midi;
            midi.addEvent (juce::MidiMessage::noteOff (1, 60), 0);
            p.processBlock (b, midi);
        }
        for (int k = 0; k < (int) (4.5 * 48000 / 256); ++k)
        {
            juce::MidiBuffer none;
            p.processBlock (b, none);
            if (k % 16 == 0)
                wait (1);
        }
        wait (300);
        check (arrived && quietest > 0.1f && ! p.engine.previousInUse.load(),
               "cambiar del motor A al " + juce::String::charToString ((juce::juce_wchar) ('A' + target))
                   + " no corta la nota que suena y libera el análisis anterior",
               "tras llegar el análisis, el bloque más bajo a " + juce::String (juce::Decibels::gainToDecibels (quietest), 1) + " dB del anterior");
    }
}

// The dice: every roll sounds, does not clip, keeps the CPU low, changes the sound, and is one step of the
// history. Snap a C pinned by the user stays through the dice, presets and saving; unpinned, presets decide.
void checkDiceAndSnap (const juce::File& sampleFile)
{
    ReanimaProcessor p;
    p.prepareToPlay (48000.0, 512);
    p.loadSampleNow (sampleFile);
    wait (300);
    juce::String bad;
    std::set<int> engines;
    double worstLoad = 0.0;
    int rolls = 20;
    for (int k = 0; k < rolls; ++k)
    {
        std::vector<float> before;
        for (int i = 0; i < P::Count; ++i)
            before.push_back (p.paramValue (i));
        p.randomize();
        if (k == 2)
        {
            // Two quick rolls, the first one's analysis still running: the second is undone alone.
            for (int i = 0; i < P::Count; ++i)
                before[(size_t) i] = p.paramValue (i);
            p.randomize();
        }
        std::vector<float> rolled;
        for (int i = 0; i < P::Count; ++i)
            rolled.push_back (p.paramValue (i));
        waitFor ([&] { return ! p.isLoading(); });
        wait (300);
        for (int i = 0; i < P::Count; ++i)
            if (std::abs (p.paramValue (i) - rolled[(size_t) i]) > 1.0e-4f * std::max (1.0f, std::abs (rolled[(size_t) i])))
                std::printf ("   tras el dado cambia solo: %s %.4f -> %.4f\n", P::defs()[(size_t) i].id, rolled[(size_t) i], p.paramValue (i));
        engines.insert ((int) p.paramValue (P::Engine));
        juce::AudioBuffer<float> b (2, 512);
        float peak = 0.0f;
        bool finite = true;
        for (int blk = 0; blk < (int) (3.0 * 48000 / 512); ++blk)
        {
            juce::MidiBuffer midi;
            if (blk == 0)
                midi.addEvent (juce::MidiMessage::noteOn (1, 60, 0.8f), 0);
            if (blk == (int) (2.0 * 48000 / 512))
                midi.addEvent (juce::MidiMessage::noteOff (1, 60), 0);
            const double t0 = juce::Time::getMillisecondCounterHiRes();
            p.processBlock (b, midi);
            worstLoad = std::max (worstLoad, (juce::Time::getMillisecondCounterHiRes() - t0) / (1000.0 * 512 / 48000));
            for (int c = 0; c < 2; ++c)
                for (int i = 0; i < 512; ++i)
                    finite = finite && std::isfinite (b.getSample (c, i));
            peak = std::max (peak, b.getMagnitude (0, 512));
        }
        if (! finite || peak < 0.003f || peak > 1.0f)
            bad << "tirada " << k << ": pico " << juce::String (peak, 3) << (finite ? "" : " no finito") << "; ";
        // Undo brings back what there was.
        if (k == 2)
        {
            p.serviceMessageThread();
            wait (400);
            p.serviceMessageThread();
            bool same = p.undo();
            std::printf ("   deshacer: %s\n", same ? "hecho" : "no había paso");
            for (int i = 0; i < P::FxSlotBase && same; ++i)
            {
                same = std::abs (p.paramValue (i) - before[(size_t) i]) <= 1.0e-4f * std::max (1.0f, std::abs (before[(size_t) i]));
                if (! same)
                    std::printf ("   distinto: %s %.4f -> %.4f\n", P::defs()[(size_t) i].id, before[(size_t) i], p.paramValue (i));
            }
            check (same, "Deshacer recupera el sonido de antes del dado");
            p.redo();
        }
    }
    check (bad.isEmpty() && engines.size() >= 4 && worstLoad < 0.8,
           "el dado da sonidos que suenan, sin saturar ni sobrecargar, y variados",
           juce::String ((int) engines.size()) + " motores distintos en " + juce::String (rolls) + " tiradas, peor bloque "
               + juce::String (100.0 * worstLoad, 0) + " % del tiempo real; " + bad);

    // Snap a C pinned.
    p.setSnapPreference (true);
    bool held = true;
    for (int i = 0; i < 6; ++i)
    {
        p.applyFactoryPreset (i * 7 % (int) Factory::presets().size());
        held = held && p.paramValue (P::SnapC) > 0.5f;
        p.randomize();
        held = held && p.paramValue (P::SnapC) > 0.5f;
    }
    juce::MemoryBlock saved;
    p.getStateInformation (saved);
    ReanimaProcessor q;
    q.setStateInformation (saved.getData(), (int) saved.getSize());
    held = held && q.paramValue (P::SnapC) > 0.5f && q.snapPinned();
    check (held, "Snap a C activado se mantiene con presets, el dado y al guardar el proyecto");

    // Unpinned: each preset decides again (Bajo sinte turns it on, Fiel al sample does not).
    p.setSnapPreference (false);
    int bass = 0;
    for (int i = 0; i < (int) Factory::presets().size(); ++i)
        if (juce::String (Factory::presets()[(size_t) i].name) == "Bajo sinte")
            bass = i;
    p.applyFactoryPreset (bass);
    const bool bassOn = p.paramValue (P::SnapC) > 0.5f;
    p.applyFactoryPreset (0);
    check (bassOn && p.paramValue (P::SnapC) < 0.5f, "Snap a C desactivado: cada preset vuelve a decidir");
    waitFor ([&] { return ! p.isLoading(); });
}

// The welcome tour is the instance's: it opens once in a new one, and once closed it stays closed in that
// instance through the window reopening, presets, saving and reopening the project; another new instance
// shows it again, and the help opens it on demand.
void checkWelcome()
{
    ReanimaProcessor proc;      // its own instances: the shared one keeps its state for the checks after this
    proc.prepareToPlay (48000.0, 512);
    ReanimaEditor::offerWelcome = true;
    const bool neverWas = ReanimaEditor::welcomeNever();
    ReanimaEditor::setWelcomeNever (false);
    bool offered = false;
    const auto opensIn = [&offered] (ReanimaProcessor& p, bool tickNever = false)
    {
        std::unique_ptr<juce::AudioProcessorEditor> e (p.createEditor());
        wait (900);
        auto* ed = dynamic_cast<ReanimaEditor*> (e.get());
        auto& g = ed->getGuide();
        const bool running = g.isRunning() && g.tourName() == "Bienvenida" && g.stepCount() >= 10;
        if (running)
        {
            offered = g.dontShowButton().isVisible();
            g.dontShowButton().setToggleState (tickNever, juce::dontSendNotification);
            g.keyPressed (juce::KeyPress (juce::KeyPress::escapeKey));
        }
        return running;
    };
    const bool opened = opensIn (proc);
    const bool marked = proc.welcomeShown();
    const bool quietAgain = ! opensIn (proc);
    // A preset does not bring the flag back, nor take it away.
    proc.applyFactoryPreset (3);
    const auto presetFile = proc.getPresetFolder().getChildFile (juce::String ("Prueba tour") + ReanimaProcessor::presetExtension);
    proc.savePreset ("Prueba tour");
    const bool presetKeeps = proc.welcomeShown() && ! opensIn (proc);
    bool presetClean = false;
    if (auto xml = juce::XmlDocument::parse (presetFile))
        presetClean = ! xml->hasAttribute ("welcomeShown");
    // The project reopened (and the track duplicated): the instance already had its tour.
    juce::MemoryBlock state;
    proc.getStateInformation (state);
    ReanimaProcessor reopened;
    reopened.prepareToPlay (48000.0, 512);
    reopened.setStateInformation (state.getData(), (int) state.getSize());
    waitFor ([&] { return ! reopened.isLoading(); });
    const bool projectQuiet = ! opensIn (reopened);
    // The preset saved by a marked instance opens quiet in it, and does not mark a new one.
    ReanimaProcessor other;
    other.prepareToPlay (48000.0, 512);
    other.loadPreset (presetFile);
    waitFor ([&] { return ! other.isLoading(); });
    const bool presetDoesNotMark = ! other.welcomeShown();
    const bool newInstance = opensIn (other);
    presetFile.deleteFile();
    // "No volver a mostrar" ticked in one instance: a new instance does not open it; the ? still does.
    ReanimaProcessor third;
    third.prepareToPlay (48000.0, 512);
    opensIn (third, true);
    const bool neverSaved = ReanimaEditor::welcomeNever();
    ReanimaProcessor fourth;
    fourth.prepareToPlay (48000.0, 512);
    const bool neverQuiet = ! opensIn (fourth);
    bool onDemand = false;
    {
        std::unique_ptr<juce::AudioProcessorEditor> e (fourth.createEditor());
        wait (900);
        auto* ed = dynamic_cast<ReanimaEditor*> (e.get());
        ed->startWelcome();
        onDemand = ed->getGuide().isRunning() && ed->getGuide().dontShowButton().isVisible();
        ed->getGuide().stop();
    }
    ReanimaEditor::setWelcomeNever (neverWas);
    ReanimaEditor::offerWelcome = false;
    check (opened && offered && marked && quietAgain && presetKeeps && presetClean && projectQuiet && presetDoesNotMark && newInstance
               && neverSaved && neverQuiet && onDemand,
           "el tour de bienvenida es de cada instancia: cerrado no vuelve (ventana, presets, proyecto); otra instancia nueva lo muestra; "
           "«no volver a mostrar» lo apaga en todas y el ? lo abre",
           juce::String (opened ? "abre " : "NO ABRE ") + (marked ? "marca " : "NO MARCA ") + (quietAgain ? "calla " : "REPITE ")
               + (presetKeeps ? "preset ok " : "PRESET LO REABRE ") + (presetClean ? "preset limpio " : "PRESET LO GUARDA ")
               + (projectQuiet ? "proyecto ok " : "PROYECTO LO REABRE ") + (presetDoesNotMark ? "no marca otra " : "MARCA OTRA ")
               + (newInstance ? "nueva abre " : "NUEVA NO ABRE ") + (offered ? "casilla " : "SIN CASILLA ")
               + (neverSaved && neverQuiet ? "nunca más ok " : "NUNCA MÁS FALLA ") + (onDemand ? "a demanda" : "NO A DEMANDA"));
}

// MPE in every engine: two notes on two channels. Bending one by +12 semitones on its own channel sounds like
// playing it an octave up while the other stays (spectra compared with the same notes played plainly);
// pressure on one channel makes that note louder; timbre (CC74) makes it brighter.
void checkMpe (const juce::File& sampleFile)
{
    juce::String report;
    bool allGood = true;
    for (int engine = 0; engine < 6; ++engine)
    {
        const auto play = [&] (std::function<void (juce::MidiBuffer&)> before, int noteA, int noteB, int chA, int chB)
        {
            ReanimaProcessor p;
            p.prepareToPlay (48000.0, 512);
            p.loadSampleNow (sampleFile);
            auto* e = p.apvts.getParameter ("engine");
            e->setValueNotifyingHost (e->convertTo0to1 ((float) engine));
            auto* st = p.apvts.getParameter ("sustainType");
            st->setValueNotifyingHost (1.0f);
            waitFor ([&] { return ! p.isLoading(); });
            wait (200);
            waitFor ([&] { return ! p.isLoading(); });
            juce::AudioBuffer<float> b (2, 512), all (2, 512 * 140);
            for (int k = 0; k < 140; ++k)
            {
                juce::MidiBuffer midi;
                if (k == 0)
                {
                    before (midi);
                    midi.addEvent (juce::MidiMessage::noteOn (chA, noteA, 0.8f), 1);
                    midi.addEvent (juce::MidiMessage::noteOn (chB, noteB, 0.8f), 2);
                }
                p.processBlock (b, midi);
                for (int c = 0; c < 2; ++c)
                    all.copyFrom (c, k * 512, b, c, 0, 512);
            }
            return all;
        };
        // Average magnitude spectrum (1/6 octave bands, 60 Hz - 16 kHz) of the held part.
        const auto bands = [] (const juce::AudioBuffer<float>& a)
        {
            constexpr int order = 13, size = 1 << order;
            juce::dsp::FFT fft (order);
            std::vector<float> acc (size / 2, 0.0f), buf ((size_t) size * 2);
            for (int start = 48000; start + size < a.getNumSamples(); start += size / 2)
            {
                std::fill (buf.begin(), buf.end(), 0.0f);
                for (int i = 0; i < size; ++i)
                    buf[(size_t) i] = 0.5f * (a.getSample (0, start + i) + a.getSample (1, start + i))
                                    * (0.5f - 0.5f * std::cos (6.2831853f * (float) i / (float) size));
                fft.performFrequencyOnlyForwardTransform (buf.data());
                for (int k = 0; k < size / 2; ++k)
                    acc[(size_t) k] += buf[(size_t) k] * buf[(size_t) k];
            }
            std::vector<float> out;
            for (double lo = 60.0; lo < 16000.0; lo *= std::pow (2.0, 1.0 / 6.0))
            {
                double e = 0.0;
                for (int k = (int) (lo / 48000.0 * size); k < (int) (lo * std::pow (2.0, 1.0 / 6.0) / 48000.0 * size); ++k)
                    e += acc[(size_t) k];
                out.push_back ((float) (10.0 * std::log10 (e + 1.0e-12)));
            }
            return out;
        };
        const auto distance = [] (const std::vector<float>& x, const std::vector<float>& y)
        {
            double mx = *std::max_element (x.begin(), x.end()), my = *std::max_element (y.begin(), y.end()), d = 0.0;
            for (size_t i = 0; i < x.size(); ++i)
                d += std::abs (std::max (x[i] - mx, -60.0) - std::max (y[i] - my, -60.0));
            return d / (double) x.size();
        };
        const int up = 8192 + 8191 / 4;     // +12 of the 48 semitones of an MPE channel
        const auto bent = play ([&] (juce::MidiBuffer& m) { m.addEvent (juce::MidiMessage::pitchWheel (2, up), 0); }, 60, 64, 2, 3);
        const auto plainUp = play ([] (juce::MidiBuffer&) {}, 72, 64, 1, 1);
        const auto plain = play ([] (juce::MidiBuffer&) {}, 60, 64, 1, 1);
        const double toUp = distance (bands (bent), bands (plainUp)), toPlain = distance (bands (bent), bands (plain));
        const bool pitchOk = toUp < toPlain * 0.6;

        const auto rms = [] (const juce::AudioBuffer<float>& a) { return a.getRMSLevel (0, 48000, a.getNumSamples() - 48000); };
        const auto pressed = play ([] (juce::MidiBuffer& m) { m.addEvent (juce::MidiMessage::channelPressureChange (2, 127), 0); }, 60, 64, 2, 3);
        const auto unpressed = play ([] (juce::MidiBuffer&) {}, 60, 64, 2, 3);
        const float louder = juce::Decibels::gainToDecibels (rms (pressed) / std::max (1.0e-9f, rms (unpressed)));

        const auto centroid = [&] (const juce::AudioBuffer<float>& a)
        {
            const auto v = bands (a);
            double num = 0.0, den = 0.0;
            for (size_t i = 0; i < v.size(); ++i)
            {
                const double w = std::pow (10.0, v[i] / 10.0);
                num += w * (double) i;
                den += w;
            }
            return num / std::max (1.0e-12, den);
        };
        const auto bright = play ([] (juce::MidiBuffer& m) { m.addEvent (juce::MidiMessage::controllerEvent (2, 74, 127), 0);
                                                             m.addEvent (juce::MidiMessage::controllerEvent (3, 74, 127), 0); }, 60, 64, 2, 3);
        const auto dark = play ([] (juce::MidiBuffer& m) { m.addEvent (juce::MidiMessage::controllerEvent (2, 74, 0), 0);
                                                           m.addEvent (juce::MidiMessage::controllerEvent (3, 74, 0), 0); }, 60, 64, 2, 3);
        const double brighter = centroid (bright) - centroid (dark);

        const bool good = pitchOk && louder > 0.5f && brighter > 0.3;
        allGood = allGood && good;
        report << juce::String::charToString ((juce::juce_wchar) ('A' + engine)) << ": bend " << juce::String (toUp, 1) << "/"
               << juce::String (toPlain, 1) << " dB, presión +" << juce::String (louder, 1) << " dB, timbre +" << juce::String (brighter, 2)
               << " bandas" << (good ? "" : " (MAL)") << "; ";
    }
    check (allGood, "MPE en los seis motores: bend, presión y timbre por nota", report);
}

// The audit of 2026-11-14: Reverse does not outlive a sample or a preset, low end in mono, the master
// ceiling, and the favourites.
void setValue (ReanimaProcessor& p, const char* id, float value)
{
    auto* param = p.apvts.getParameter (id);
    param->beginChangeGesture();
    param->setValueNotifyingHost (param->convertTo0to1 (value));
    param->endChangeGesture();
}

juce::AudioBuffer<float> playNote (ReanimaProcessor& p, int note, double seconds, float velocity = 0.8f)
{
    const int block = 512, blocks = (int) (seconds * 48000 / block);
    juce::AudioBuffer<float> all (2, blocks * block), b (2, block);
    for (int k = 0; k < blocks; ++k)
    {
        juce::MidiBuffer midi;
        if (k == 0)
            midi.addEvent (juce::MidiMessage::noteOn (1, note, velocity), 0);
        if (k == blocks * 2 / 3)
            midi.addEvent (juce::MidiMessage::noteOff (1, note), 0);
        b.clear();
        p.processBlock (b, midi);
        for (int c = 0; c < 2; ++c)
            all.copyFrom (c, k * block, b, c, 0, block);
    }
    return all;
}

int factoryIndex (const char* name)
{
    for (int i = 0; i < (int) Factory::presets().size(); ++i)
        if (juce::String::fromUTF8 (Factory::presets()[(size_t) i].name) == juce::String::fromUTF8 (name))
            return i;
    return 0;
}

// A sample loaded by the user (Cargar, or dropped) starts a clean session: engine A, no reverse, no texture,
// no filter, modulation or effects, the sample's own envelope; Volumen, Pan, Snap a C, the playing settings
// and Fijar ADSR stay. A project reopened keeps everything it had.
void checkFreshSample (const juce::File& sampleFile, const juce::File& folder)
{
    const auto other = folder.getChildFile ("Otro sample.wav");
    {
        constexpr double sr = 44100.0;
        juce::AudioBuffer<float> b (2, (int) (1.5 * sr));
        for (int i = 0; i < b.getNumSamples(); ++i)
        {
            const double t = i / sr;
            const float v = (float) (0.5 * std::exp (-3.0 * t) * std::sin (2.0 * juce::MathConstants<double>::pi * 330.0 * t));
            b.setSample (0, i, v);
            b.setSample (1, i, v);
        }
        other.deleteFile();
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (other);
        juce::WavAudioFormat wav;
        if (auto w = wav.createWriterFor (stream, juce::AudioFormatWriterOptions().withSampleRate (sr).withNumChannels (2).withBitsPerSample (24)))
            w->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
    }

    ReanimaProcessor p;
    p.prepareToPlay (48000.0, 512);
    p.loadSampleNow (sampleFile);
    waitFor ([&] { return p.getSample() != nullptr && ! p.isLoading(); });
    p.serviceMessageThread();
    setValue (p, "engine", 1.0f);
    setValue (p, "grainMix", 0.5f);
    setValue (p, "fltOn", 1.0f);
    setValue (p, "transpose", 5.0f);
    setValue (p, "unison", 3.0f);
    setValue (p, "m1Src", 1.0f);
    setValue (p, "m1Dst", 1.0f);
    setValue (p, "m1Amt", 0.3f);
    setValue (p, "bits", 8.0f);
    setValue (p, "volume", -10.0f);
    setValue (p, "pan", 0.3f);
    setValue (p, "polyMode", 1.0f);
    setValue (p, "glide", 120.0f);
    p.setFxSlotType (0, Fx::Reverb);
    p.setSnapPreference (true);
    p.apvts.state.setProperty ("presetName", "Algo", nullptr);
    waitFor ([&] { return ! p.isLoading(); });
    wait (200);
    p.serviceMessageThread();

    // What a project holds comes back as it was (a project never starts a fresh session).
    juce::MemoryBlock state;
    p.getStateInformation (state);

    p.loadSample (other);
    waitFor ([&] { return p.getSample() != nullptr && ! p.isLoading() && p.getSample()->name == "Otro sample"; });
    wait (300);
    p.serviceMessageThread();
    const auto& d = p.getSample()->detected;
    const bool clean = p.paramValue (P::Engine) < 0.5f && p.paramValue (P::GrainMix) == 0.0f && p.paramValue (P::FltOn) < 0.5f
                    && p.paramValue (P::Transpose) == 0.0f && p.paramValue (P::Unison) == 1.0f && p.paramValue (P::M1Src) < 0.5f
                    && p.paramValue (P::Bits) == 24.0f && p.paramValue (P::fxType (0)) < 0.5f && p.paramValue (P::Reverse) < 0.5f;
    const bool envelope = std::abs (p.paramValue (P::AmpA) - d.attack * 1000.0f) < 0.5f && std::abs (p.paramValue (P::AmpR) - d.release * 1000.0f) < 1.0f
                       && std::abs (p.paramValue (P::AmpS) - d.sustain) < 1.0e-3f;
    const bool kept = std::abs (p.paramValue (P::Volume) + 10.0f) < 0.01f && std::abs (p.paramValue (P::Pan) - 0.3f) < 0.01f
                   && p.paramValue (P::SnapC) > 0.5f && p.snapPinned() && p.paramValue (P::PolyMode) > 0.5f && std::abs (p.paramValue (P::Glide) - 120.0f) < 0.5f;
    const bool unnamed = p.getPresetName().isEmpty();
    check (clean && envelope && kept && unnamed, "un sample nuevo empieza limpio: motor A, sin efectos, filtro, modulación ni transformaciones, con su ADSR",
           juce::String (clean ? "limpio " : "HEREDA ") + (envelope ? "ADSR propio " : "ADSR AJENO ") + (kept ? "conserva salida/Snap/interpretación " : "PIERDE PREFERENCIAS ")
               + (unnamed ? "sin preset" : "PRESET ANTIGUO"));

    // Undo brings back the sample before and everything it had.
    p.undo();
    waitFor ([&] { return ! p.isLoading() && p.getSample() != nullptr && p.getSample()->name != "Otro sample"; });
    wait (300);
    p.serviceMessageThread();
    const bool undone = p.paramValue (P::Engine) > 0.5f && p.paramValue (P::fxType (0)) > 0.5f && std::abs (p.paramValue (P::Transpose) - 5.0f) < 0.01f;
    check (undone, "Deshacer tras cargar un sample recupera el anterior con todos sus ajustes",
           "motor " + juce::String (p.paramValue (P::Engine), 0) + ", ranura 1 " + juce::String (p.paramValue (P::fxType (0)), 0));

    // A project opened is never reset.
    ReanimaProcessor reopened;
    reopened.prepareToPlay (48000.0, 512);
    reopened.setStateInformation (state.getData(), (int) state.getSize());
    waitFor ([&] { return reopened.getSample() != nullptr && ! reopened.isLoading(); });
    wait (300);
    reopened.serviceMessageThread();
    check (reopened.paramValue (P::Engine) > 0.5f && reopened.paramValue (P::fxType (0)) > 0.5f && std::abs (reopened.paramValue (P::Transpose) - 5.0f) < 0.01f,
           "un proyecto reabierto conserva su motor, efectos y ajustes");

    // Fijar ADSR closed: the envelope stays as it was set.
    setValue (p, "envLock", 1.0f);
    setValue (p, "ampA", 777.0f);
    p.loadSample (other);
    waitFor ([&] { return p.getSample() != nullptr && ! p.isLoading() && p.getSample()->name == "Otro sample"; });
    wait (300);
    p.serviceMessageThread();
    check (std::abs (p.paramValue (P::AmpA) - 777.0f) < 1.0f && p.paramValue (P::EnvLock) > 0.5f, "con Fijar ADSR cerrado, el sample nuevo conserva tu envolvente",
           juce::String (p.paramValue (P::AmpA), 0) + " ms");
    p.setSnapPreference (false);
    other.deleteFile();
}

// Deleting one of the user's presets: only files of the preset folder; out of the favourites; the sound loaded
// stays; the list offers deleting for the user's own and never for the factory ones.
void checkDeletePreset (const juce::File& sampleFile, const juce::File& folder)
{
    ReanimaProcessor p;
    p.prepareToPlay (48000.0, 512);
    p.loadSampleNow (sampleFile);
    waitFor ([&] { return p.getSample() != nullptr && ! p.isLoading(); });
    folder.deleteRecursively();
    p.setPresetFolder (folder);
    setValue (p, "sampleGain", 4.0f);
    const juce::String name = "Prueba borrar " + juce::String (juce::Random::getSystemRandom().nextInt (100000));
    p.savePreset (name);
    p.savePreset (name + " b");
    const auto file = folder.getChildFile (name + ReanimaProcessor::presetExtension);
    PresetBar::setFavourite (name, true);

    bool userDeletable = false, factoryFixed = true;
    {
        PresetBar bar (p);
        auto menu = bar.buildMenu ({}, p.getPresets());
        std::function<void (const juce::PopupMenu&)> visit = [&] (const juce::PopupMenu& m)
        {
            for (juce::PopupMenu::MenuItemIterator it (m); it.next();)
            {
                const auto& x = it.getItem();
                if (x.subMenu != nullptr)
                    visit (*x.subMenu);
                if (auto* item = dynamic_cast<PresetItem*> (x.customComponent.get()))
                {
                    if (x.itemID >= 2000 && x.itemID < 5000)
                        factoryFixed = factoryFixed && ! item->canDelete();
                    else if (x.itemID >= 1 && x.itemID < 1000)
                        userDeletable = item->canDelete() || userDeletable;
                }
            }
        };
        visit (menu);
    }

    const bool removed = PresetBar::removePreset (p, file);
    const bool gone = ! file.existsAsFile() && p.getPresets().size() == 1 && ! PresetBar::favourites().contains (name);
    const bool soundKept = std::abs (p.paramValue (P::SampleGain) - 4.0f) < 0.01f && p.getSample() != nullptr;
    const auto outside = folder.getParentDirectory().getChildFile (juce::String ("fuera") + ReanimaProcessor::presetExtension);
    outside.replaceWithText ("<x/>");
    const bool guarded = ! p.deletePreset (outside) && outside.existsAsFile() && ! p.deletePreset (file);
    outside.deleteFile();
    check (removed && gone && soundKept && guarded && userDeletable && factoryFixed,
           "borrar un preset tuyo: sale de la lista y de favoritos, el sonido cargado se queda; los de fábrica no se pueden borrar",
           juce::String (removed && gone ? "borrado " : "NO BORRA ") + (soundKept ? "sonido intacto " : "CAMBIA EL SONIDO ")
               + (guarded ? "sólo su carpeta " : "BORRA FUERA ") + (userDeletable ? "papelera en los tuyos " : "SIN PAPELERA ")
               + (factoryFixed ? "fábrica protegida" : "FÁBRICA BORRABLE"));
    setValue (p, "sampleGain", 0.0f);
    folder.deleteRecursively();
}

// Dropping on the window: every format the computer reads (whatever the case of the extension), a drag that
// arrives as a file:// URL, and nothing that is not audio.
void checkDrop (const juce::File& sampleFile, const juce::File& folder)
{
    folder.createDirectory();
    const auto upper = folder.getChildFile ("Arrastrado DESDE ableton.WAV");
    sampleFile.copyFileTo (upper);
    const auto text = folder.getChildFile ("nota.txt");
    text.replaceWithText ("no es audio");

    ReanimaProcessor p;
    p.prepareToPlay (48000.0, 512);
    p.loadSampleNow (sampleFile);
    waitFor ([&] { return p.getSample() != nullptr && ! p.isLoading(); });
    std::unique_ptr<juce::AudioProcessorEditor> e (p.createEditor());
    auto* ed = dynamic_cast<ReanimaEditor*> (e.get());
    const bool wants = ed->isInterestedInFileDrag ({ text.getFullPathName(), upper.getFullPathName() })
                    && ! ed->isInterestedInFileDrag ({ text.getFullPathName() });
    ed->filesDropped ({ text.getFullPathName(), upper.getFullPathName() }, 10, 10);
    const bool loaded = waitFor ([&] { return ! p.isLoading() && p.getSample() != nullptr && p.getSample()->name == "Arrastrado DESDE ableton"; });
    const auto url = juce::URL (upper).toString (false);
    const bool textWanted = ed->isInterestedInTextDrag (url) && ! ed->isInterestedInTextDrag ("hola");
    const auto wildcard = SampleAnalysis::audioWildcard();
    const bool formats = wildcard.contains ("*.wav") && wildcard.contains ("*.aif") && wildcard.contains ("*.flac") && wildcard.contains ("*.mp3") && wildcard.contains ("*.ogg");
    check (wants && loaded && textWanted && formats, "arrastrar: carga el primer audio entre los archivos, con extensión en mayúsculas o como URL de texto",
           juce::String (wants ? "acepta " : "NO ACEPTA ") + (loaded ? "carga " : "NO CARGA ") + (textWanted ? "URL ok " : "URL NO ") + wildcard);
    e.reset();
    folder.deleteRecursively();
}

// A loop loaded by the user plays as LOOP by itself (its name and length say so), with its gate; MOTOR shows
// the loop's controls; back to ONE SHOT the sample's own envelope returns; a one shot stays a one shot.
void checkLoopMode (const juce::File& folder)
{
    folder.createDirectory();
    const auto loopFile = folder.getChildFile ("Prueba drums loop 100bpm.wav");
    {
        constexpr double sr = 48000.0, bpm = 100.0;
        const int n = (int) std::lround (4 * 60.0 / bpm * sr);
        juce::AudioBuffer<float> b (2, n);
        juce::Random rng (5);
        for (int i = 0; i < n; ++i)
        {
            const double t = i / sr, beat = std::fmod (t, 60.0 / bpm), half = std::fmod (t + 30.0 / bpm, 60.0 / bpm);
            const float v = (float) (0.6 * std::exp (-beat / 0.08) * std::sin (2.0 * juce::MathConstants<double>::pi * 60.0 * beat)
                                     + 0.25 * std::exp (-half / 0.02) * (rng.nextFloat() * 2.0 - 1.0));
            b.setSample (0, i, v);
            b.setSample (1, i, v);
        }
        loopFile.deleteFile();
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (loopFile);
        juce::WavAudioFormat wav;
        if (auto w = wav.createWriterFor (stream, juce::AudioFormatWriterOptions().withSampleRate (sr).withNumChannels (2).withBitsPerSample (24)))
            w->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
    }

    ReanimaProcessor p;
    p.prepareToPlay (48000.0, 512);
    p.loadSampleNow (folder.getParentDirectory().getChildFile ("Pluck A3.wav"));
    waitFor ([&] { return p.getSample() != nullptr && ! p.isLoading(); });
    const bool pluckOneShot = (int) p.paramValue (P::PlayMode) == P::PlayOneShot;
    p.loadSample (loopFile);
    waitFor ([&] { return ! p.isLoading() && p.getSample() != nullptr && p.getSample()->name == "Prueba drums loop 100bpm"; });
    wait (300);
    p.serviceMessageThread();
    const auto& info = p.getSample()->loopInfo;
    const bool loop = (int) p.paramValue (P::PlayMode) == P::PlayLoop && std::abs (p.paramValue (P::AmpA) - 1.0f) < 0.01f
                   && std::abs (p.paramValue (P::AmpR) - 30.0f) < 0.5f && info.beats == 4;

    bool panel = false, sliceShown = false;
    {
        std::unique_ptr<juce::AudioProcessorEditor> e (p.createEditor());
        e->setSize (ReanimaEditor::designW, ReanimaEditor::designH);
        e->setVisible (true);
        wait (400);
        juce::Component* beatsChoice = nullptr;
        juce::Component* warpChoice = nullptr;
        std::function<juce::Component* (juce::Component&, const juce::String&)> find = [&] (juce::Component& c, const juce::String& id) -> juce::Component*
        {
            if (c.getComponentID() == id)
                return &c;
            for (auto* child : c.getChildren())
                if (auto* f = find (*child, id))
                    return f;
            return nullptr;
        };
        beatsChoice = find (*e, "p:" + juce::String (P::LoopBeats));
        warpChoice = find (*e, "p:" + juce::String (P::WarpMode));
        panel = beatsChoice != nullptr && beatsChoice->isVisible() && (warpChoice == nullptr || ! warpChoice->isVisible());
        const auto img = e->createComponentSnapshot (e->getLocalBounds(), true, 1.0f);
        auto shotFile = folder.getParentDirectory().getChildFile ("11_loop.png");
        shotFile.deleteFile();
        juce::FileOutputStream os (shotFile);
        juce::PNGImageFormat().writeImageToStream (img, os);
        // SLICE: its own controls, the markers with their keys on the waveform.
        setValue (p, "loopKind", 1.0f);
        setValue (p, "ampR", 8000.0f);      // a preset's long release: the loop must still fill the width
        wait (400);
        {
            auto order = p.getSliceOrder();
            if (order.size() >= 3)
                std::swap (order[0], order[2]);
            p.setSliceOrder (order);
        }
        p.serviceMessageThread();
        wait (200);
        auto* sliceBy = find (*e, "p:" + juce::String (P::SliceBy));
        auto* algoChoice = find (*e, "p:" + juce::String (P::LoopAlgo));
        sliceShown = sliceBy != nullptr && sliceBy->isVisible() && (algoChoice == nullptr || ! algoChoice->isVisible()) && p.getSlices().size() >= 2;
        const auto img2 = e->createComponentSnapshot (e->getLocalBounds(), true, 1.0f);
        auto shot2 = folder.getParentDirectory().getChildFile ("11b_slice.png");
        shot2.deleteFile();
        juce::FileOutputStream os2 (shot2);
        juce::PNGImageFormat().writeImageToStream (img2, os2);
        // Three keys held: three slices lit, a playhead in each, from the engine.
        {
            juce::AudioBuffer<float> buf (2, 512);
            juce::MidiBuffer keys;
            for (const int k : { 60, 63, 66 })
                keys.addEvent (juce::MidiMessage::noteOn (1, k, 0.9f), 0);
            buf.clear();
            p.processBlock (buf, keys);
            for (int i = 0; i < 12; ++i)
            {
                juce::MidiBuffer none;
                buf.clear();
                p.processBlock (buf, none);
            }
            wait (250);
            const auto img3 = e->createComponentSnapshot (e->getLocalBounds(), true, 1.0f);
            auto shot3 = folder.getParentDirectory().getChildFile ("11c_slice_tocando.png");
            shot3.deleteFile();
            juce::FileOutputStream os3 (shot3);
            juce::PNGImageFormat().writeImageToStream (img3, os3);
            // EDICIÓN with the slices' order changed: the strip and the exports.
            std::function<juce::TabbedComponent* (juce::Component&)> tabsOf = [&] (juce::Component& c) -> juce::TabbedComponent*
            {
                if (auto* t = dynamic_cast<juce::TabbedComponent*> (&c))
                    return t;
                for (auto* child : c.getChildren())
                    if (auto* t = tabsOf (*child))
                        return t;
                return nullptr;
            };
            if (auto* tabs = tabsOf (*e))
            {
                tabs->setCurrentTabIndex (1);
                wait (300);
                const auto img4 = e->createComponentSnapshot (e->getLocalBounds(), true, 1.0f);
                auto shot4 = folder.getParentDirectory().getChildFile ("11d_edicion_slices.png");
                shot4.deleteFile();
                juce::FileOutputStream os4 (shot4);
                juce::PNGImageFormat().writeImageToStream (img4, os4);
                tabs->setCurrentTabIndex (0);
                wait (100);
            }
        }
        setValue (p, "loopKind", 0.0f);
        wait (200);
    }

    // Back to ONE SHOT by hand: the sample's own envelope.
    setValue (p, "playMode", 0.0f);
    wait (200);
    p.serviceMessageThread();
    const auto& d = p.getSample()->detected;
    const bool back = std::abs (p.paramValue (P::AmpA) - d.attack * 1000.0f) < 0.5f && std::abs (p.paramValue (P::AmpR) - d.release * 1000.0f) < 1.0f;

    // A preset keeps the sample's mode; the project keeps everything.
    setValue (p, "playMode", 1.0f);
    wait (200);
    p.serviceMessageThread();
    p.applyFactoryPreset (5);
    const bool presetKeeps = (int) p.paramValue (P::PlayMode) == P::PlayLoop;
    juce::MemoryBlock state;
    p.getStateInformation (state);
    ReanimaProcessor reopened;
    reopened.prepareToPlay (48000.0, 512);
    reopened.setStateInformation (state.getData(), (int) state.getSize());
    waitFor ([&] { return reopened.getSample() != nullptr && ! reopened.isLoading(); });
    wait (200);
    reopened.serviceMessageThread();
    const bool projectKeeps = (int) reopened.paramValue (P::PlayMode) == P::PlayLoop;

    check (pluckOneShot && loop && panel && sliceShown && back && presetKeeps && projectKeeps,
           "un loop cargado pasa solo a LOOP (4 tiempos, puerta 1/30 ms) y MOTOR muestra sus controles; ONE SHOT recupera su ADSR; preset y proyecto lo conservan",
           juce::String (pluckOneShot ? "pluck one shot " : "PLUCK LOOP ") + (loop ? "loop " : "NO LOOP ") + (panel ? "panel " : "PANEL MAL ") + (sliceShown ? "slice " : "SLICE MAL ")
               + (back ? "adsr vuelve " : "ADSR NO VUELVE ") + (presetKeeps ? "preset ok " : "PRESET LO QUITA ") + (projectKeeps ? "proyecto ok" : "PROYECTO LO PIERDE"));
    folder.deleteRecursively();
}

void checkAudit (const juce::File& sampleFile)
{
    {
        // A bowed note: there in 120 ms at -4.4 dB, then swelling to the top over two seconds. Its attack ends
        // where it settles, not at the top; a project saved by 0.9.5 with the envelope it detected then (attack
        // at the top) opens with the new detection, and still plays the file as it is.
        constexpr double sr = 48000.0;
        juce::AudioBuffer<float> b (2, (int) (3.5 * sr));
        for (int i = 0; i < b.getNumSamples(); ++i)
        {
            const double t = i / sr;
            const double env = t < 0.12 ? 0.6 * t / 0.12 : t < 2.3 ? 0.6 + 0.4 * (t - 0.12) / 2.18 : t < 3.2 ? 1.0 : (3.5 - t) / 0.3;
            double v = 0.0;
            for (int h = 1; h <= 6; ++h)
                v += std::sin (2.0 * juce::MathConstants<double>::pi * 220.0 * h * t) / h;
            b.setSample (0, i, (float) (0.4 * env * v));
            b.setSample (1, i, (float) (0.4 * env * v));
        }
        auto file = sampleFile.getParentDirectory().getChildFile ("Arco A3.wav");
        file.deleteFile();
        {
            std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
            juce::WavAudioFormat wav;
            if (auto w = wav.createWriterFor (stream, juce::AudioFormatWriterOptions().withSampleRate (sr).withNumChannels (2).withBitsPerSample (24)))
                w->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
        }
        ReanimaProcessor a;
        a.prepareToPlay (48000.0, 512);
        a.loadSampleNow (file);
        waitFor ([&] { return a.getSample() != nullptr && ! a.isLoading(); });
        const auto data = a.getSample();
        const auto now = data->detected, before = data->earlierDetected;
        check (data->hasEarlierDetected && now.attack < 0.3f && before.attack > 1.5f, "el ataque de una nota que sigue creciendo acaba donde se asienta",
               "ahora " + juce::String (now.attack * 1000.0f, 0) + " ms, antes " + juce::String (before.attack * 1000.0f, 0) + " ms");

        // Saved by 0.9.5: the envelope as it detected it.
        for (const auto& [id, v] : { std::pair { "ampA", before.attack * 1000.0f }, { "ampCurve", before.curve }, { "ampD", before.decay * 1000.0f },
                                    { "ampS", before.sustain }, { "ampFade", before.fadeDbPerSec }, { "ampR", before.release * 1000.0f } })
            setValue (a, id, v);
        juce::MemoryBlock state;
        a.getStateInformation (state);
        const auto reopen = [&] (int version)
        {
            auto xml = juce::AudioProcessor::getXmlFromBinary (state.getData(), (int) state.getSize());
            xml->setAttribute ("stateVersion", version);
            juce::MemoryBlock old;
            juce::AudioProcessor::copyXmlToBinary (*xml, old);
            auto q = std::make_unique<ReanimaProcessor>();
            q->prepareToPlay (48000.0, 512);
            q->setStateInformation (old.getData(), (int) old.getSize());
            waitFor ([&] { return q->getSample() != nullptr && ! q->isLoading(); });
            wait (100);
            q->serviceMessageThread();
            return q;
        };
        const auto old = reopen (4), recent = reopen (5);
        const auto& od = old->getSample()->detected;
        const bool converted = std::abs (old->paramValue (P::AmpA) - od.attack * 1000.0f) < 1.0f && std::abs (old->paramValue (P::AmpS) - od.sustain) < 1.0e-3f
                               && std::abs (old->paramValue (P::AmpD) - od.decay * 1000.0f) < 1.0f && std::abs (od.attack - now.attack) < 1.0e-4f;
        const bool kept = std::abs (recent->paramValue (P::AmpA) - before.attack * 1000.0f) < 1.0f;
        check (converted && kept, "un proyecto de 0.9.5 con la envolvente detectada abre con la detección nueva (suena el archivo tal cual)",
               "antiguo: ataque " + juce::String (old->paramValue (P::AmpA), 0) + " ms; actual con valores propios: "
                   + juce::String (recent->paramValue (P::AmpA), 0) + " ms");
        file.deleteFile();
    }
    {
        ReanimaProcessor p;
        p.prepareToPlay (48000.0, 512);
        p.loadSampleNow (sampleFile);
        waitFor ([&] { return ! p.isLoading(); });
        const auto settle = [&] { wait (200); p.serviceMessageThread(); waitFor ([&] { return ! p.isLoading(); }); wait (200); p.serviceMessageThread(); };
        setValue (p, "reverse", 1.0f);
        settle();
        const bool on = p.getSample()->reversed;
        p.applyFactoryPreset (0);
        settle();
        const bool presetClears = p.paramValue (P::Reverse) < 0.5f && ! p.getSample()->reversed;
        const bool undid = p.undo();
        std::printf ("   reverse: deshacer %d, param justo después %.0f\n",(int) undid, p.paramValue (P::Reverse));
        settle();
        const bool undoBack = p.paramValue (P::Reverse) > 0.5f;
        std::printf ("   reverse: tras deshacer param %.0f sample %d preset '%s'\n", p.paramValue (P::Reverse), (int) p.getSample()->reversed, p.getPresetName().toRawUTF8());
        p.loadSampleNow (sampleFile);
        settle();
        const bool sampleClears = p.paramValue (P::Reverse) < 0.5f && ! p.getSample()->reversed;
        p.applyFactoryPreset (factoryIndex ("Al revés"));
        settle();
        const bool presetSets = p.paramValue (P::Reverse) > 0.5f && p.getSample()->reversed;
        std::printf ("   reverse: al revés param %.0f sample %d, cargando %d\n", p.paramValue (P::Reverse), (int) p.getSample()->reversed, (int) p.isLoading());
        check (on && presetClears && undoBack && sampleClears && presetSets,
               "Reverse: no pasa a otro preset ni a otro sample; un preset que lo pide lo pone; deshacer lo devuelve",
               juce::String (on ? "on " : "OFF? ") + (presetClears ? "preset limpia " : "PRESET NO LIMPIA ") + (undoBack ? "deshacer ok " : "DESHACER NO ")
                   + (sampleClears ? "sample limpia " : "SAMPLE NO LIMPIA ") + (presetSets ? "preset al revés ok" : "PRESET AL REVÉS NO"));
    }
    {
        // Low end of the side signal (L-R) with Graves mono at 0 and at 150 Hz, on a wide preset.
        const auto sideLow = [&] (float hz)
        {
            ReanimaProcessor p;
            p.prepareToPlay (48000.0, 512);
            p.loadSampleNow (sampleFile);
            waitFor ([&] { return ! p.isLoading(); });
            p.applyFactoryPreset (factoryIndex ("Vaivén estéreo"));
            wait (200);
            p.serviceMessageThread();
            waitFor ([&] { return ! p.isLoading(); });
            setValue (p, "monoBass", hz);
            wait (100);
            p.serviceMessageThread();
            const auto a = playNote (p, 36, 2.0);
            double lo = 0.0, all = 0.0, z1 = 0.0, z2 = 0.0;
            const double k = 1.0 - std::exp (-2.0 * 3.14159265 * 60.0 / 48000.0);
            for (int i = 0; i < a.getNumSamples(); ++i)
            {
                const double s = 0.5 * (a.getSample (0, i) - a.getSample (1, i));
                z1 += k * (s - z1);
                z2 += k * (z1 - z2);
                lo += z2 * z2;
                all += (double) a.getSample (0, i) * a.getSample (0, i);
            }
            return 10.0 * std::log10 (std::max (1.0e-20, lo) / std::max (1.0e-12, all));
        };
        const double open = sideLow (0.0f), mono = sideLow (150.0f);
        check (mono < open - 12.0, "Graves mono: el lado (L-R) bajo 60 Hz baja con el control",
               juce::String (open, 1) + " dB -> " + juce::String (mono, 1) + " dB");
    }
    {
        // Volume at +12 dB with the filter drive up: the output never passes -0.6 dBFS. At the default
        // level a normal note stays under the -3 dBFS knee, untouched.
        ReanimaProcessor p;
        p.prepareToPlay (48000.0, 512);
        p.loadSampleNow (sampleFile);
        waitFor ([&] { return ! p.isLoading(); });
        const float def = p.paramValue (P::Volume);
        const auto quiet = playNote (p, 60, 1.5);
        setValue (p, "volume", 12.0f);
        setValue (p, "fltDrive", 24.0f);
        wait (100);
        p.serviceMessageThread();
        const auto loud = playNote (p, 48, 1.5, 1.0f);
        const float peakQuiet = quiet.getMagnitude (0, quiet.getNumSamples()), peakLoud = loud.getMagnitude (0, loud.getNumSamples());
        check (std::abs (def + 6.0f) < 0.01f && peakLoud <= 0.9334f && peakLoud > 0.5f && peakQuiet < 0.7079f,
               "Master: -6 dB por defecto; nota normal sin tocar; a +12 dB la salida no pasa de -0,6 dBFS",
               "defecto " + juce::String (def, 1) + " dB; pico normal " + juce::String (juce::Decibels::gainToDecibels (peakQuiet), 1)
                   + " dBFS; pico a +12 dB " + juce::String (juce::Decibels::gainToDecibels (peakLoud), 2) + " dBFS");
    }
    {
        // Favourites on this computer's settings; what was there is put back.
        auto& s = PresetBar::settingsFile();
        const juce::String name = "Prueba favorito " + juce::String (juce::Random::getSystemRandom().nextInt (100000)), other = name + " b";
        const auto useKey = [] (const juce::String& n) { return "uses_" + juce::String::toHexString (n.hashCode64()); };
        std::vector<std::pair<juce::String, juce::String>> saved;
        for (const char* key : { "favourites", "favouritesDismissed", "autoFavourites" })
            saved.push_back ({ key, s.getValue (key) });
        // The heart beside each preset in the list: a click on it marks it and a second one unmarks it; a click
        // on the name loads the preset and leaves the favourites alone.
        bool heartOn = false, heartOff = false, nameLeaves = false;
        {
            PresetItem item (name, "Prueba", false);
            item.setSize (260, 24);
            const auto click = [&item] (juce::Point<float> at)
            {
                const auto now = juce::Time::getCurrentTime();
                item.mouseUp (juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), at, {}, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                                &item, &item, now, at, now, 1, false));
            };
            const auto heart = item.heartArea().getCentre().toFloat();
            click (heart);
            heartOn = PresetBar::favourites().contains (name);
            click ({ 40.0f, 12.0f });
            nameLeaves = PresetBar::favourites().contains (name);
            click (heart);
            heartOff = ! PresetBar::favourites().contains (name);
        }
        // FAVS is the first category of the list, a submenu like the others, holding only the marked presets.
        bool favsFirst = false, onlyFavs = false, sameLevel = false;
        {
            ReanimaProcessor q;
            PresetBar bar (q);
            auto menu = bar.buildMenu ({ name, ReanimaProcessor::factoryPresetName (3) }, {});
            std::vector<const juce::PopupMenu::Item*> top;
            for (juce::PopupMenu::MenuItemIterator it (menu); it.next();)
                if (! it.getItem().isSectionHeader && ! it.getItem().isSeparator)
                    top.push_back (&it.getItem());
            if (top.size() > 2 && top[0]->subMenu != nullptr)
            {
                favsFirst = top[0]->text.contains ("FAVS");
                int marked = 0, others = 0;
                for (juce::PopupMenu::MenuItemIterator in (*top[0]->subMenu); in.next();)
                {
                    const auto& x = in.getItem();
                    if (x.isSeparator)
                        continue;
                    if (x.itemID >= 5000 && x.itemID <= 5001 && dynamic_cast<PresetItem*> (x.customComponent.get()) != nullptr)
                        ++marked;
                    else if (x.itemID != 4999)
                        ++others;
                }
                onlyFavs = marked == 2 && others == 0;
                sameLevel = top[1]->subMenu != nullptr && top[2]->subMenu != nullptr;
            }
        }
        check (favsFirst && onlyFavs && sameLevel, "FAVS es la primera categoría del navegador y sólo muestra los presets con corazón",
               juce::String (favsFirst ? "primera " : "NO ES LA PRIMERA ") + (onlyFavs ? "sólo favoritos " : "CONTENIDO MAL ")
                   + (sameLevel ? "mismo nivel" : "OTRO NIVEL"));

        s.setValue ("favouritesDismissed", saved[1].second);
        PresetBar::setFavourite (name, true);
        const bool added = PresetBar::favourites().contains (name);
        PresetBar::setFavourite (name, false);
        const bool removed = ! PresetBar::favourites().contains (name);
        for (int k = 0; k < 6; ++k)
            PresetBar::countUse (name);
        const bool dismissedStays = ! PresetBar::favourites().contains (name);
        s.setValue ("favouritesDismissed", saved[1].second);
        s.removeValue (useKey (name));
        for (int k = 0; k < 4; ++k)
            PresetBar::countUse (name);
        const bool fourNo = ! PresetBar::favourites().contains (name);
        PresetBar::countUse (name);
        const bool fiveYes = PresetBar::favourites().contains (name);
        s.setValue ("autoFavourites", false);
        for (int k = 0; k < 6; ++k)
            PresetBar::countUse (other);
        const bool offNo = ! PresetBar::favourites().contains (other);
        s.removeValue (useKey (name));
        s.removeValue (useKey (other));
        for (const auto& [key, value] : saved)
            if (value.isEmpty())
                s.removeValue (key);
            else
                s.setValue (key, value);
        s.saveIfNeeded();
        check (heartOn && nameLeaves && heartOff, "Favoritos: el corazón de cada preset de la lista lo marca y lo desmarca",
               juce::String (heartOn ? "marca " : "NO MARCA ") + (nameLeaves ? "el nombre no lo toca " : "EL NOMBRE LO QUITA ")
                   + (heartOff ? "desmarca" : "NO DESMARCA"));
        check (added && removed && dismissedStays && fourNo && fiveYes && offNo, "Favoritos: corazón, quitar a mano, automático a los 5 usos y desactivable",
               juce::String (added ? "añade " : "NO AÑADE ") + (removed ? "quita " : "NO QUITA ") + (dismissedStays ? "respeta quitado " : "VUELVE SOLO ")
                   + (fourNo && fiveYes ? "5 usos ok " : "USOS MAL ") + (offNo ? "apagado ok" : "APAGADO NO"));
    }
}

void checkLoading (const juce::File& sampleFile)
{
    // Closing the plugin in the middle of an analysis.
    for (int i = 0; i < 5; ++i)
    {
        ReanimaProcessor p;
        p.prepareToPlay (44100.0, 512);
        p.loadSample (sampleFile);
        p.loadSample (sampleFile);
    }
    check (true, "cerrar el plugin durante un análisis no falla");

    // Leaving engine A completes the analysis in the background, keeping the knobs.
    ReanimaProcessor p;
    p.prepareToPlay (48000.0, 512);
    p.loadSample (sampleFile);
    check (p.isLoading(), "la carga no bloquea", "");
    check (waitFor ([&] { return ! p.isLoading() && p.getSample() != nullptr; }), "la carga en segundo plano termina");
    check (! p.getSample()->forAllEngines, "el motor A carga sólo lo que necesita");

    auto* decay = p.apvts.getParameter ("ampD");
    decay->setValueNotifyingHost (0.7f);
    p.apvts.getParameter ("engine")->setValueNotifyingHost (0.2f);     // B · Reenvolvente
    check (waitFor ([&] { return p.getSample()->forAllEngines && ! p.isLoading(); }), "cambiar de motor completa el análisis");
    check (std::abs (decay->getValue() - 0.7f) < 1.0e-5f, "completar el análisis respeta los mandos");

    // The padlock keeps the envelope through a new sample; open, the sample's own values come in.
    p.apvts.getParameter ("envLock")->setValueNotifyingHost (1.0f);
    decay->setValueNotifyingHost (0.42f);
    p.loadSample (sampleFile);
    waitFor ([&] { return ! p.isLoading(); });
    check (std::abs (decay->getValue() - 0.42f) < 1.0e-5f, "con el candado cerrado, un sample nuevo no cambia el ADSR");
    p.apvts.getParameter ("envLock")->setValueNotifyingHost (0.0f);
    p.loadSample (sampleFile);
    waitFor ([&] { return ! p.isLoading(); });
    check (std::abs (p.paramValue (P::AmpD) - p.getSample()->detected.decay * 1000.0f) < 1.0f, "con el candado abierto, toma el decay detectado");

    p.apvts.getParameter ("reverse")->setValueNotifyingHost (1.0f);
    check (waitFor ([&] { return p.getSample()->reversed && ! p.isLoading(); }), "Reverse reanaliza en segundo plano");

    const auto broken = sampleFile.getSiblingFile ("roto.wav");
    broken.replaceWithText ("esto no es audio");
    p.loadSample (broken);
    waitFor ([&] { return ! p.isLoading(); });
    check (p.takeLoadError().isNotEmpty(), "un archivo dañado da un aviso");
    broken.deleteFile();
}
}

int main (int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI init;
    std::setvbuf (stdout, nullptr, _IONBF, 0);
    const juce::File out (argc > 1 ? juce::String (argv[1]) : juce::File::getCurrentWorkingDirectory().getFullPathName());
    out.createDirectory();

    ReanimaProcessor proc;
    proc.prepareToPlay (48000.0, 512);
    const auto sampleFile = writePluck (out);
    std::printf ("load: %s\n", proc.loadSampleNow (sampleFile) ? "ok" : "FAILED");

    // Play a note so the playhead shows, with the filter on and a few rack effects.
    if (auto* f = proc.apvts.getParameter ("fltOn"))
        f->setValueNotifyingHost (1.0f);
    proc.setFxSlotType (0, Fx::Distortion);
    proc.setFxSlotType (1, Fx::Reverb);
    proc.setFxSlotType (P::fxMainSlots, Fx::Delay);
    proc.setFxSlotType (P::fxMainSlots + P::fxBusSlots, Fx::SplitLMH);
    proc.moveFxSlot (1, 0, 0);   // reverb before distortion

    juce::AudioBuffer<float> buf (2, 512);
    for (int b = 0; b < 30; ++b)
    {
        juce::MidiBuffer midi;
        if (b == 0)
            midi.addEvent (juce::MidiMessage::noteOn (1, 57, 1.0f), 0);
        proc.processBlock (buf, midi);
    }
    std::printf ("peak after 30 blocks: %.3f\n", buf.getMagnitude (0, 512));

    ReanimaEditor::offerWelcome = false;       // the tour is checked on its own (checkWelcome)
    checkWelcome();
    std::unique_ptr<juce::AudioProcessorEditor> editor (proc.createEditor());
    editor->setSize (ReanimaEditor::designW, ReanimaEditor::designH);
    editor->setVisible (true);

    auto* tabs = findChild<juce::TabbedComponent> (*editor);
    const char* names[] = { "1_sample", "2_edicion", "3_filtro_mod", "4_matriz", "5_fx", "6_arp_seq" };
    auto shot = [&] (const juce::String& name)
    {
        const auto img = editor->createComponentSnapshot (editor->getLocalBounds(), true, 1.0f);
        auto file = out.getChildFile (name + ".png");
        file.deleteFile();
        juce::FileOutputStream os (file);
        juce::PNGImageFormat().writeImageToStream (img, os);
    };

    wait (200);
    for (int t = 0; t < tabs->getNumTabs(); ++t)
    {
        tabs->setCurrentTabIndex (t);
        wait (100);
        shot (names[t]);
    }

    // iman <loop.wav>: a loop in LOOP with a hit turned off and one added (SAMPLE), and EDICIÓN with the magnet on, at
    // 100 % and 75 %.
    if (argc > 3 && juce::String (argv[2]) == "iman")
    {
        proc.loadSampleNow (juce::File (juce::String::fromUTF8 (argv[3])));
        for (int k = 0; k < 600 && proc.isLoading(); ++k)
        {
            juce::Thread::sleep (10);
            proc.serviceMessageThread();
        }
        proc.serviceMessageThread();
        if (auto* p = proc.apvts.getParameter ("playMode"))
            p->setValueNotifyingHost (p->convertTo0to1 (1.0f));
        wait (300);
        const auto cuts = proc.getBeatCuts().empty() && proc.getSample() != nullptr ? proc.getSample()->loopInfo.slices : proc.getBeatCuts();
        HitEdits edits;
        if (cuts.size() > 4 && proc.getSample() != nullptr)
        {
            const int tol = HitEdits::tolerance (proc.getSample()->sampleRate);
            edits.toggle (cuts[3], true, tol);
            edits.add ((cuts[1] + cuts[2]) / 2, tol);
        }
        proc.setHitEdits (edits);
        std::printf ("golpes: %d cortes, correcciones %s\n", (int) proc.getBeatCuts().size(), edits.toString().toRawUTF8());
        wait (300);
        tabs->setCurrentTabIndex (0);
        wait (200);
        shot ("iman_1_sample_loop_golpes");
        // Ventana: a bar from beat 5 (bar 2)
        if (auto* p = proc.apvts.getParameter ("loopWinLen"))
            p->setValueNotifyingHost (p->convertTo0to1 (3.0f));
        if (auto* p = proc.apvts.getParameter ("loopWinPos"))
            p->setValueNotifyingHost (p->convertTo0to1 (4.0f));
        wait (300);
        shot ("iman_4_sample_ventana");
        if (auto* p = proc.apvts.getParameter ("loopWinLen"))
            p->setValueNotifyingHost (0.0f);
        wait (100);
        tabs->setCurrentTabIndex (1);         // the page is made when its tab opens
        wait (200);
        if (auto* view = findChild<EditView> (*editor))
        {
            view->setSnapsToGrid (true);
            if (view->onMagnetChanged)
                view->onMagnetChanged();
            view->selectAll();
        }
        else
            std::printf ("sin vista de EDICIÓN\n");
        wait (200);
        shot ("iman_2_edicion_100");
        editor->setSize (juce::roundToInt (ReanimaEditor::designW * 0.75), juce::roundToInt (ReanimaEditor::designH * 0.75));
        wait (250);
        shot ("iman_3_edicion_75");
        // Aros: Macro 1 to the cutoff and to Drive, Macro 2 to the resonance (negative), with no note.
        editor->setSize (ReanimaEditor::designW, ReanimaEditor::designH);
        for (const auto& [id, v] : { std::pair<const char*, float> { "fltOn", 1.0f }, { "fltFreq", 800.0f }, { "m1Src", (float) P::SrcMacro1 },
                                     { "m1Dst", (float) P::DstCutoff }, { "m1Amt", 0.5f }, { "m2Src", (float) P::SrcMacro2 }, { "m2Dst", (float) P::DstRes },
                                     { "m2Amt", -0.6f }, { "m3Src", (float) P::SrcMacro1 }, { "m3Dst", (float) P::DstDrive }, { "m3Amt", 0.4f },
                                     { "macro1", 0.7f }, { "macro2", 0.5f } })
            if (auto* p = proc.apvts.getParameter (id))
                p->setValueNotifyingHost (p->convertTo0to1 (v));
        juce::AudioBuffer<float> silent (2, 512);
        juce::MidiBuffer none;
        silent.clear();
        proc.processBlock (silent, none);
        tabs->setCurrentTabIndex (2);
        wait (400);
        shot ("aros_1_filtro_mod");
        // Explorador: open, on its first source (Live's folders of this computer)
        std::function<juce::Button* (juce::Component&)> findBrowse = [&] (juce::Component& c) -> juce::Button*
        {
            for (auto* child : c.getChildren())
            {
                if (child->getName() == "explorar")
                    return dynamic_cast<juce::Button*> (child);
                if (auto* b = findBrowse (*child))
                    return b;
            }
            return nullptr;
        };
        if (auto* b = findBrowse (*editor))
        {
            b->triggerClick();
            wait (1500);
            shot ("explorador_1");
        }
        else
            std::printf ("sin botón del explorador\n");
        return 0;
    }

    // escalas: every tab at the sizes the menu offers below 100 % (and 100 %), to judge text and colour.
    if (argc > 2 && juce::String (argv[2]) == "escalas")
    {
        for (const int pct : { 100, 90, 80, 75, 70, 60 })
        {
            editor->setSize (juce::roundToInt (ReanimaEditor::designW * pct / 100.0), juce::roundToInt (ReanimaEditor::designH * pct / 100.0));
            wait (150);
            for (int t = 0; t < tabs->getNumTabs(); ++t)
            {
                tabs->setCurrentTabIndex (t);
                wait (80);
                shot ("esc" + juce::String (pct) + "_" + names[t]);
            }
        }
        return 0;
    }

    // FX: the effect chosen in each chain, in that chain's colour (its page is there while its tab is open).
    tabs->setCurrentTabIndex (4);
    wait (100);
    if (auto* fx = findChild<FxPage> (*editor))
    {
        tabs->setCurrentTabIndex (4);
        for (const int slot : { P::fxMainSlots, P::fxMainSlots + P::fxBusSlots, 0 })
        {
            fx->select (slot);
            wait (120);
            shot (juce::String ("5_fx_ranura_") + juce::String (slot + 1));
        }
    }

    tabs->setCurrentTabIndex (0);
    checkLayout (proc, *editor, *tabs);
    checkWaveform (proc, *editor);
    checkMeter (proc, *editor);

    if (auto* engine = proc.apvts.getParameter ("engine"))
    {
        const int engines = P::choicesOf (P::Engine).size();
        for (int e = 0; e < engines; ++e)
        {
            engine->setValueNotifyingHost ((float) e / (float) (engines - 1));
            wait (150);
            shot ("7_motor_" + juce::String::charToString ((juce::juce_wchar) ('a' + e)));
        }
        engine->setValueNotifyingHost (0.0f);

        // Engine A with a long decay held: the result goes on from the sample without a step.
        {
            const char* ids[] = { "sustainType", "ampD", "ampS", "ampCurve" };
            const float values[] = { 1.0f, 5280.0f, 0.14f, 2.56f };
            float before[4];
            for (int i = 0; i < 4; ++i)
            {
                auto* param = proc.apvts.getParameter (ids[i]);
                before[i] = param->getValue();
                param->setValueNotifyingHost (param->convertTo0to1 (values[i]));
            }
            wait (300);
            shot ("7_motor_a_decay_largo");
            for (int i = 0; i < 4; ++i)
                proc.apvts.getParameter (ids[i])->setValueNotifyingHost (before[i]);
            wait (300);
        }

        proc.apvts.getParameter ("envMode")->setValueNotifyingHost (1.0f);
        wait (300);
        shot ("8_sintetizador");
        check (std::abs (proc.paramValue (P::AmpS) - 0.6f) < 0.01f, "pasar a Sintetizador pone valores de sinte", juce::String (proc.paramValue (P::AmpS), 2));
        proc.apvts.getParameter ("envMode")->setValueNotifyingHost (0.0f);
        wait (300);
        check (std::abs (proc.paramValue (P::AmpD) - proc.getSample()->detected.decay * 1000.0f) < 1.0f, "volver a Remodelar recupera lo detectado");
    }

    // A project saved before Soplo existed, with "Del sample": it opens with "Del sample".
    {
        juce::MemoryBlock saved;
        proc.getStateInformation (saved);
        auto xml = juce::AudioProcessor::getXmlFromBinary (saved.getData(), (int) saved.getSize());
        auto tree = juce::ValueTree::fromXml (*xml);
        tree.setProperty ("stateVersion", 3, nullptr);
        tree.getChildWithProperty ("id", "mechType").setProperty ("value", 4, nullptr);
        juce::MemoryBlock old;
        juce::AudioProcessor::copyXmlToBinary (*tree.createXml(), old);
        proc.setStateInformation (old.getData(), (int) old.getSize());
        wait (300);
        check (juce::roundToInt (proc.paramValue (P::MechType)) == 5, "un proyecto antiguo con Del sample sigue en Del sample",
               juce::String (proc.paramValue (P::MechType)));
        proc.setStateInformation (saved.getData(), (int) saved.getSize());
        wait (300);
    }

    // A/B: each setting keeps what was done in it, and with "= nivel" the louder one is heard as
    // loud as the other.
    {
        auto* volume = proc.apvts.getParameter ("volume");
        const auto loudnessOfNotes = [&]
        {
            juce::AudioBuffer<float> b (2, 512);
            double sum = 0.0;
            int count = 0;
            for (int k = 0; k < 160; ++k)
            {
                juce::MidiBuffer midi;
                if (k % 20 == 0)
                    midi.addEvent (juce::MidiMessage::noteOn (1, 57, 1.0f), 0);
                if (k % 20 == 15)
                    midi.addEvent (juce::MidiMessage::noteOff (1, 57), 0);
                proc.processBlock (b, midi);
                if (k >= 80)
                {
                    const double r = b.getRMSLevel (0, 0, 512);
                    sum += r * r;
                    ++count;
                }
            }
            return 10.0 * std::log10 (sum / count + 1.0e-20);
        };
        const float volumeA = volume->getValue();
        proc.compareSelect (1);                         // B starts as A
        volume->setValueNotifyingHost (volume->convertTo0to1 (volume->convertFrom0to1 (volumeA) + 6.0f));
        wait (300);
        const float volumeB = volume->getValue();
        proc.compareSelect (0);
        const bool backToA = std::abs (volume->getValue() - volumeA) < 1.0e-4f;
        proc.matchLevels = false;
        const double a = loudnessOfNotes();
        proc.compareSelect (1);
        const bool backToB = std::abs (volume->getValue() - volumeB) < 1.0e-4f;
        const double bRaw = loudnessOfNotes();
        proc.matchLevels = true;
        const double bMatched = loudnessOfNotes();
        proc.compareSelect (0);
        check (backToA && backToB && bRaw - a > 5.0 && std::abs (bMatched - a) < 1.0, "A/B: cada ajuste guarda lo suyo y = nivel los iguala",
               "B sin igualar " + juce::String (bRaw - a, 1) + " dB sobre A, igualado " + juce::String (bMatched - a, 1) + " dB");
    }

    if (auto* start = proc.apvts.getParameter ("startPos"))
        start->setValueNotifyingHost (0.12f);
    wait (100);
    shot ("6_inicio");
    if (auto* start = proc.apvts.getParameter ("startPos"))
        start->setValueNotifyingHost (0.0f);

    // TIMBRE says how well each gesture suits the sample, in the menu and on the line under it.
    {
        auto* advice = findChild<MechanismAdvice> (*editor);
        juce::String menuText;
        std::function<void (juce::Component&)> find = [&] (juce::Component& c)
        {
            if (auto* box = dynamic_cast<juce::ComboBox*> (&c))
                if (box->getNumItems() == P::choicesOf (P::MechType).size() && box->getItemText (0).contains ("Piano"))
                    menuText = box->getText() + " | " + box->getItemText (0);
            for (auto* child : c.getChildren())
                find (*child);
        };
        find (*editor);
        check (advice != nullptr && menuText.contains ("%"), "TIMBRE dice cuánto encaja cada gesto de Mecanismo", menuText);
    }

    checkTooltips (proc, *editor, *tabs);

    if (auto* reanima = dynamic_cast<ReanimaEditor*> (editor.get()))
    {
        // The guide of each tab: its "?" opens it, every step finds its real control on screen, the bubble
        // stays inside the window and off the control, the control takes its own clicks, the three levels
        // are there, and nothing in the sound changes.
        auto& guide = reanima->getGuide();
        const auto sampleBefore = proc.getSample();
        std::vector<float> before;
        for (auto* p : proc.getParameters())
            before.push_back (p->getValue());
        const auto window = juce::Rectangle<float> (0, 0, (float) ReanimaEditor::designW, (float) ReanimaEditor::designH);
        int stepsChecked = 0;
        // Every control on screen (knob, menu, switch, button, slider) is either wholly inside what a step
        // lights up or wholly outside it: none is cut by the outline. And what is outlined inside lies
        // within the panel.
        auto* surface = guide.getParentComponent();
        const auto cutControls = [&]
        {
            juce::String cut;
            const auto target = guide.targetArea();
            if (target.isEmpty())
                return cut;
            std::function<void (juce::Component&)> walk = [&] (juce::Component& c)
            {
                for (auto* child : c.getChildren())
                {
                    if (! child->isVisible() || child == &guide)
                        continue;
                    const bool control = child->getComponentID().startsWith ("p:") || dynamic_cast<juce::Slider*> (child) != nullptr
                                      || dynamic_cast<juce::ComboBox*> (child) != nullptr || dynamic_cast<juce::Button*> (child) != nullptr;
                    if (control && dynamic_cast<juce::TabBarButton*> (child) == nullptr && child->getComponentID() != "tabguide:0"
                        && ! child->getComponentID().startsWith ("tabguide:"))
                    {
                        const auto r = surface->getLocalArea (child, child->getLocalBounds());
                        if (r.intersects (target) && ! target.contains (r))
                        {
                            auto name = child->getComponentID();
                            if (auto* b = dynamic_cast<juce::Button*> (child))
                                name = b->getButtonText();
                            cut << "corta " << (name.isEmpty() ? juce::String ("un control") : name) << " ";
                        }
                    }
                    walk (*child);
                }
            };
            walk (*surface);
            for (const auto& f : guide.focusAreas())
                if (! guide.spotArea().contains (f.reduced (3.0f)))
                    cut << "foco fuera del panel ";
            return cut;
        };
        for (int t = 0; t < TabGuides::numTabs; ++t)
        {
            juce::Button* dot = nullptr;
            std::function<void (juce::Component&)> findDot = [&] (juce::Component& c)
            {
                if (c.getComponentID() == "tabguide:" + juce::String (t))
                    dot = dynamic_cast<juce::Button*> (&c);
                for (auto* child : c.getChildren())
                    findDot (*child);
            };
            findDot (*editor);
            check (dot != nullptr && dot->isVisible() && dot->getWidth() > 10, "la pestaña " + TabGuides::tabName (t) + " tiene su ?");
            if (dot == nullptr)
                continue;
            if (t > 0)
                tabs->setCurrentTabIndex (0);
            dot->triggerClick();
            wait (60);
            wait (60);
            check (guide.isRunning() && guide.tourName() == TabGuides::tabName (t) && tabs->getCurrentTabIndex() == t,
                   "el ? de " + TabGuides::tabName (t) + " abre su guía en su pestaña");

            bool levels[4] {};
            juce::String bad;
            for (int s = 0; s < guide.stepCount(); ++s)
            {
                guide.showStep (s);
                ++stepsChecked;
                const auto spot = guide.spotArea(), bubble = guide.bubbleArea();
                const bool big = spot.getWidth() * spot.getHeight() > 0.3f * window.getWidth() * window.getHeight();
                const auto centre = spot.getCentre().roundToInt();
                juce::String why;
                if (spot.isEmpty())
                    why = "no encuentra el control";
                else if (! window.contains (spot.reduced (6.0f)))
                    why = "resaltado fuera de la ventana";
                else if (! window.contains (bubble))
                    why = "burbuja fuera de la ventana";
                else if (! big && bubble.intersects (spot))
                    why = "la burbuja tapa el control";
                else if (! big && guide.hitTest (centre.x, centre.y))
                    why = "el control resaltado no recibe clics";
                else
                    why = cutControls();
                if (why.isNotEmpty())
                    bad << "paso " << (s + 1) << ": " << why << "; ";
            }
            // Levels, by walking with the arrows from the start (as a user does) and taking a picture of
            // the first step of each.
            guide.showStep (0);
            int lastLevel = 0;
            for (int s = 0; s < guide.stepCount(); ++s)
            {
                const int level = guide.levelOf (s);
                if (level >= 1 && level <= 3)
                    levels[level] = true;
                if (level != lastLevel)
                {
                    wait (40);
                    shot ("10_guia_" + juce::String (t + 1) + "_nivel_" + juce::String (level));
                    lastLevel = level;
                }
                check (guide.currentStep() == s, "las flechas avanzan paso a paso");
                guide.keyPressed (juce::KeyPress (juce::KeyPress::rightKey));
                if (s + 1 < guide.stepCount())
                    wait (10);
            }
            check (bad.isEmpty(), "guía de " + TabGuides::tabName (t) + ": " + juce::String (guide.stepCount()) + " pasos con su control", bad);
            check (levels[1] && levels[2] && levels[3], "guía de " + TabGuides::tabName (t) + ": Fundamentos, Práctica y Avanzado");
            check (! guide.isRunning(), "Terminar en el último paso cierra la guía de " + TabGuides::tabName (t));

            // Back, and close with the dot again and with Esc.
            dot->triggerClick();
            wait (60);
            guide.showStep (2);
            guide.keyPressed (juce::KeyPress (juce::KeyPress::leftKey));
            check (guide.currentStep() == 1, "Anterior vuelve un paso");
            dot->triggerClick();
            wait (60);
            check (! guide.isRunning(), "el ? de la pestaña cierra su guía");
            dot->triggerClick();
            wait (60);
            guide.keyPressed (juce::KeyPress (juce::KeyPress::escapeKey));
            check (! guide.isRunning(), "Esc cierra la guía de la pestaña");
        }
        bool same = true;
        int k = 0;
        for (auto* p : proc.getParameters())
            same = same && std::abs (p->getValue() - before[(size_t) k++]) < 1.0e-6f;
        check (same, "las guías no cambian ningún ajuste", juce::String (stepsChecked) + " pasos recorridos");
        check (proc.getSample() == sampleBefore && ! proc.isLoading(), "abrir y cerrar las guías no carga ni cambia el sample");
        tabs->setCurrentTabIndex (0);
    }

    if (auto* reanima = dynamic_cast<ReanimaEditor*> (editor.get()))
    {
        auto* guide = &reanima->getGuide();
        reanima->startQuickGuide();
        wait (100);
        {
            // The quick guide: every step whole, and lit up the same at another size of the window.
            juce::String bad;
            auto* surface = guide->getParentComponent();
            std::vector<juce::Rectangle<int>> at100;
            for (int s = 0; s < guide->stepCount(); ++s)
            {
                guide->showStep (s);
                at100.push_back (guide->targetArea());
                const auto target = guide->targetArea();
                if (s > 0 && target.isEmpty())
                    bad << "paso " << s << ": no encuentra su panel; ";
                std::function<void (juce::Component&)> walk = [&] (juce::Component& c)
                {
                    for (auto* child : c.getChildren())
                    {
                        if (! child->isVisible() || child == guide)
                            continue;
                        const bool control = child->getComponentID().startsWith ("p:") || dynamic_cast<juce::Slider*> (child) != nullptr
                                          || dynamic_cast<juce::ComboBox*> (child) != nullptr || (dynamic_cast<juce::Button*> (child) != nullptr
                                              && dynamic_cast<juce::TabBarButton*> (child) == nullptr && ! child->getComponentID().startsWith ("tabguide:"));
                        if (control && ! target.isEmpty())
                        {
                            const auto r = surface->getLocalArea (child, child->getLocalBounds());
                            if (r.intersects (target) && ! target.contains (r))
                                bad << "paso " << s << ": corta " << child->getComponentID() << "; ";
                        }
                        walk (*child);
                    }
                };
                walk (*surface);
            }
            const auto size = editor->getBounds();
            editor->setSize (ReanimaEditor::designW * 3 / 4, ReanimaEditor::designH * 3 / 4);
            wait (60);
            for (int s = 0; s < guide->stepCount(); ++s)
            {
                guide->showStep (s);
                if (guide->targetArea() != at100[(size_t) s])
                    bad << "paso " << s << ": cambia al 75 %; ";
            }
            editor->setBounds (size);
            wait (60);
            guide->showStep (0);
            check (bad.isEmpty(), "guía rápida: cada paso resalta su panel entero, también al 75 %", bad);
        }
        shot ("9_guia_1");
        for (int i = 0; i < 3; ++i)
            guide->keyPressed (juce::KeyPress (juce::KeyPress::rightKey));
        wait (100);
        shot ("9_guia_4");
        for (int i = 0; i < 5; ++i)
            guide->keyPressed (juce::KeyPress (juce::KeyPress::rightKey));
        wait (100);
        shot ("9_guia_9");
        check (guide->isRunning(), "la guía recorre sus pasos");
        guide->keyPressed (juce::KeyPress (juce::KeyPress::escapeKey));
        check (! guide->isRunning(), "Esc cierra la guía");
        tabs->setCurrentTabIndex (0);
    }

    {
        // EXPORTAR is gone, and so is the heart that stood between the presets and the die.
        int exports = 0, hearts = 0;
        std::function<void (juce::Component&)> walk = [&] (juce::Component& c)
        {
            exports += c.getComponentID() == "export" ? 1 : 0;
            hearts += dynamic_cast<juce::Button*> (&c) != nullptr && c.getName() == "Favorito" ? 1 : 0;
            for (auto* child : c.getChildren())
                walk (*child);
        };
        walk (*editor);
        check (exports == 0 && hearts == 0, "sin botón EXPORTAR ni corazón suelto en la barra de presets",
               juce::String (exports) + " exportar, " + juce::String (hearts) + " corazones");
    }

    editor.reset();

    checkState (proc, sampleFile);
    checkPresets (proc, out.getChildFile ("presets"));
    checkFactory (sampleFile);
    checkHistory (sampleFile);
    checkEditing (sampleFile);
    checkLoading (sampleFile);
    checkDemo (sampleFile);
    checkMpe (sampleFile);
    checkDiceAndSnap (sampleFile);
    checkEngineSwitch (sampleFile);
    checkAudit (sampleFile);
    checkFreshSample (sampleFile, out);
    checkDeletePreset (sampleFile, out.getChildFile ("presets_borrar"));
    checkDrop (sampleFile, out.getChildFile ("arrastre"));
    checkLoopMode (out.getChildFile ("loop"));

    std::printf ("\n%s\n", failures == 0 ? "UI OK" : "UI CON FALLOS");
    return failures == 0 ? 0 : 1;
}
