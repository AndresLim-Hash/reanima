// Time-stretch candidates side by side, offline, on the same files (evaluation only: Rubber Band is GPL and is
// never linked into the plug-in).
// Usage: ReanimaStretchLab <in.wav> <out.wav> <algo> <speed> <semitones>
//   algo: ss (Signalsmith default), ssshort (Signalsmith, shorter blocks), rb3 (Rubber Band R3 finer), rb2 (R2
//         crisp), hyb (harmonic/percussive split: harmonic through Signalsmith, percussive by short OLA, Driedger &
//         Muller 2014), hybrb (the same with the harmonic part through R3)
//   speed: output speed as a multiple of the input's (122 -> 100 BPM: 0.8197); prints the CPU time per audio second.

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_dsp/juce_dsp.h>
#include "signalsmith-stretch.h"
#include "rubberband/RubberBandStretcher.h"
#include <chrono>
#include <cstdio>

namespace
{
using Buf = juce::AudioBuffer<float>;

Buf stretchSS (const Buf& in, double rate, double speed, double semis, bool shortBlocks)
{
    signalsmith::stretch::SignalsmithStretch<float> s;
    if (shortBlocks)
        s.configure (in.getNumChannels(), (int) (0.06 * rate), (int) (0.015 * rate));
    else
        s.presetDefault (in.getNumChannels(), (float) rate);
    s.setTransposeSemitones ((float) semis);
    const int outN = (int) std::round (in.getNumSamples() / speed);
    Buf out (in.getNumChannels(), outN);
    std::vector<const float*> ip (in.getArrayOfReadPointers(), in.getArrayOfReadPointers() + in.getNumChannels());
    std::vector<float*> op (out.getArrayOfWritePointers(), out.getArrayOfWritePointers() + out.getNumChannels());
    s.exact (ip.data(), in.getNumSamples(), op.data(), outN);
    return out;
}

Buf stretchRB (const Buf& in, double rate, double speed, double semis, bool r3)
{
    using RB = RubberBand::RubberBandStretcher;
    RB::Options o = RB::OptionProcessOffline | RB::OptionChannelsTogether | (r3 ? RB::OptionEngineFiner : (RB::OptionEngineFaster | RB::OptionTransientsCrisp));
    RB rb ((size_t) rate, (size_t) in.getNumChannels(), o, 1.0 / speed, std::pow (2.0, semis / 12.0));
    rb.setExpectedInputDuration ((size_t) in.getNumSamples());
    std::vector<const float*> ip (in.getArrayOfReadPointers(), in.getArrayOfReadPointers() + in.getNumChannels());
    rb.study (ip.data(), (size_t) in.getNumSamples(), true);
    const int outN = (int) std::round (in.getNumSamples() / speed);
    Buf out (in.getNumChannels(), outN + 8192);
    out.clear();
    int got = 0;
    const int chunk = 4096;
    for (int pos = 0; pos < in.getNumSamples(); pos += chunk)
    {
        const int n = std::min (chunk, in.getNumSamples() - pos);
        std::vector<const float*> p (in.getNumChannels());
        for (int c = 0; c < in.getNumChannels(); ++c)
            p[(size_t) c] = in.getReadPointer (c) + pos;
        rb.process (p.data(), (size_t) n, pos + n >= in.getNumSamples());
        while (rb.available() > 0)
        {
            const int a = std::min ((int) rb.available(), out.getNumSamples() - got);
            if (a <= 0)
                break;
            std::vector<float*> w (out.getNumChannels());
            for (int c = 0; c < out.getNumChannels(); ++c)
                w[(size_t) c] = out.getWritePointer (c) + got;
            got += (int) rb.retrieve (w.data(), (size_t) a);
        }
    }
    while (rb.available() > 0 && got < out.getNumSamples())
    {
        std::vector<float*> w (out.getNumChannels());
        for (int c = 0; c < out.getNumChannels(); ++c)
            w[(size_t) c] = out.getWritePointer (c) + got;
        got += (int) rb.retrieve (w.data(), (size_t) std::min ((int) rb.available(), out.getNumSamples() - got));
    }
    out.setSize (out.getNumChannels(), outN, true);
    return out;
}

// Harmonic / percussive split by median filtering of the STFT magnitude (Fitzgerald), soft masks.
void hpss (const Buf& in, Buf& harm, Buf& perc)
{
    constexpr int order = 11, size = 1 << order, hop = size / 4, lh = 17, lp = 17;
    juce::dsp::FFT fft (order);
    const int n = in.getNumSamples(), frames = n / hop + 2;
    std::vector<float> win (size);
    for (int i = 0; i < size; ++i)
        win[(size_t) i] = 0.5f - 0.5f * std::cos (2.0f * juce::MathConstants<float>::pi * i / size);
    harm.setSize (in.getNumChannels(), n);
    perc.setSize (in.getNumChannels(), n);
    harm.clear();
    perc.clear();
    // Magnitudes of the channels' sum decide the masks (the same for both sides: the image stays).
    std::vector<std::vector<float>> mag ((size_t) frames, std::vector<float> (size / 2 + 1));
    std::vector<std::vector<std::complex<float>>> spec ((size_t) in.getNumChannels() * frames, std::vector<std::complex<float>> (size));
    for (int f = 0; f < frames; ++f)
        for (int c = 0; c < in.getNumChannels(); ++c)
        {
            std::vector<std::complex<float>> t (size), o (size);
            for (int i = 0; i < size; ++i)
            {
                const int at = f * hop + i - size / 2;
                t[(size_t) i] = at >= 0 && at < n ? in.getSample (c, at) * win[(size_t) i] : 0.0f;
            }
            fft.perform (t.data(), o.data(), false);
            spec[(size_t) (c * frames + f)] = o;
            for (int k = 0; k <= size / 2; ++k)
                mag[(size_t) f][(size_t) k] += std::abs (o[(size_t) k]);
        }
    std::vector<float> tmp;
    for (int f = 0; f < frames; ++f)
    {
        for (int k = 0; k <= size / 2; ++k)
        {
            tmp.clear();
            for (int q = std::max (0, f - lh / 2); q <= std::min (frames - 1, f + lh / 2); ++q)
                tmp.push_back (mag[(size_t) q][(size_t) k]);
            std::nth_element (tmp.begin(), tmp.begin() + (long) tmp.size() / 2, tmp.end());
            const float h = tmp[tmp.size() / 2];
            tmp.clear();
            for (int q = std::max (0, k - lp / 2); q <= std::min (size / 2, k + lp / 2); ++q)
                tmp.push_back (mag[(size_t) f][(size_t) q]);
            std::nth_element (tmp.begin(), tmp.begin() + (long) tmp.size() / 2, tmp.end());
            const float p = tmp[tmp.size() / 2];
            const float mh = h * h / (h * h + p * p + 1.0e-12f);
            for (int c = 0; c < in.getNumChannels(); ++c)
            {
                auto& o = spec[(size_t) (c * frames + f)];
                o[(size_t) k] *= mh;            // harmonic part kept in spec; percussive = input - harmonic
                if (k > 0 && k < size / 2)
                    o[(size_t) (size - k)] = std::conj (o[(size_t) k]);
            }
        }
        for (int c = 0; c < in.getNumChannels(); ++c)
        {
            std::vector<std::complex<float>> t (size);
            fft.perform (spec[(size_t) (c * frames + f)].data(), t.data(), true);
            for (int i = 0; i < size; ++i)
            {
                const int at = f * hop + i - size / 2;
                if (at >= 0 && at < n)
                    harm.addSample (c, at, t[(size_t) i].real() * win[(size_t) i] / 1.5f);     // Hann^2 at 75 % overlap sums to 1.5
            }
        }
    }
    for (int c = 0; c < in.getNumChannels(); ++c)
        for (int i = 0; i < n; ++i)
            perc.setSample (c, i, in.getSample (c, i) - harm.getSample (c, i));
}

// The percussive part: overlap-add of short Hann windows (12 ms), read at the speed, placed at a steady hop: the
// hits keep their shape; repitched by reading faster (as a sampler: drums are not tuned notes).
Buf olaPercussive (const Buf& in, double rate, double speed, double semis)
{
    const int win = (int) (0.012 * rate) / 2 * 2, hop = win / 2;
    const double pitch = std::pow (2.0, semis / 12.0);
    const int outN = (int) std::round (in.getNumSamples() / speed);
    Buf out (in.getNumChannels(), outN);
    out.clear();
    for (int o = -win; o < outN; o += hop)
    {
        const double src = (o + win / 2) * speed - win / 2 * pitch;
        for (int i = 0; i < win; ++i)
        {
            const int to = o + i;
            if (to < 0 || to >= outN)
                continue;
            const double x = src + i * pitch;
            const int k = (int) std::floor (x);
            if (k < 0 || k + 1 >= in.getNumSamples())
                continue;
            const float fr = (float) (x - k), w = 0.5f - 0.5f * std::cos (2.0f * juce::MathConstants<float>::pi * i / win);
            for (int c = 0; c < in.getNumChannels(); ++c)
                out.addSample (c, to, w * (in.getSample (c, k) * (1.0f - fr) + in.getSample (c, k + 1) * fr));
        }
    }
    return out;
}
} // namespace

