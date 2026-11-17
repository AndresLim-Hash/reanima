#include "FactoryPresets.h"
#include "FxRack.h"

namespace Factory
{
// Types of Mecanismo (see Mechanism::Type) used below.
constexpr int Mechanism_RealPiano = 6, Mechanism_RealPick = 7, Mechanism_RealGuitar = 10;

const std::vector<const char*>& categories()
{
    static const std::vector<const char*> list { "Natural", "Transformado", "Híbrido", "Sintético", "Rítmico", "Percusivo", "Agresivo", "Industrial", "Cinemático", "Oscuro",
                                                 "Atmósfera", "Evolutivo", "Experimental" };
    return list;
}

// Ganancia (SampleGain) trims each preset to the common loudness; where a preset of 0.9.5 already used it,
// the new value includes the old one.
const std::vector<Preset>& presets()
{
    using namespace P;
    static const std::vector<Preset> list = {
        // Natural ---------------------------------------------------------------------------------
        { "Fiel al sample", "Natural", "El punto de partida: el motor A toca el sample tal cual, con la envolvente detectada.",
          "Comparar, y empezar cualquier sonido desde el original.",
          {}, {} },

        { "Piano de verdad", "Natural", "Golpe original intacto, cola que se oscurece al soltar y con la fuerza, macillo real y apagador.",
          "Pianos, Rhodes, Wurlitzer y clavinet: teclados reales.",
          { { Engine, EngineHybrid }, { VelBright, 0.8f }, { RelDark, 0.8f }, { RelNoise, 0.39f }, { MechType, Mechanism_RealPiano },
            { VelAmp, 0.85f }, { AmpR, 900.0f }, { Variation, 0.15f }, { SampleGain, 1.7f } },
          {}, { { main1, Fx::Reverb, 0.18f, { { 0, 1 }, { 2, 1.8f }, { 3, 15 }, { 5, 150 }, { 6, 8000 } } } } },

        { "Cuerdas con arco", "Natural", "Ataque lento que empieza oscuro, roce de arco real y vibrato con el aftertouch y la rueda.",
          "Cuerdas y vientos sostenidos, líneas lentas, colchones con arco.",
          { { Engine, EngineReenvelope }, { SustainType, SustainHold }, { AmpA, 350.0f }, { AmpD, 800.0f }, { AmpS, 0.8f },
            { AmpR, 700.0f }, { Bow, 1.0f }, { RelNoise, 0.4f }, { MechType, 2 }, { AtAmount, 0.8f }, { VibDepth, 0.6f },
            { Breath, 0.6f }, { SampleGain, -2.5f } },
          {}, { { main1, Fx::Reverb, 0.3f, { { 0, 1 }, { 1, 0.75f }, { 2, 3.2f }, { 3, 30 }, { 5, 180 } } } } },

        { "Arpa pulsada", "Natural", "El golpe de un arpa o un pizzicato de verdad sobre tu sonido, con cola que se apaga como una cuerda.",
          "Arpegios, ostinatos y plucks acústicos.",
          { { Engine, EngineHybrid }, { MechType, Mechanism_RealPick }, { RelNoise, 0.45f }, { VelBright, 0.7f }, { RelDark, 0.7f },
            { VelAmp, 0.8f }, { AmpFade, 6.0f }, { AmpR, 1200.0f }, { Variation, 0.12f }, { SampleGain, 2.1f } },
          { { Fx::Reverb, 0.22f } } },

        { "Guitarra de púa", "Natural", "Púa de guitarra real, nota que se apaga como una cuerda pulsada y un amplificador que la ensucia un poco.",
          "Riffs, rasgueos y líneas de guitarra con un sample de cuerda o de sinte.",
          { { Engine, EngineRebuild }, { MechType, Mechanism_RealGuitar }, { RelNoise, 0.5f }, { VelBright, 0.5f }, { RelDark, 0.5f },
            { Variation, 0.18f }, { AmpFade, 8.0f }, { AmpR, 300.0f }, { SampleGain, 2.5f } },
          { { Fx::Distortion, 0.12f }, { Fx::Chorus, 0.2f }, { Fx::Reverb, 0.12f } } },

        { "Pizzicato de cámara", "Natural", "Cuerda pellizcada de verdad, corta y seca, que se apaga enseguida como un pizzicato de orquesta.",
          "Ostinatos de cuerdas, ritmos de bandas sonoras, staccatos.",
          { { Engine, EngineRebuild }, { MechType, Mechanism_RealPick }, { RelNoise, 0.55f }, { VelBright, 0.9f }, { RelDark, 0.8f },
            { VelAmp, 0.85f }, { AmpFade, 40.0f }, { AmpR, 120.0f }, { Variation, 0.15f }, { SampleGain, 1.7f } },
          { { Fx::Reverb, 0.2f } } },

        { "Flauta con aliento", "Natural", "Entra con la lengua y el aire de un soplo real, se sostiene respirando y vibra con la rueda.",
          "Melodías de viento, líneas lentas y expresivas.",
          { { Engine, EngineRebuild }, { SustainType, SustainHold }, { MechType, 4 }, { RelNoise, 0.45f }, { AmpA, 60.0f },
            { AmpS, 0.85f }, { AmpR, 300.0f }, { Breath, 0.6f }, { VibDepth, 0.5f }, { AtAmount, 0.7f }, { VelBright, 0.6f }, { SampleGain, -4.4f } },
          { { Fx::Reverb, 0.25f } } },

        // Híbrido ---------------------------------------------------------------------------------
        { "Teclado eléctrico", "Híbrido", "Macillo real, brillo que muerde con la fuerza y un trémolo estéreo lento: un eléctrico hecho de tu sample.",
          "Acordes de soul, neo-soul y lo-fi; voicings largos.",
          { { Engine, EngineRebuild }, { MechType, Mechanism_RealPiano }, { RelNoise, 0.3f }, { VelBright, 0.85f }, { RelDark, 0.6f },
            { AmpR, 700.0f }, { Lfo1Rate, 4.5f }, { Lfo1Shape, 0.0f }, { M1Src, SrcLfo1 }, { M1Dst, DstPan }, { M1Amt, 0.35f },
            { SampleGain, 3.9f } },
          { { Fx::Phaser, 0.2f }, { Fx::Chorus, 0.2f }, { Fx::Reverb, 0.15f } } },

        { "Órgano de iglesia", "Híbrido", "El sample resintetizado y sostenido como tubos de órgano, con el clic de la tecla y una nave enorme.",
          "Corales, acordes sostenidos, música sacra y épica.",
          { { Engine, EngineResynth }, { EnvMode, EnvSynth }, { AmpA, 30.0f }, { AmpD, 100.0f }, { AmpS, 1.0f }, { AmpR, 400.0f },
            { MechType, 3 }, { RelNoise, 0.3f }, { Unison, 2.0f }, { UniDetune, 6.0f }, { UniWidth, 0.6f }, { SampleGain, 2.7f } },
          { { Fx::Reverb, 0.45f } } },

        { "Marimba de madera", "Híbrido", "Pocos armónicos, golpe de baqueta y caída rápida: tu sample convertido en láminas de madera.",
          "Patrones de percusión melódica, tropical, minimalismo.",
          { { Engine, EngineResynth }, { EnvMode, EnvSynth }, { AmpA, 1.0f }, { AmpD, 700.0f }, { AmpS, 0.0f }, { AmpR, 400.0f },
            { Partials, 24.0f }, { MechType, 3 }, { RelNoise, 0.35f }, { VelBright, 0.8f }, { VelAmp, 0.85f }, { SampleGain, 4.5f } },
          { { Fx::Reverb, 0.15f } } },

        { "Clavinet funky", "Híbrido", "Ataque mordido, notas cortas y un filtro que se abre con cada golpe: un clavinet con wah automático.",
          "Funk, riffs rítmicos, groove.",
          { { Engine, EngineRebuild }, { MechType, Mechanism_RealPiano }, { RelNoise, 0.35f }, { VelBright, 1.0f }, { AmpFade, 10.0f },
            { AmpR, 120.0f }, { FltOn, 1.0f }, { FltType, LP }, { FltFreq, 900.0f }, { FltRes, 0.3f }, { FltFollow, 0.4f },
            { FltFolAtk, 3.0f }, { FltFolRel, 150.0f }, { SampleGain, -4.1f } },
          { { Fx::Phaser, 0.15f } } },

        // Sintético -------------------------------------------------------------------------------
        { "Bajo sinte", "Sintético", "Envolvente de sintetizador sobre el timbre del sample, una sola voz con glide y filtro que muerde.",
          "Bajos de house, techno y pop; líneas con legato.",
          { { Engine, EngineRebuild }, { EnvMode, EnvSynth }, { AmpA, 3.0f }, { AmpD, 350.0f }, { AmpS, 0.7f }, { AmpR, 120.0f },
            { Punch, 0.8f }, { PolyMode, Mono }, { Glide, 40.0f }, { SnapC, 1.0f }, { SampleGain, 0.8f }, { FltOn, 1.0f }, { FltType, LP },
            { FltSlope, 1.0f }, { FltFreq, 900.0f }, { FltRes, 0.25f }, { FltKey, 0.5f }, { FltEnvAmt, 0.35f },
            { E2A, 1.0f }, { E2D, 250.0f }, { E2S, 0.2f }, { E2R, 150.0f } },
          {} },

        { "Supersaw de sample", "Sintético", "El sample resintetizado y multiplicado en 7 voces desafinadas, como un supersaw de Serum.",
          "Acordes de trance y EDM, leads anchos.",
          { { Engine, EngineResynth }, { EnvMode, EnvSynth }, { Unison, 7.0f }, { UniDetune, 35.0f }, { UniBlend, 0.7f },
            { UniWidth, 1.0f }, { AmpA, 5.0f }, { AmpD, 800.0f }, { AmpS, 0.75f }, { AmpR, 400.0f }, { FltOn, 1.0f },
            { FltType, LP }, { FltSlope, 0.0f }, { FltFreq, 6000.0f }, { SampleGain, 2.4f } },
          { { Fx::HyperDim, 0.5f }, { Fx::Reverb, 0.2f } } },

        { "Pluck", "Sintético", "Ataque seco y caída corta con el filtro cerrándose: el sample convertido en un pulsado.",
          "Melodías rápidas, arpegiadores, house y future bass.",
          { { Engine, EngineResynth }, { EnvMode, EnvSynth }, { AmpA, 1.0f }, { AmpD, 260.0f }, { AmpS, 0.0f }, { AmpR, 220.0f },
            { VelBright, 0.8f }, { FltOn, 1.0f }, { FltType, LP }, { FltSlope, 1.0f }, { FltFreq, 1500.0f }, { FltRes, 0.3f },
            { FltEnvAmt, 0.5f }, { E2A, 1.0f }, { E2D, 200.0f }, { E2S, 0.0f }, { E2R, 200.0f }, { SampleGain, 0.2f } },
          {}, { { main1, Fx::Delay, 0.2f, { { 0, 1 }, { 3, 3 }, { 4, 3 }, { 6, 0.4f }, { 7, 1 }, { 8, 3000 }, { 9, 3 } } } } },

        { "Lead expresivo", "Sintético", "Una voz que liga con glide, se abre con la fuerza y canta con vibrato en la rueda y el aftertouch.",
          "Melodías principales y solos.",
          { { Engine, EngineRebuild }, { EnvMode, EnvSynth }, { AmpA, 8.0f }, { AmpD, 600.0f }, { AmpS, 0.8f }, { AmpR, 250.0f },
            { PolyMode, Legato }, { Glide, 70.0f }, { VelBright, 0.7f }, { AtAmount, 0.8f }, { VibDepth, 0.7f },
            { FltOn, 1.0f }, { FltType, LP }, { FltFreq, 3500.0f }, { FltRes, 0.2f }, { FltKey, 0.5f }, { FltEnvAmt, 0.2f },
            { E2A, 5.0f }, { E2D, 400.0f }, { E2S, 0.3f }, { E2R, 300.0f }, { SampleGain, 5.6f } },
          {}, { { main1, Fx::Delay, 0.22f, { { 0, 1 }, { 3, 4 }, { 4, 4 }, { 6, 0.35f }, { 7, 1 }, { 8, 2500 }, { 9, 3 } } },
                 { main1 + 1, Fx::Reverb, 0.15f, { { 0, 0 }, { 2, 1.4f } } } } },

        { "Campana de cristal", "Sintético", "El sample resintetizado una octava arriba, golpe brillante y cola larga que se apaga sola.",
          "Campanas, cajas de música, melodías brillantes y navideñas.",
          { { Engine, EngineResynth }, { EnvMode, EnvSynth }, { AmpA, 1.0f }, { AmpD, 3000.0f }, { AmpS, 0.0f }, { AmpR, 2500.0f },
            { Transpose, 12.0f }, { VelBright, 0.9f }, { VelAmp, 0.8f }, { SampleGain, 3.5f } },
          { { Fx::Delay, 0.15f }, { Fx::Reverb, 0.35f } } },

        { "Bajo sub", "Sintético", "Casi solo la fundamental, una octava abajo, una voz con glide: un sub limpio que empuja.",
          "Subgraves de trap, dubstep, techno; reforzar otro bajo.",
          { { Engine, EngineResynth }, { EnvMode, EnvSynth }, { Partials, 4.0f }, { Transpose, -12.0f }, { AmpA, 3.0f }, { AmpD, 400.0f },
            { AmpS, 0.9f }, { AmpR, 150.0f }, { PolyMode, Mono }, { Glide, 30.0f }, { SnapC, 1.0f }, { SampleGain, 1.8f } },
          {} },

        { "Acid", "Sintético", "Filtro resonante que se cierra rápido, saturado, con glide entre notas ligadas: la línea ácida clásica.",
          "Acid house, techno, líneas hipnóticas.",
          { { Engine, EngineRebuild }, { EnvMode, EnvSynth }, { AmpA, 2.0f }, { AmpD, 300.0f }, { AmpS, 0.6f }, { AmpR, 80.0f },
            { PolyMode, Legato }, { Glide, 60.0f }, { FltOn, 1.0f }, { FltType, LP }, { FltSlope, 1.0f }, { FltCircuit, OSR },
            { FltFreq, 450.0f }, { FltRes, 0.75f }, { FltDrive, 6.0f }, { FltEnvAmt, 0.7f }, { E2A, 1.0f }, { E2D, 180.0f },
            { E2S, 0.0f }, { E2R, 100.0f }, { SampleGain, 1.3f } },
          { { Fx::Distortion, 0.15f }, { Fx::Delay, 0.15f } } },

        { "Pad analógico", "Sintético", "Cinco voces desafinadas, filtro cálido que respira despacio y chorus: el pad de un polisinte de los 80.",
          "Synthwave, pop ochentero, acordes cálidos.",
          { { Engine, EngineResynth }, { EnvMode, EnvSynth }, { AmpA, 400.0f }, { AmpD, 1500.0f }, { AmpS, 0.8f }, { AmpR, 1200.0f },
            { Unison, 5.0f }, { UniDetune, 20.0f }, { UniWidth, 0.9f }, { FltOn, 1.0f }, { FltType, LP }, { FltFreq, 2500.0f },
            { FltRes, 0.15f }, { FltCircuit, MS2 }, { Lfo1Rate, 0.3f }, { M1Src, SrcLfo1 }, { M1Dst, DstCutoff }, { M1Amt, 0.15f }, { SampleGain, 7.3f } },
          { { Fx::Chorus, 0.3f }, { Fx::Reverb, 0.3f } } },

        { "Stab de house", "Sintético", "Un golpe corto de acorde, ancho y comprimido, que se corta en seco: para tocar acordes rítmicos.",
          "House, garage, acordes en contratiempo.",
          { { Engine, EngineResynth }, { EnvMode, EnvSynth }, { AmpA, 1.0f }, { AmpD, 180.0f }, { AmpS, 0.0f }, { AmpR, 150.0f },
            { Unison, 3.0f }, { UniDetune, 15.0f }, { UniWidth, 0.8f }, { FltOn, 1.0f }, { FltType, LP }, { FltFreq, 2200.0f },
            { FltRes, 0.3f }, { SampleGain, 1.6f } },
          { { Fx::Compressor, 0.6f }, { Fx::Delay, 0.15f }, { Fx::Reverb, 0.2f } } },

        // Rítmico --------------------------------------------------------------------------------
        { "Eco de cinta", "Rítmico", "Un golpe con algo de válvula y un eco en paralelo, oscuro y algo inestable como una cinta: corchea a un lado, negra al otro.",
          "Dub, guitarras y plucks con eco, frases sueltas que se repiten.",
          { { Engine, EngineRebuild }, { EnvMode, EnvSynth }, { AmpA, 1.0f }, { AmpD, 400.0f }, { AmpS, 0.3f }, { AmpR, 300.0f },
            { VelBright, 0.7f }, { FxBus1Send, 0.45f }, { SampleGain, 2.5f } },
          {}, { { main1, Fx::Distortion, 0.35f, { { 0, 0 }, { 1, 0.25f } } },
                { bus1, Fx::Delay, 1.0f, { { 0, 1 }, { 3, 4 }, { 4, 6 }, { 5, 0 }, { 6, 0.55f }, { 7, 0 }, { 8, 1200 }, { 9, 2 } } },
                { bus1 + 1, Fx::Chorus, 0.5f, { { 0, 0.6f }, { 1, 12 }, { 2, 18 }, { 3, 0.35f } } } } },

        { "Ping-pong de semicorcheas", "Rítmico", "Golpes cortos y abiertos que rebotan de lado a lado en semicorcheas, con una placa corta detrás.",
          "Secuencias, arpegios rápidos, house y trance.",
          { { Engine, EngineResynth }, { EnvMode, EnvSynth }, { AmpA, 1.0f }, { AmpD, 200.0f }, { AmpS, 0.0f }, { AmpR, 120.0f },
            { Unison, 2.0f }, { UniDetune, 10.0f }, { SampleGain, 8.2f } },
          {}, { { main1, Fx::Delay, 0.3f, { { 0, 1 }, { 3, 2 }, { 4, 2 }, { 6, 0.45f }, { 7, 1 }, { 8, 4000 }, { 9, 4 } } },
                { main1 + 1, Fx::Reverb, 0.15f, { { 0, 0 }, { 2, 1.2f } } } } },

        { "Puerta trance", "Rítmico", "Un pad ancho cortado en semicorcheas al tempo, con phaser y un eco en paralelo: el gate de trance.",
          "Trance, progressive, rellenos rítmicos sobre acordes largos.",
          { { Engine, EngineReenvelope }, { SustainType, SustainHold }, { SustainMode, SustainGranular }, { AmpA, 5.0f }, { AmpS, 1.0f },
            { AmpR, 300.0f }, { Unison, 4.0f }, { UniDetune, 20.0f }, { UniWidth, 1.0f }, { Lfo1Sync, 1.0f }, { Lfo1Div, 2.0f },
            { Lfo1Shape, Square }, { M1Src, SrcLfo1 }, { M1Dst, DstVolume }, { M1Amt, 0.3f }, { SampleGain, -6.4f }, { FxBus1Send, 0.3f } },
          {}, { { main1, Fx::Phaser, 0.5f, { { 0, 0.2f }, { 1, 0.6f }, { 3, 0.5f }, { 5, 3 } } },
                { main1 + 1, Fx::Reverb, 0.2f, { { 0, 1 }, { 2, 2.5f } } },
                { bus1, Fx::Delay, 1.0f, { { 0, 1 }, { 3, 4 }, { 4, 4 }, { 6, 0.3f }, { 7, 1 }, { 8, 3000 } } } } },

        { "Filtro que late", "Rítmico", "La nota sostenida y un filtro resonante que se abre y se cierra en corcheas, saturado.",
          "Techno, electro, bajos y acordes con movimiento.",
          { { Engine, EngineRebuild }, { SustainType, SustainHold }, { AmpS, 1.0f }, { AmpR, 200.0f }, { FltOn, 1.0f }, { FltType, LP },
            { FltSlope, 1.0f }, { FltCircuit, OSR }, { FltFreq, 600.0f }, { FltRes, 0.6f }, { Lfo1Sync, 1.0f }, { Lfo1Div, 4.0f },
            { Lfo1Shape, Triangle }, { M1Src, SrcLfo1 }, { M1Dst, DstCutoff }, { M1Amt, 0.45f }, { SampleGain, -5.7f } },
          {}, { { main1, Fx::Distortion, 0.6f, { { 0, 15 }, { 1, 0.35f } } },
                { main1 + 1, Fx::Delay, 0.15f, { { 0, 1 }, { 3, 6 }, { 4, 6 }, { 6, 0.3f }, { 7, 1 } } } } },

        { "Wobble", "Rítmico", "Un bajo de una voz con el filtro barrido en corcheas, recortado y comprimido: el wobble del dubstep.",
          "Dubstep, drum and bass, bajos que hablan.",
          { { Engine, EngineRebuild }, { EnvMode, EnvSynth }, { AmpA, 2.0f }, { AmpS, 1.0f }, { AmpR, 120.0f }, { PolyMode, Mono },
            { SnapC, 1.0f }, { FltOn, 1.0f }, { FltType, LP }, { FltSlope, 1.0f }, { FltCircuit, MS2 }, { FltFreq, 300.0f }, { FltRes, 0.5f },
            { Lfo1Sync, 1.0f }, { Lfo1Div, 4.0f }, { Lfo1Shape, SawDown }, { M1Src, SrcLfo1 }, { M1Dst, DstCutoff }, { M1Amt, 0.6f }, { SampleGain, 1.5f } },
          {}, { { main1, Fx::Distortion, 0.7f, { { 0, 2 }, { 1, 0.5f }, { 3, 2 }, { 4, 0 }, { 5, 6000 } } },
                { main1 + 1, Fx::Compressor, 1.0f, { { 0, -20 }, { 1, 4 }, { 4, 4 } } } } },

        { "Hi-hats granulares", "Rítmico", "Granos diminutos y brillantes que cambian de sitio en cada semicorchea y saltan por el estéreo.",
          "Hats, shakers y ritmos hechos con cualquier sonido.",
          { { Engine, EngineGranular }, { GrainPos, 0.1f }, { GrainMs, 15.0f }, { GrainDensity, 150.0f }, { Spray, 0.3f }, { GrainWidth, 1.0f },
            { AmpA, 1.0f }, { AmpS, 0.0f }, { AmpD, 250.0f }, { AmpR, 80.0f }, { FltOn, 1.0f }, { FltType, HP }, { FltFreq, 3500.0f },
            { Lfo1Sync, 1.0f }, { Lfo1Div, 2.0f }, { Lfo1Shape, SampleHold }, { M1Src, SrcLfo1 }, { M1Dst, DstSpray }, { M1Amt, 0.6f },
            { M2Src, SrcRandom }, { M2Dst, DstPan }, { M2Amt, 0.5f }, { SampleGain, 9.0f } },
          {}, { { main1, Fx::Delay, 0.3f, { { 0, 1 }, { 3, 1 }, { 4, 1 }, { 6, 0.3f }, { 7, 1 }, { 8, 8000 } } } } },

        { "Golpes en multi-tap", "Rítmico", "Un golpe corto, un toque metálico desplazado y repeticiones en semicorcheas que se van apagando.",
          "Percusión melódica, IDM, patrones que se multiplican solos.",
          { { Engine, EngineRebuild }, { EnvMode, EnvSynth }, { AmpA, 1.0f }, { AmpD, 250.0f }, { AmpS, 0.0f }, { AmpR, 150.0f }, { Punch, 0.6f }, { SampleGain, 7.8f } },
          {}, { { main1, Fx::Bode, 0.3f, { { 0, 300 }, { 1, 0.4f }, { 2, 0.3f } } },
                { main1 + 1, Fx::Delay, 0.35f, { { 0, 1 }, { 3, 2 }, { 4, 2 }, { 6, 0.35f }, { 7, 2 }, { 8, 2500 } } } } },

        { "Latido de tráiler", "Rítmico", "Un golpe grave que cae de tono, con un eco oscuro en negras y una sala enorme, los dos en paralelo.",
          "Tráileres, intros épicas, pulsos de tensión.",
          { { Engine, EngineRebuild }, { EnvMode, EnvSynth }, { AmpA, 1.0f }, { AmpD, 900.0f }, { AmpS, 0.0f }, { AmpR, 400.0f },
            { Transpose, -12.0f }, { PitchEnvAmt, 5.0f }, { PitchEnvTime, 120.0f }, { Punch, 0.7f }, { FxBus1Send, 0.35f }, { FxBus2Send, 0.4f }, { SampleGain, 3.4f } },
          {}, { { main1, Fx::Distortion, 0.4f, { { 0, 0 }, { 1, 0.4f } } },
                { bus1, Fx::Delay, 1.0f, { { 0, 1 }, { 3, 6 }, { 4, 6 }, { 6, 0.45f }, { 7, 0 }, { 8, 900 }, { 9, 2 } } },
                { bus2, Fx::Reverb, 1.0f, { { 0, 1 }, { 1, 0.9f }, { 2, 6 }, { 3, 30 }, { 5, 100 } } } } },

        { "Arpegio de cristal", "Rítmico", "Notas brillantes una octava arriba, cada una en otro sitio del estéreo, con eco en semicorcheas y una cueva de reverb.",
          "Arpegiadores, música de videojuegos, ambient rítmico.",
          { { Engine, EngineResynth }, { EnvMode, EnvSynth }, { AmpA, 1.0f }, { AmpD, 600.0f }, { AmpS, 0.0f }, { AmpR, 500.0f },
            { Transpose, 12.0f }, { VelBright, 0.9f }, { M1Src, SrcRandom }, { M1Dst, DstPan }, { M1Amt, 0.5f }, { FxBus2Send, 0.35f }, { SampleGain, 3.8f } },
          {}, { { main1, Fx::Delay, 0.3f, { { 0, 1 }, { 3, 2 }, { 4, 2 }, { 6, 0.5f }, { 7, 1 }, { 8, 5000 } } },
                { bus2, Fx::Reverb, 1.0f, { { 0, 4 }, { 1, 0.9f }, { 2, 7 } } } } },

        { "Trémolo surf", "Rítmico", "Púa de guitarra, trémolo que late y una reverb vintage corta y brillante, con un slapback detrás.",
          "Surf, rockabilly, guitarras de western y spaghetti.",
          { { Engine, EngineRebuild }, { MechType, Mechanism_RealGuitar }, { RelNoise, 0.4f }, { AmpR, 400.0f }, { Lfo1Rate, 6.0f },
            { M1Src, SrcLfo1 }, { M1Dst, DstVolume }, { M1Amt, 0.2f }, { SampleGain, 1.9f } },
          {}, { { main1, Fx::Delay, 0.2f, { { 0, 0 }, { 1, 120 }, { 2, 120 }, { 6, 0.15f }, { 7, 0 }, { 8, 3000 } } },
                { main1 + 1, Fx::Reverb, 0.35f, { { 0, 2 }, { 1, 0.4f }, { 2, 1.6f }, { 3, 0 }, { 6, 7000 } } } } },

        // Percusivo -------------------------------------------------------------------------------
        { "808 desde un bombo", "Percusivo", "Un bombo afinado al Do con caída de tono y cola larga: un 808 hecho con tu propio golpe.",
          "Bajos 808 de trap y hip hop.",
          { { Engine, EngineRebuild }, { EnvMode, EnvSynth }, { AmpA, 1.0f }, { AmpD, 2500.0f }, { AmpS, 0.0f }, { AmpR, 300.0f },
            { Punch, 0.7f }, { PitchEnvAmt, 12.0f }, { PitchEnvTime, 90.0f }, { SnapC, 1.0f }, { PolyMode, Mono }, { SampleGain, 0.6f } },
          { { Fx::Distortion, 0.25f } } },

        { "Tom electrónico", "Percusivo", "Golpe con pegada y un tono que cae: un tom de caja de ritmos afinable en todo el teclado.",
          "Fills, percusión electrónica, ritmos tribales.",
          { { Engine, EngineRebuild }, { EnvMode, EnvSynth }, { AmpA, 1.0f }, { AmpD, 500.0f }, { AmpS, 0.0f }, { AmpR, 200.0f },
            { Punch, 0.6f }, { PitchEnvAmt, 7.0f }, { PitchEnvTime, 150.0f }, { SampleGain, 2.3f } },
          { { Fx::Compressor, 0.6f } } },

        { "Golpe metálico", "Percusivo", "Pocos parciales desplazados en frecuencia y sin graves: un golpe de metal inarmónico.",
          "Percusión industrial, campanas raras, efectos.",
          { { Engine, EngineResynth }, { EnvMode, EnvSynth }, { Partials, 12.0f }, { AmpA, 1.0f }, { AmpD, 900.0f }, { AmpS, 0.0f },
            { AmpR, 600.0f }, { FltOn, 1.0f }, { FltType, HP }, { FltFreq, 400.0f }, { SampleGain, 2.4f } },
          { { Fx::Bode, 0.25f }, { Fx::Reverb, 0.25f } } },

        { "Caja de ritmos lo-fi", "Percusivo", "Golpes cortos con pocos bits, menos muestras y saturación: una caja de ritmos sucia y con carácter.",
          "Hip hop, boom bap, percusión con textura.",
          { { Engine, EngineRebuild }, { Bits, 8.0f }, { Downsample, 4.0f }, { AmpFade, 20.0f }, { AmpR, 150.0f }, { Punch, 0.5f }, { SampleGain, -0.9f } },
          { { Fx::Distortion, 0.2f } } },

        // Atmósfera -------------------------------------------------------------------------------
        { "Pad infinito", "Atmósfera", "Convierte cualquier tono en un pad: entra despacio, se sostiene respirando y se abre en estéreo.",
          "Fondos, acordes largos, intros.",
          { { Engine, EngineReenvelope }, { SustainType, SustainHold }, { SustainMode, SustainGranular }, { AmpA, 900.0f },
            { AmpD, 2000.0f }, { AmpS, 0.85f }, { AmpR, 2500.0f }, { Breath, 0.5f }, { Unison, 4.0f }, { UniDetune, 18.0f },
            { UniWidth, 1.0f }, { GrainMix, 0.25f }, { SampleGain, 2.0f } },
          {}, { { main1, Fx::Chorus, 0.3f, { { 0, 0.3f }, { 3, 0.5f } } },
                 { main1 + 1, Fx::Reverb, 0.35f, { { 0, 1 }, { 1, 0.85f }, { 2, 4.5f }, { 3, 40 }, { 5, 200 } } } } },

        { "Pad que respira", "Atmósfera", "Un pad cuyo filtro se abre y se cierra muy despacio mientras se mueve en el estéreo: nunca está quieto.",
          "Fondos que evolucionan, ambient, bandas sonoras.",
          { { Engine, EngineReenvelope }, { SustainType, SustainHold }, { SustainMode, SustainGranular }, { AmpA, 1200.0f },
            { AmpD, 2000.0f }, { AmpS, 0.9f }, { AmpR, 3000.0f }, { Unison, 3.0f }, { UniDetune, 12.0f }, { UniWidth, 0.9f },
            { FltOn, 1.0f }, { FltType, LP }, { FltSlope, 0.0f }, { FltFreq, 1200.0f }, { FltRes, 0.2f },
            { Lfo2Rate, 0.08f }, { Lfo2Shape, 1.0f }, { Lfo2Retrig, 0.0f }, { Lfo3Rate, 0.13f }, { Lfo3Retrig, 0.0f },
            { M1Src, SrcLfo2 }, { M1Dst, DstCutoff }, { M1Amt, 0.35f }, { M2Src, SrcLfo3 }, { M2Dst, DstPan }, { M2Amt, 0.3f },
            { SampleGain, -0.9f } },
          { { Fx::Reverb, 0.4f } } },

        { "Nube granular", "Atmósfera", "El sample deshecho en una nube de granos que deriva despacio: texturas y ambientes.",
          "Texturas, transiciones suaves, ambientes.",
          { { Engine, EngineGranular }, { GrainPos, 0.15f }, { GrainMs, 120.0f }, { GrainDensity, 60.0f }, { Spray, 0.6f }, { GrainPitch, 0.3f },
            { GrainWidth, 1.0f }, { GranSpeed, 0.1f }, { AmpA, 600.0f }, { AmpS, 1.0f }, { AmpR, 2500.0f }, { SampleGain, 5.2f } },
          {}, { { main1, Fx::Delay, 0.2f, { { 0, 1 }, { 3, 6 }, { 4, 6 }, { 6, 0.5f }, { 7, 1 }, { 8, 1500 }, { 9, 2 } } },
                 { main1 + 1, Fx::Reverb, 0.4f, { { 0, 4 }, { 1, 0.9f }, { 2, 6 }, { 3, 30 } } } } },

        { "Drone eterno", "Atmósfera", "El sample estirado doscientas veces, afinado en sus armónicos y con una octava por debajo.",
          "Drones, notas pedal, fondos oscuros.",
          { { Engine, EngineDrone }, { GrainPos, 0.15f }, { DroneStretch, 200.0f }, { DroneSize, 4.0f }, { DroneHarm, 0.4f }, { DroneSpread, 0.2f },
            { DroneSub1, 0.5f }, { DroneUp1, 0.3f }, { AmpA, 2000.0f }, { AmpS, 1.0f }, { AmpR, 4000.0f }, { SampleGain, 0.5f } },
          {}, { { main1, Fx::Reverb, 0.35f, { { 0, 4 }, { 1, 1.0f }, { 2, 10 }, { 5, 80 } } } } },

        { "Shimmer", "Atmósfera", "Drone con octavas por encima y sólo lo tonal: el brillo de una reverb shimmer, sin reverb shimmer.",
          "Fondos luminosos, finales, ambient.",
          { { Engine, EngineDrone }, { GrainPos, 0.15f }, { DroneStretch, 40.0f }, { DroneTonal, 0.5f }, { DroneUp1, 0.6f }, { DroneUp2, 0.5f },
            { AmpA, 1500.0f }, { AmpS, 1.0f }, { AmpR, 5000.0f }, { SampleGain, -2.7f } },
          {}, { { main1, Fx::Reverb, 0.5f, { { 0, 4 }, { 1, 0.9f }, { 2, 8 }, { 3, 60 }, { 5, 200 } } } } },

        { "Viento helado", "Atmósfera", "Granos largos sin graves esparcidos por todo el sample, que entran despacio y se pierden en una reverb enorme.",
          "Ambientes fríos, paisajes sonoros, intros.",
          { { Engine, EngineGranular }, { GrainPos, 0.3f }, { GrainMs, 300.0f }, { GrainDensity, 20.0f }, { Spray, 1.0f },
            { GrainPitch, 0.5f }, { GrainWidth, 1.0f }, { GranSpeed, 0.05f }, { AmpA, 2000.0f }, { AmpS, 1.0f }, { AmpR, 3000.0f },
            { FltOn, 1.0f }, { FltType, HP }, { FltFreq, 800.0f }, { SampleGain, 5.4f } },
          { { Fx::Reverb, 0.6f } } },

        { "Coro etéreo", "Atmósfera", "El sample sostenido a varias voces y pasado por un filtro de vocales que cambia muy despacio.",
          "Coros sintéticos, fondos místicos, bandas sonoras.",
          { { Engine, EngineReenvelope }, { SustainType, SustainHold }, { SustainMode, SustainGranular }, { AmpA, 800.0f },
            { AmpS, 0.9f }, { AmpR, 2500.0f }, { Unison, 4.0f }, { UniDetune, 12.0f }, { UniWidth, 1.0f }, { FltOn, 1.0f },
            { FltType, Vowel }, { FltMorph, 0.4f }, { FltMix, 0.5f }, { Lfo2Rate, 0.1f }, { Lfo2Retrig, 0.0f }, { M1Src, SrcLfo2 }, { M1Dst, DstMorph },
            { M1Amt, 0.3f }, { SampleGain, 6.9f } },
          { { Fx::Chorus, 0.2f }, { Fx::Reverb, 0.45f } } },

        { "Instante congelado", "Atmósfera", "Un momento del sample congelado y respirando, que entra despacio y se repite en ecos.",
          "Fondos estáticos, pausas, transiciones suaves.",
          { { Engine, EngineReenvelope }, { SustainType, SustainHold }, { SustainMode, SustainFreeze }, { AmpA, 1500.0f },
            { AmpS, 1.0f }, { AmpR, 3000.0f }, { Breath, 0.7f }, { FltOn, 1.0f }, { FltType, LP }, { FltFreq, 3000.0f }, { SampleGain, -1.3f } },
          { { Fx::Delay, 0.25f }, { Fx::Reverb, 0.4f } } },

        { "Cinta que se arrastra", "Atmósfera", "Un pad con la afinación ondulante de una cinta gastada, un eco oscuro libre y una reverb vintage.",
          "Lo-fi, chillwave, recuerdos, fondos nostálgicos.",
          { { Engine, EngineReenvelope }, { SustainType, SustainHold }, { SustainMode, SustainGranular }, { AmpA, 600.0f }, { AmpS, 0.9f },
            { AmpR, 1500.0f }, { Bits, 12.0f }, { SampleGain, 5.0f } },
          {}, { { main1, Fx::Chorus, 0.6f, { { 0, 0.25f }, { 1, 15 }, { 2, 22 }, { 3, 0.7f } } },
                { main1 + 1, Fx::Delay, 0.25f, { { 0, 0 }, { 1, 450 }, { 2, 600 }, { 5, 0 }, { 6, 0.5f }, { 7, 0 }, { 8, 1000 }, { 9, 1.5f } } },
                { main1 + 2, Fx::Reverb, 0.3f, { { 0, 2 }, { 2, 3 } } } } },

        // Cinemático ------------------------------------------------------------------------------
        { "Impacto cinemático", "Cinemático", "El sample una octava abajo, cayendo hacia su nota, con saturación y una sala enorme.",
          "Golpes de tráiler, finales de frase, transiciones.",
          { { Engine, EngineRebuild }, { Transpose, -12.0f }, { PitchEnvAmt, 7.0f }, { PitchEnvTime, 600.0f }, { AmpR, 3000.0f },
            { VelAmp, 0.9f }, { SampleGain, 3.0f } },
          { { Fx::Distortion, 0.2f }, { Fx::Reverb, 0.5f } } },

        { "Riser de tensión", "Cinemático", "Sube de tono durante cuatro segundos mientras el filtro se abre: la tensión antes del golpe.",
          "Subidas, builds, tráileres.",
          { { Engine, EngineRebuild }, { SustainType, SustainHold }, { PitchEnvAmt, -24.0f }, { PitchEnvTime, 4000.0f }, { AmpA, 3000.0f },
            { AmpS, 1.0f }, { AmpR, 1500.0f }, { FltOn, 1.0f }, { FltType, LP }, { FltFreq, 300.0f }, { FltEnvAmt, 0.8f },
            { E2A, 4000.0f }, { E2S, 1.0f }, { E2R, 1500.0f }, { SampleGain, -0.5f } },
          { { Fx::Reverb, 0.4f } } },

        { "Braam", "Cinemático", "Una octava abajo, seis voces, saturación y un filtro que se abre: el bramido de metales de las bandas sonoras.",
          "Tráileres, momentos épicos, golpes de tensión.",
          { { Engine, EngineResynth }, { EnvMode, EnvSynth }, { Transpose, -12.0f }, { Unison, 6.0f }, { UniDetune, 25.0f },
            { UniWidth, 1.0f }, { AmpA, 300.0f }, { AmpS, 1.0f }, { AmpR, 2000.0f }, { FltOn, 1.0f }, { FltType, LP },
            { FltFreq, 900.0f }, { FltEnvAmt, 0.4f }, { E2A, 800.0f }, { E2D, 1500.0f }, { E2S, 0.5f }, { SampleGain, 2.1f } },
          { { Fx::Distortion, 0.4f }, { Fx::Reverb, 0.4f } } },

        { "Pulso de suspense", "Cinemático", "La nota sostenida y cortada a corcheas al tempo del proyecto, oscura y con eco: un latido que inquieta.",
          "Suspense, thriller, tensión rítmica.",
          { { Engine, EngineRebuild }, { SustainType, SustainHold }, { AmpS, 1.0f }, { AmpR, 600.0f }, { Lfo1Sync, 1.0f },
            { Lfo1Div, 4.0f }, { Lfo1Shape, Square }, { M1Src, SrcLfo1 }, { M1Dst, DstVolume }, { M1Amt, 0.3f },
            { FltOn, 1.0f }, { FltType, LP }, { FltFreq, 1200.0f }, { SampleGain, -9.3f } },
          { { Fx::Delay, 0.2f }, { Fx::Reverb, 0.3f } } },

        // Agresivo --------------------------------------------------------------------------------
        { "Fuzz con flanger", "Agresivo", "Un fuzz de pedal muy cargado, un flanger con mucho feedback detrás y compresión que lo pega todo.",
          "Leads y riffs sucios, rock, psicodelia.",
          { { Engine, EngineRebuild }, { AmpR, 300.0f }, { SampleGain, -0.7f } },
          {}, { { main1, Fx::Distortion, 1.0f, { { 0, 14 }, { 1, 0.85f }, { 3, 2 }, { 4, 0 }, { 5, 5000 } } },
                { main1 + 1, Fx::Flanger, 0.5f, { { 0, 0.2f }, { 1, 0.8f }, { 2, 0.75f } } },
                { main1 + 2, Fx::Compressor, 1.0f, { { 0, -24 }, { 1, 6 }, { 4, 12 } } } } },

        { "Sierra oxidada", "Agresivo", "Granos rotos con menos bits, desplazados en frecuencia y sin muestras: una máquina industrial chirriante.",
          "Industrial, techno duro, diseño sonoro.",
          { { Engine, EngineGranular }, { GrainPos, 0.2f }, { GrainMs, 40.0f }, { GrainDensity, 60.0f }, { Spray, 0.7f }, { GrainPitch, 2.0f },
            { GrainWidth, 1.0f }, { AmpA, 5.0f }, { AmpS, 1.0f }, { AmpR, 400.0f }, { Bits, 10.0f }, { SampleGain, 5.5f } },
          {}, { { main1, Fx::Bode, 0.5f, { { 0, -250 }, { 2, 0.5f } } },
                { main1 + 1, Fx::Distortion, 0.5f, { { 0, 8 }, { 1, 0.6f } } },
                { main1 + 2, Fx::Reverb, 0.25f, { { 0, 3 }, { 2, 2 } } } } },

        { "Rectificador", "Agresivo", "Una octava abajo, tres voces, rectificado y con un phaser de ocho etapas: un bajo que ruge y se retuerce.",
          "Bajos agresivos, neurofunk, metal electrónico.",
          { { Engine, EngineResynth }, { EnvMode, EnvSynth }, { AmpA, 3.0f }, { AmpS, 0.9f }, { AmpR, 200.0f }, { Transpose, -12.0f },
            { Unison, 3.0f }, { UniDetune, 15.0f }, { PolyMode, Mono }, { SampleGain, 17.0f } },
          {}, { { main1, Fx::Distortion, 0.8f, { { 0, 10 }, { 1, 0.7f }, { 3, 2 }, { 4, 0 }, { 5, 3000 } } },
                { main1 + 1, Fx::Phaser, 0.5f, { { 0, 0.15f }, { 3, 0.7f }, { 5, 3 } } },
                { main1 + 2, Fx::Utility, 1.0f, { { 0, 6 }, { 3, 150 } } } } },

        { "Saturación paralela", "Agresivo", "El sonido limpio y comprimido delante; detrás, en paralelo, una copia destrozada y filtrada que le da mordida, y una placa.",
          "Dar pegada sin perder definición: baterías, bajos, voces.",
          { { Engine, EngineRebuild }, { FxBus1Send, 0.5f }, { FxBus1Level, 0.6f }, { FxBus2Send, 0.25f }, { SampleGain, 2.0f } },
          {}, { { main1, Fx::Compressor, 1.0f, { { 0, -18 }, { 1, 3 }, { 2, 20 } } },
                { bus1, Fx::Distortion, 1.0f, { { 0, 2 }, { 1, 0.9f } } },
                { bus1 + 1, Fx::Filter, 1.0f, { { 0, 2 }, { 3, 1200 }, { 4, 0.3f } } },
                { bus2, Fx::Reverb, 1.0f, { { 0, 0 }, { 2, 1.8f } } } } },

        // Experimental ----------------------------------------------------------------------------
        { "Lo-fi", "Experimental", "Menos bits y menos muestras, notas que varían y suenan apagadas si tocas suave: un sampler de los 80.",
          "Hip hop, lo-fi, maquetas con carácter.",
          { { Engine, EngineRebuild }, { Bits, 10.0f }, { Downsample, 3.0f }, { VelBright, 0.9f }, { Variation, 0.4f },
            { FltOn, 1.0f }, { FltType, LP }, { FltFreq, 4000.0f }, { SampleGain, 2.1f } },
          { { Fx::Chorus, 0.3f } } },

        { "Texturas glitch", "Experimental", "Granos cortos que saltan de sitio, de afinación y de lado, algunos al revés, con un eco que rebota.",
          "Fills, diseño sonoro, ritmos rotos.",
          { { Engine, EngineGranular }, { GrainPos, 0.2f }, { GrainMs, 30.0f }, { GrainDensity, 40.0f }, { Spray, 0.9f },
            { GrainPitch, 7.0f }, { GrainReverse, 0.4f }, { GrainWidth, 1.0f }, { AmpA, 5.0f }, { AmpS, 1.0f }, { AmpR, 600.0f },
            { Bits, 12.0f }, { M1Src, SrcRandom }, { M1Dst, DstPan }, { M1Amt, 0.6f }, { SampleGain, 4.3f } },
          { { Fx::Delay, 0.3f } } },
        { "Al revés", "Experimental", "El sample tocado del final al principio, con una cola de eco y reverb: crece hacia la nota.",
          "Transiciones, efectos psicodélicos, swells.",
          { { Engine, EngineRebuild }, { Reverse, 1.0f }, { AmpR, 800.0f }, { SampleGain, 5.8f } },
          {}, { { main1, Fx::Reverb, 0.45f, { { 0, 1 }, { 1, 0.9f }, { 2, 5 }, { 3, 0 }, { 5, 150 } } },
                 { main1 + 1, Fx::Delay, 0.2f, { { 0, 1 }, { 3, 6 }, { 4, 6 }, { 6, 0.35f }, { 7, 1 }, { 8, 2000 } } } } },

        { "Robot formante", "Experimental", "Filtro de vocales que salta de vocal a semicorcheas, con menos bits: una voz de máquina que habla.",
          "Efectos vocales, electro, IDM.",
          { { Engine, EngineResynth }, { EnvMode, EnvSynth }, { AmpA, 2.0f }, { AmpS, 0.9f }, { AmpR, 200.0f }, { FltOn, 1.0f },
            { FltType, Vowel }, { FltMorph, 0.5f }, { FltMix, 0.6f }, { Lfo1Sync, 1.0f }, { Lfo1Div, 2.0f }, { Lfo1Shape, SampleHold },
            { M1Src, SrcLfo1 }, { M1Dst, DstMorph }, { M1Amt, 0.6f }, { Bits, 14.0f }, { SampleGain, 8.6f } },
          { { Fx::Delay, 0.15f } } },
        { "Espiral de Bode", "Experimental", "Un desplazador de frecuencia con mucho feedback: el sonido sube o baja sin fin, como una escalera imposible.",
          "Transiciones, efectos de ciencia ficción, rarezas.",
          { { Engine, EngineRebuild }, { AmpR, 1500.0f }, { SampleGain, 1.7f } },
          {}, { { main1, Fx::Bode, 0.6f, { { 0, 40 }, { 1, 0.6f }, { 2, 0.7f } } },
                { main1 + 1, Fx::Reverb, 0.25f, { { 0, 0 }, { 2, 2.5f } } } } },
#include "FactoryBank.inc"     // the 150 of the expansion (research/presets/banco150.py)
    };
    return list;
}
} // namespace Factory
