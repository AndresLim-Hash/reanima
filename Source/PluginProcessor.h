#pragma once

#include "DroneStretch.h"
#include "Params.h"
#include "SampleAnalysis.h"
#include "MasterStage.h"
#include "SynthEngine.h"
#include "DspUtil.h"
#include "FactoryPresets.h"

class ReanimaProcessor : public juce::AudioProcessor,
                         public juce::ChangeBroadcaster,
                         private juce::AsyncUpdater,
                         private juce::Timer,
                         private juce::AudioProcessorValueTreeState::Listener,
                         private juce::AudioProcessorParameter::Listener
{
public:
    ReanimaProcessor();
    ~ReanimaProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool supportsMPE() const override { return true; }     // the host may send MPE (and VST3 note expression)
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override;

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return "Reanima"; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    //==============================================================================
    // Message thread. The analysis takes a while, so it runs in the background: the sample
    // arrives through a change message. Offline tools can wait for it with loadSampleNow().
    void loadSample (const juce::File& file);
    bool loadSampleNow (const juce::File& file);
    bool isLoading() const noexcept { return loading.load(); }
    // Offline tools without a message loop: what the timer and the async updates would do now.
    void serviceMessageThread() { timerCallback(); handleAsyncUpdate(); }
    juce::String takeLoadError();

    // A new, empty instance plays a demonstration sample (built into the plugin) so there is something to
    // play and to follow the guides with. Only once, only if nothing else came first: a project being
    // opened, a preset, a sample loaded by the user. Tools that make their own instances can turn it off.
    static inline bool offerDemo = true;
    static constexpr const char* demoName = "Demo · synth pluck";
    // Only when there is no audio at all: a project carries its sample inside.
    bool sampleIsMissing() const { return uiSample == nullptr && sampleFile != juce::File() && ! sampleFile.existsAsFile(); }
    juce::String sampleFileName() const { return sampleFile.getFileName(); }
    std::shared_ptr<const SampleData> getSample() const { return uiSample; }
    void resetEnvelopeToDetected();

    // A/B: two settings of the sound (every knob, not the sample) to compare. Both start as the
    // sound there was; each keeps what is done while it is chosen. With matchLevels the louder of
    // the two is turned down to the other's loudness (K-weighted, as EBU R128 measures it), so that
    // the louder one does not win only for being louder. Message thread; not saved.
    void compareSelect (int slot);
    int compareSlot() const noexcept { return activeSlot; }
    bool compareStarted() const noexcept { return slotsUsed; }
    float compareTrimDb() const noexcept { return juce::Decibels::gainToDecibels (matchGain.load(), -60.0f); }
    std::atomic<bool> matchLevels { true };

    // The drawn filter of engine F, in dB per point (Drone::curvePoints). Saved with the project.
    float getDroneCurve (int point) const noexcept { return droneCurve[(size_t) point].load(); }
    void setDroneCurve (int point, float db) noexcept;
    void snapshot (ParamSnapshot& out) const;

    // Editing the sample itself (message thread). Positions are samples of the sample as it is shown,
    // reversed or not. The original file is never touched: the edited audio is kept in the project.
    enum class Edit { Trim, Delete, Silence, Cut, Paste, Reverse, FadeIn, FadeOut, Normalize, Gain, SeamlessLoop };
    bool edit (Edit what, int from, int to, float amount = 0.0f);
    void copyAudio (int from, int to);
    bool hasClipboard() const noexcept { return clipboard.getNumSamples() > 0; }
    bool isEdited() const noexcept { return edited; }

    // Undo and redo of what the user does: knobs and menus, presets, loaded samples and edits.
    // A knob turned by hand is one step; automation from the host is not recorded.
    bool undo();
    bool redo();
    bool canUndo() const noexcept { return historyPos > 0; }
    bool canRedo() const noexcept { return historyPos < (int) history.size(); }
    void noteUserChange() noexcept { touched = true; }      // for changes made without a parameter gesture

    // What the output did since the last call, for the meter.
    struct MeterReading { std::array<float, 2> peak {}, meanSquare {}; };
    MeterReading takeMeterReading();

    // Presets: the whole state, sample path included, in a file the user names (message thread).
    static constexpr const char* presetExtension = ".reanimapreset";
    juce::File getPresetFolder() const;
    void setPresetFolder (const juce::File& folder) { presetFolder = folder; }
    juce::Array<juce::File> getPresets() const;
    // Moves a preset of the user's to the computer's trash (deleted if there is none). Only files of the
    // preset folder with the preset extension: the factory ones are not files and cannot be reached.
    // What is loaded stays as it is: a project carries its whole state, not a link to the file.
    bool deletePreset (const juce::File& file);
    bool savePreset (const juce::String& name);
    bool loadPreset (const juce::File& file);
    juce::String getPresetName() const { return apvts.state.getProperty ("presetName").toString(); }

    // Factory presets: templates that keep the loaded sample (see FactoryPresets.h).
    void applyFactoryPreset (int index);

    // The dice: a random sound (Randomizer), checked and levelled by playing a note offline first; a sound
    // that comes out silent or far too loud is thrown away and another made. One step of the history.
    void randomize();

    // Snap a C as the user's own choice: once turned on it stays on through presets, the dice, other
    // changes and saving, and is saved with the project. Off, each preset decides as before.
    void setSnapPreference (bool on);
    bool snapPinned() const { return (bool) apvts.state.getProperty ("snapPinned", false); }
    // The host's tempo as the last block saw it; 0 when the host gives none.
    double getHostBpm() const noexcept { return lastBpm.load(); }

    // SLICE: where each slice starts (samples of the sample, from 0), as the engine plays them; the first key
    // (Primera nota) plays the first. Moving, adding or removing a marker by hand turns Cortes to "A mano".
    std::vector<int> getSlices() const { return slicePoints; }
    // The loop as a MIDI clip of its slices (a temporary .mid), for dragging into the DAW; empty if none.
    juce::File writeSliceMidi() const;
    // EDICIÓN: the order of the slices on the keys (key k plays slice order[k]); saved with the project.
    std::vector<int> getSliceOrder() const { return sliceOrder; }
    void setSliceOrder (const std::vector<int>& order);
    struct SliceNote { int key; double beat, length; };
    std::vector<SliceNote> sliceSequence (double& bpm, double& beats) const;
    // EXPORTAR AUDIO: what the plug-in plays, rendered offline (format 0 WAV, 1 AIFF, 2 FLAC; bits 16/24/32).
    bool renderExport (const juce::File& out, int format, double rate, int bits, juce::String& error) const;
    void setSliceMarks (std::vector<int> marks);
    // The loop's length in beats as LOOP plays it (BPM original, Tiempos, or what was detected); 0 unknown.
    double loopBeatsNow() const;

    // The welcome tour belongs to the instance: shown once when it is new, closed, never again in it (through
    // presets, closing and opening the window, saving and reopening the project, duplicating the track). A new
    // instance on another track starts over. Kept in the instance's own state, not in a preference of the computer.
    bool welcomeShown() const { return (bool) apvts.state.getProperty ("welcomeShown", false); }
    void setWelcomeShown() { apvts.state.setProperty ("welcomeShown", true, nullptr); }
    static juce::String factoryPresetName (int index);

    // FX rack editing (message thread).
    void setFxSlotType (int slot, int type);
    void moveFxSlot (int fromSlot, int toChain, int toIndex);
    float paramValue (int index) const { return paramPtrs[(size_t) index]->load(); }

    juce::AudioProcessorValueTreeState apvts;
    SynthEngine engine;

private:
    using Audio = std::shared_ptr<const juce::AudioBuffer<float>>;

    // What a load has to do, and what it produced.
    struct Load
    {
        int id = 0;
        juce::File file;                                        // read it, unless audio is already there
        std::shared_ptr<const juce::AudioBuffer<float>> audio;
        double rate = 44100.0;
        juce::String name;
        bool reverse = false, forAllEngines = true, applyDetected = true;
        bool sameSound = false;                                 // only more analysis of the sample that sounds
        float oldStart = -1.0f;                                 // "Inicio" saved by an earlier version
        bool earlierDetection = false;                          // saved before the attack of a swelling note was read where it settles
        bool edit = false, structural = false, followDetected = false;  // an edit, one that moved the timing, an envelope left as detected
        juce::String encoded;                                   // the audio as saved in a project, to decode
        bool record = false;                                    // a step of the history, made by the user
        bool fresh = false;                                     // a sample the user loaded: a session of its own starts
        Audio before;                                           // ...and what it replaces
        double rateBefore = 44100.0;
        juce::String nameBefore;
        juce::File fileBefore;
        bool editedBefore = false;
        std::shared_ptr<SampleData> data;
    };

    juce::AudioBuffer<float> shownAudio() const;
    void rememberCurrentSample (Load& load) const;
    void installEdit (Audio audio, bool structural);
    bool envelopeIsDetected() const;
    static juce::String encodeAudio (const juce::AudioBuffer<float>& audio, double rate);
    static bool decodeAudio (const juce::String& text, juce::AudioBuffer<float>& audio, double& rate);
    juce::String savedAudio();
    void encodeInBackground();

    bool needsAllEngines() const;
    void startLoad (Load load);
    static void runLoad (Load& load);
    void finishLoad (Load& load);
    void install (std::shared_ptr<SampleData> data, bool applyDetected, bool sameSound = false);
    void keepLingering();
    void applyDetected (const SampleData& data);
    void applySynthEnvelope();
    void startFresh();
    void applyLoopGate();
    void setParam (int index, float value);
    void parameterChanged (const juce::String& parameterID, float newValue) override;
    void measureOutput (const juce::AudioBuffer<float>& buffer) noexcept;
    void matchLoudness (juce::AudioBuffer<float>& buffer) noexcept;
    void handleAsyncUpdate() override;
    void timerCallback() override;
    void loadDemo();
    void saturateOutput (juce::AudioBuffer<float>& buffer) noexcept;
    MasterStage masterStage;
    static constexpr int monoScratchSize = 256;
    std::array<float, monoScratchSize> monoScratch {};
    void applySetup (const std::vector<Factory::Setting>& values, const std::vector<std::pair<int, float>>& effects,
                     const std::vector<Factory::FxSetting>& fx, bool keepEnvelope);
    std::pair<float, float> measure() const;       // loudness (RMS dB) and peak (dB) of a note played now, offline
    juce::Random dice;
    int demoCountdown = 4;          // timer ticks (0,1 s) before the demo is offered
    bool demoSettled = false;       // the demo was loaded, or something else came first
    void parameterValueChanged (int, float) override {}
    void parameterGestureChanged (int, bool starting) override;

    using Curve = std::array<float, Drone::curvePoints>;
    struct Step
    {
        ParamSnapshot before {}, after {};
        Curve curveBefore {}, curveAfter {};
        Audio audioBefore, audioAfter;          // empty when the sample did not change
        double rateBefore = 0, rateAfter = 0;
        juce::String nameBefore, nameAfter;
        juce::File fileBefore, fileAfter;
        bool editedBefore = false, editedAfter = false;
    };
    Curve currentCurve() const;
    void commitHistory (bool atOnce = false);
    void pushStep (Step step);
    void restore (const Step& step, bool forward);

    std::array<std::atomic<float>*, P::Count> paramPtrs {};
    ParamSnapshot audioParams {};

    juce::SpinLock swapLock;
    std::shared_ptr<const SampleData> pending;
    bool hasPending = false;
    bool pendingSameSound = false;                  // the same audio analysed further: notes are not cut
    std::shared_ptr<const SampleData> lingering;    // an analysis notes still play: freed here once they end

    std::shared_ptr<const SampleData> uiSample;
    std::shared_ptr<const juce::AudioBuffer<float>> rawAudio;
    double rawRate = 44100.0;
    juce::File sampleFile;
    juce::File presetFolder;
    juce::String loadError;
    std::atomic<double> lastBpm { 0.0 };
    std::array<std::atomic<float>, Drone::curvePoints> droneCurve {};

    std::vector<Step> history;
    int historyPos = 0;                     // steps applied; the rest can be redone
    ParamSnapshot committed {};             // the state the last step ended in
    Curve committedCurve {};
    std::atomic<bool> touched { false };    // the user changed something since then
    std::atomic<int> gestures { 0 };
    std::atomic<double> lastGesture { 0.0 };
    bool extendLast = false;                // a sample step still taking the values its analysis sets
    bool restoring = false, presetLoading = false;

    // A/B (see compareSelect).
    std::array<ParamSnapshot, 2> slots {};
    std::array<Curve, 2> slotCurves {};
    int activeSlot = 0;
    bool slotsUsed = false;
    std::atomic<int> audioSlot { 0 };
    std::array<std::atomic<float>, 2> slotLoudness {};     // K-weighted mean square, slowly averaged; 0 = not heard yet
    std::atomic<float> matchGain { 1.0f };
    float appliedMatch = 1.0f;
    Dsp::Biquad kShelf, kHighPass;
    double loudnessRate = 48000.0;
    juce::AudioBuffer<float> clipboard;
    double clipboardRate = 44100.0;
    bool edited = false;

    // The sample as it goes into a project, prepared in the background after each load.
    juce::CriticalSection savedLock;
    Audio savedFor;
    juce::String saved;
    int knownPlayMode = 0;
    juce::String sliceSignature;
    std::vector<int> slicePoints, sliceOrder;
    void updateSlices();
    int knownEnvMode = P::EnvReshape;   // to tell a switch of the ADSR mode from a restored state

    std::array<std::atomic<float>, 2> meterPeak {}, meterSquares {};
    std::atomic<int> meterSamples { 0 };

    // Low priority: the analysis and the encoding always give way to the audio and to the host.
    // LOOP "Rubber Band": the render being made (pool of one thread), the one the engine reads, and the ones it
    // read before, kept a second before they are freed (a block uses one at most).
    juce::ThreadPool rubberPool { juce::ThreadPoolOptions{}.withNumberOfThreads (1).withThreadName ("Reanima Rubber Band") };
    std::atomic<RubberRender*> rubberDone { nullptr };
    std::atomic<bool> rubberBusy { false };
    std::unique_ptr<RubberRender> rubberCurrent;
    std::vector<std::pair<std::unique_ptr<RubberRender>, double>> rubberRetired;
    void serviceRubber();
    juce::CriticalSection rubberLock;
    juce::ThreadPool loader { juce::ThreadPoolOptions{}.withNumberOfThreads (1).withThreadName ("Reanima analysis")
                                                         .withDesiredThreadPriority (juce::Thread::Priority::low) };
    std::atomic<int> latestLoad { 0 };
    std::atomic<bool> loading { false };
    bool wantedReverse = false;
    juce::CriticalSection finishedLock;
    std::unique_ptr<Load> finished;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ReanimaProcessor)
};