int main (int argc, char* argv[])
{
    if (argc < 6)
        return 2;
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (juce::File (juce::String::fromUTF8 (argv[1]))));
    if (r == nullptr)
        return 1;
    Buf in (2, (int) r->lengthInSamples);
    r->read (&in, 0, (int) r->lengthInSamples, 0, true, true);
    const double rate = r->sampleRate, speed = juce::String (argv[4]).getDoubleValue(), semis = juce::String (argv[5]).getDoubleValue();
    const juce::String algo (argv[3]);
    const auto t0 = std::chrono::steady_clock::now();
    Buf out;
    if (algo == "ss" || algo == "ssshort")
        out = stretchSS (in, rate, speed, semis, algo == "ssshort");
    else if (algo == "rb3" || algo == "rb2")
        out = stretchRB (in, rate, speed, semis, algo == "rb3");
    else if (algo == "hyb" || algo == "hybrb")
    {
        Buf h, p;
        hpss (in, h, p);
        out = algo == "hyb" ? stretchSS (h, rate, speed, semis, false) : stretchRB (h, rate, speed, semis, true);
        const auto q = olaPercussive (p, rate, speed, semis);
        for (int c = 0; c < 2; ++c)
            out.addFrom (c, 0, q, c, 0, std::min (out.getNumSamples(), q.getNumSamples()));
    }
    else
        return 3;
    const double secs = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
    std::printf ("%s: %.3f s de CPU por segundo de audio\n", algo.toRawUTF8(), secs / (out.getNumSamples() / rate));
    const juce::File outFile (juce::String::fromUTF8 (argv[2]));
    outFile.deleteFile();
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (outFile);
    juce::WavAudioFormat wav;
    if (auto w = wav.createWriterFor (stream, juce::AudioFormatWriterOptions().withSampleRate (rate).withNumChannels (2).withBitsPerSample (32)))
        w->writeFromAudioSampleBuffer (out, 0, out.getNumSamples());
    return 0;
}
