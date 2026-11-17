#include "TabGuides.h"

namespace TabGuides
{
namespace
{
using Step = GuideOverlay::Step;

// What a step lights up: the whole panel (or row, or slot) its controls belong to, and inside it the
// controls themselves, outlined one by one.
struct Pick
{
    Area target;
    Focus focus;
    Pick (Area a = {}, Focus f = {}) : target (std::move (a)), focus (std::move (f)) {}
};

Area unite (std::vector<Area> areas)
{
    return [areas]
    {
        juce::Rectangle<int> r;
        for (const auto& a : areas)
        {
            const auto x = a ? a() : juce::Rectangle<int>();
            if (! x.isEmpty())
                r = r.isEmpty() ? x : r.getUnion (x);
        }
        return r;
    };
}

Focus join (std::vector<Focus> parts)
{
    return [parts]
    {
        std::vector<juce::Rectangle<int>> all;
        for (const auto& f : parts)
            if (f)
                for (const auto& r : f())
                    all.push_back (r);
        return all;
    };
}

// The first that shows, or the second.
Pick either (Pick a, Pick b)
{
    const auto ta = a.target, tb = b.target;
    const auto fa = a.focus, fb = b.focus;
    return { [ta, tb] { const auto r = ta(); return r.isEmpty() ? tb() : r; },
             [ta, fa, fb]
             {
                 const auto& f = ! ta().isEmpty() ? fa : fb;
                 return f ? f() : std::vector<juce::Rectangle<int>>();
             } };
}

Pick both (Pick a, Pick b)
{
    return { unite ({ a.target, b.target }), join ({ a.focus, b.focus }) };
}

// Writes the steps of one tab, level after level.
struct Writer
{
    const Targets& t;
    const int tab;
    int level = 1;
    std::vector<Step> steps;

    // Things by key, their panels, and the things outlined inside (not when a thing is the panel itself).
    Pick keys (std::vector<juce::String> names) const
    {
        auto rect = t.rect, group = t.group;
        Area target = [group, names]
        {
            juce::Rectangle<int> r;
            for (const auto& n : names)
            {
                const auto x = group (n);
                if (! x.isEmpty())
                    r = r.isEmpty() ? x : r.getUnion (x);
            }
            return r;
        };
        Focus focus = [rect, group, names]
        {
            std::vector<juce::Rectangle<int>> all;
            for (const auto& n : names)
            {
                const auto x = rect (n);
                if (! x.isEmpty() && x != group (n))
                    all.push_back (x);
            }
            return all;
        };
        return { target, focus };
    }
    Pick id (const juce::String& key) const { return keys ({ key }); }
    Pick param (int index) const { return id ("p:" + juce::String (index)); }
    Pick params (std::vector<int> indices) const
    {
        std::vector<juce::String> names;
        for (const int i : indices)
            names.push_back ("p:" + juce::String (i));
        return keys (names);
    }
    Pick section (const char* title) const
    {
        auto rect = t.rect;
        const auto key = "s:" + P::u8 (title);
        return { [rect, key] { return rect (key); } };
    }
    Pick button (const char* text) const { return id ("text:" + P::u8 (text)); }
    Pick buttons (std::vector<const char*> texts) const
    {
        std::vector<juce::String> names;
        for (const auto* x : texts)
            names.push_back ("text:" + P::u8 (x));
        return keys (names);
    }
    Pick area (const char* name) const { return { t.area (name) }; }

    void add (const char* title, const char* text, Pick pick, const char* action, const char* tip)
    {
        Step step { P::u8 (title), P::u8 (text), std::move (pick.target), tab, level, P::u8 (action), P::u8 (tip) };
        step.focus = std::move (pick.focus);
        steps.push_back (std::move (step));
    }
};

//==============================================================================
void samplePage (Writer& w)
{
    w.level = 1;
    w.add ("Carga tu sample",
           "Una instancia nueva empieza con un sample de demostración (Demo · synth pluck) para practicar todos los pasos. "
           "Cargar abre otro archivo; también puedes arrastrar un WAV, AIFF, FLAC, MP3 u OGG sobre la forma de onda. "
           "Debajo del nombre aparece la nota detectada. Snap a C afina el sample al Do más cercano.",
           w.area ("sampleCard"),
           "pulsa Cargar o arrastra un archivo desde el Explorador o desde Ableton.",
           "un sonido de una sola nota, sin otros sonidos detrás.");
    w.add ("La forma de onda",
           "Es tu sample. La línea INICIO marca dónde empieza la nota y se arrastra por su pestaña. La curva naranja es "
           "la envolvente y sus puntos también se arrastran. La rueda del ratón acerca y aleja.",
           w.area ("waveform"),
           "arrastra la pestaña INICIO un poco a la derecha y toca una nota.",
           "deja INICIO justo antes del golpe del sonido.");
    w.add ("El motor",
           "Decide cómo se rehace el sample. A Reconstrucción suena como el original; D Híbrido conserva el golpe "
           "(pianos, percusión); C Resíntesis y B Reenvolvente lo vuelven sintetizador; E Granular hace texturas y "
           "F Drone Maker, drones. Cada motor muestra sólo los mandos que usa.",
           w.area ("engineCard"),
           "toca la misma nota con el motor A y después con el D.",
           "A Reconstrucción.");
    w.add ("La envolvente",
           "Ataque, Decay, Sustain y Release: cómo entra, cae, se mantiene y se apaga cada nota. Detectado vuelve a "
           "los valores medidos en el sample. El candado conserva tu envolvente al cargar otro sample.",
           w.section ("ENVOLVENTE"),
           "sube Release, toca una nota corta y escucha cómo se apaga más despacio.",
           "pulsa Detectado y cambia sólo un mando cada vez.");
    w.add ("Volumen",
           "El volumen final del plugin. Empieza en -6 dB para que quepan acordes y efectos. En el medidor de SALIDA, hasta 0 dB "
           "suena limpio; la zona roja, de 0 a +6 dB, es una saturación suave que crece poco a poco y nunca pasa de 0 dBFS.",
           w.param (P::Volume),
           "toca un acorde fuerte y mira el medidor de SALIDA.",
           "limpio: por debajo del rojo; con color: entre 0 y +3 dB; saturación clara: hacia +6 dB.");

    w.level = 2;
    w.add ("Warp",
           "Cómo cambia el tono lejos de la nota del sample. Re-Pitch es el sampler clásico: más agudo, más rápido. "
           "Tonos mantiene la duración y el timbre; Textura es para pads y ambientes.",
           either (w.param (P::WarpMode), w.section ("MOTOR")),
           "elige Tonos y toca una octava por encima de la nota del sample.",
           "Tonos para voces, vientos, bajos y teclas; Textura para pads.");
    w.add ("Loop y Slice",
           "El botón LOOP / ONE SHOT del MOTOR cambia entre tocar una nota y tocar un loop. En LOOP, Modo loop elige Normal "
           "(el loop sigue el tempo del proyecto con Warp; Play lo hace sonar con el transporte del DAW) o Slice (un trozo por "
           "tecla desde C3, cortado por golpes o por rejilla). Calidad elige cómo se estira; Auto acierta casi siempre.",
           either (w.param (P::LoopKind), w.section ("MOTOR")),
           "carga un loop de batería, elige Slice y toca C3, C#3, D3...",
           "Rejilla 1/16 para baterías cuadradas; Transitorios para ritmos sueltos.");
    w.add ("Sustain",
           "Natural: la nota cae como en el sample, como con el pedal de un piano. Sostenido: se mantiene mientras "
           "pulsas la tecla, como un sintetizador. Está en los motores A, B, C y D.",
           either (w.param (P::SustainType), w.section ("MOTOR")),
           "elige Sostenido y mantén una tecla pulsada.",
           "Natural para pianos y plucks; Sostenido para pads y leads.");
    w.add ("Modo ADSR y Golpe",
           "Remodelar cambia la forma del sample sin perder su carácter. Sintetizador lo vuelve un tono plano que la "
           "envolvente modela desde cero. Golpe (motor A) crea la pegada que falta si pones un ataque más rápido que el "
           "del sample.",
           w.params ({ P::EnvMode, P::Punch }),
           "con el motor A, baja Ataque al mínimo y sube Golpe.",
           "Remodelar.");
    w.add ("Brillo y Apagado",
           "Brillo: tocar suave suena más oscuro, como en un instrumento real. Apagado: los agudos se apagan antes que "
           "los graves al soltar la tecla.",
           w.params ({ P::VelBright, P::RelDark }),
           "toca la misma nota muy suave y muy fuerte.",
           "los dos al 50 %.");
    w.add ("Voz y afinación",
           "Modo Poly toca acordes; Mono y Legato, una nota. Glide desliza entre notas. Transponer y Fino afinan. "
           "Voces y Detune engordan el sonido con copias ligeramente desafinadas.",
           w.section ("VOZ · AFINACIÓN"),
           "sube Voces a 5 y Detune a 25 % y toca un acorde.",
           "para un bajo o un lead: Mono con Glide de unos 80 ms.");

    w.level = 3;
    w.add ("Mecanismo",
           "Añade el gesto de un instrumento real, grabado: el golpe al pulsar y al soltar. El tipo se elige en el menú "
           "junto al título TIMBRE (» marca el que mejor encaja con tu sample). Mecanismo es cuánto suena; Entrada y "
           "Salida, el golpe al pulsar y al soltar; Adaptar lo funde con el sample.",
           w.params ({ P::MechType, P::RelNoise, P::MechDown, P::MechUp, P::MechAdapt }),
           "sube Mecanismo al 50 % y prueba varios tipos del menú.",
           "Piano real para pianos, Rhodes, Wurlitzer o clavinet; Piano para plucks, arpas y leads.");
    w.add ("Oír todo, nota o golpe",
           "Cambia lo que oyes para juzgar Mecanismo: todo, sólo la nota o sólo el golpe. No se guarda en el proyecto.",
           w.id ("listen"),
           "púlsalo hasta que diga Oír golpe y toca una nota.",
           "vuelve a Oír todo antes de seguir.");
    w.add ("Movimiento en la nota",
           "Variación hace cada nota algo distinta para que las repeticiones no suenen iguales. Respiración mueve "
           "despacio volumen y afinación en las notas sostenidas. Textura mezcla granos del propio sample. Están en "
           "los motores A, B, C y D.",
           either (w.params ({ P::Variation, P::Breath, P::GrainMix }), w.section ("MOTOR")),
           "sube Variación y repite la misma nota varias veces.",
           "Variación 20 %.");
    w.add ("Color lo-fi",
           "Bits reduce la resolución y Muestreo baja la frecuencia de muestreo, como un sampler antiguo.",
           w.params ({ P::Bits, P::Downsample }),
           "baja Bits a 8 y escucha el grano que aparece.",
           "Bits 12 y Muestreo 2x para un color de sampler clásico.");
}

//==============================================================================
void editPage (Writer& w)
{
    w.level = 1;
    w.add ("La vista de edición",
           "Arrastra sobre la onda para seleccionar; un clic marca dónde pegar. La rueda acerca y aleja; con Mayúsculas "
           "se desplaza. Las pestañas INICIO, FIN y BUCLE se arrastran. El archivo original nunca se toca.",
           w.area ("editView"),
           "arrastra sobre el principio del sonido para seleccionarlo.",
           "acerca con la rueda antes de seleccionar con precisión.");
    w.add ("Cortar y pegar",
           "Recortar deja sólo la selección; Borrar la quita; Silencio la deja muda sin cambiar la duración. Copiar, "
           "Cortar y Pegar funcionan como en un editor de texto, también entre samples.",
           w.buttons ({ "Recortar", "Borrar", "Silencio", "Copiar", "Cortar", "Pegar" }),
           "selecciona el silencio del principio y pulsa Borrar.",
           "si te equivocas, pulsa Deshacer.");
    w.add ("Deshacer y Rehacer",
           "Recuperan cualquier cambio: una edición, un mando, un preset o un sample cargado.",
           w.buttons ({ "Deshacer", "Rehacer" }),
           "pulsa Deshacer para recuperar lo que borraste.",
           "edita sin miedo: todo se puede deshacer.");
    w.add ("Recorte y nivel",
           "Fin marca dónde acaba el sample; los fundidos quitan clics al empezar y al acabar. Ganancia sube o baja "
           "el sample antes de todo, y Normalizar lo lleva a 0 dB de pico. Fin y los fundidos actúan con el motor A.",
           w.section ("RECORTE Y NIVEL"),
           "sube Fundido entrada a unos 5 ms si oyes un clic al empezar.",
           "Ganancia a 0 dB y Normalizar apagado.");

    w.level = 2;
    w.add ("Fundidos y nivel de la selección",
           "Fundido ↗ y Fundido ↘ escriben un fundido en la parte seleccionada. Normalizar lleva su pico a -0,1 dB y "
           "Ganancia ▾ la sube o baja.",
           w.buttons ({ "Fundido ↗", "Fundido ↘", "Normalizar", "Ganancia ▾" }),
           "selecciona el final del sonido y pulsa Fundido ↘.",
           "fundidos cortos en los cortes, largos en las colas.");
    w.add ("Bucle",
           "Repite una zona del sample mientras sostienes la nota. On lo activa; Inicio bucle y Fin bucle son las "
           "líneas verdes; Cruce funde el salto para que no se oiga. Actúa con el motor A.",
           w.section ("BUCLE"),
           "activa On, mantén una nota y mueve Fin bucle.",
           "una zona estable del sonido y Cruce de 30 a 60 ms.");
    w.add ("Envolvente de pitch",
           "Cambia el tono al empezar la nota y lo devuelve a su sitio: positivo cae como un 808 o un tom; negativo, "
           "sube. Tiempo pitch es lo que tarda en volver.",
           w.section ("ENVOLVENTE DE PITCH"),
           "pon Pitch env. en +12 st y toca una nota grave.",
           "+12 st y unos 120 ms para un bombo o un 808.");

    w.add ("Orden de los trozos y exportar",
           "Con LOOP en Slice, la tira muestra qué trozo toca cada tecla: arrastra una casilla para cambiar el orden. "
           "EXPORTAR MIDI crea un clip con los trozos en ese orden (arrástralo a una pista de Ableton o haz clic para "
           "guardarlo). EXPORTAR AUDIO graba lo que suena, con todos los efectos.",
           w.buttons ({ "EXPORTAR AUDIO", "EXPORTAR MIDI" }),
           "en Slice, mueve la primera casilla al final y arrastra EXPORTAR MIDI a Ableton.",
           "en Ableton, el clip toca el loop reordenado con Reanima en Slice.");

    w.level = 3;
    w.add ("Bucle sin costura",
           "Escribe en el audio el fundido cruzado del bucle, con la duración de Cruce, para que el salto no se oiga en "
           "ningún motor.",
           w.button ("Bucle sin costura"),
           "con el bucle activo, púlsalo y escucha una nota larga.",
           "ajusta antes Inicio bucle y Fin bucle a una zona estable.");
    w.add ("Cruces por cero",
           "Lleva los bordes de la selección y el punto de pegado al cruce por cero más cercano, para que los cortes "
           "no hagan clic.",
           w.button ("Cruces por cero"),
           "selecciona una zona y púlsalo antes de Borrar o Cortar.",
           "úsalo siempre antes de cortar en mitad de un sonido.");
    w.add ("Compara con A/B",
           "A y B guardan dos ajustes; con = nivel suenan igual de fuerte, así eliges por cómo suenan y no por el "
           "volumen. Las ediciones del sample se comparan igual.",
           w.area ("ab"),
           "pulsa B, haz un cambio y alterna entre A y B.",
           "deja = nivel encendido.");
}

//==============================================================================
void filterPage (Writer& w)
{
    w.level = 1;
    w.add ("Enciende el filtro",
           "El filtro quita o realza frecuencias. On lo enciende; apagado no gasta nada.",
           w.param (P::FltOn),
           "pulsa On y gira Frecuencia mientras tocas.",
           "enciéndelo sólo cuando lo necesites.");
    w.add ("Tipo, Pendiente y Circuito",
           "Tipo: Lowpass deja pasar los graves, Highpass los agudos, Bandpass una banda; Morph, DJ, Comb y Vowel son "
           "especiales. Pendiente: 12 o 24 dB, suave o marcado. Circuito: Clean es limpio; los demás añaden color "
           "analógico.",
           w.params ({ P::FltType, P::FltSlope, P::FltCircuit }),
           "cambia entre Lowpass y Highpass con la Frecuencia en el centro.",
           "Lowpass, 24 dB, Clean.");
    w.add ("Frecuencia y Resonancia",
           "Frecuencia es el punto de corte. Resonancia realza las frecuencias junto al corte; mucha suena aguda y "
           "nasal. La curva de arriba dibuja el resultado.",
           w.params ({ P::FltFreq, P::FltRes }),
           "baja Frecuencia hasta que el sonido se oscurezca.",
           "Frecuencia sobre 2 kHz y Resonancia 20 %.");
    w.add ("ENV 2",
           "Una segunda envolvente para mover el filtro en cada nota. En la matriz puede mover otros destinos.",
           w.section ("ENV 2"),
           "baja su Sustain y sube Env 2 → en el filtro.",
           "Ataque corto, Decay de unos 300 ms y Sustain bajo.");

    w.level = 2;
    w.add ("Env 2 → y Keytrack",
           "Env 2 → es cuánto mueve ENV 2 el corte, hacia arriba o hacia abajo. Keytrack hace que el corte siga la nota: "
           "al 100 % sube una octava por octava.",
           w.params ({ P::FltEnvAmt, P::FltKey }),
           "sube Env 2 → al 50 % y toca notas cortas.",
           "Keytrack 50 % para que las notas agudas no queden apagadas.");
    w.add ("LFO",
           "Ondas lentas que mueven lo que conectes en MATRIZ: solas no hacen nada. Forma, Velocidad o División con "
           "Sync (sigue el tempo), y Retrig, que las reinicia en cada nota.",
           both (w.section ("LFO 1"), both (w.section ("LFO 2"), w.section ("LFO 3"))),
           "activa Sync y elige una División; después conéctalo en MATRIZ.",
           "una forma suave y Retrig encendido.");
    w.add ("Macros",
           "Cuatro mandos libres. En MATRIZ cada uno puede mover varios destinos a la vez, y en Ableton se automatizan "
           "o se asignan a tu controlador.",
           w.section ("MACROS"),
           "conecta Macro 1 en MATRIZ y gíralo aquí.",
           "un macro para el brillo y otro para el espacio.");
    w.add ("Expresión MIDI",
           "Aftertouch: apretar la tecla abre y sube el sonido. Bend: semitonos de la rueda de pitch. Vibrato y Vel. "
           "vibrato: el vibrato de la rueda de modulación.",
           w.section ("EXPRESIÓN MIDI"),
           "mueve la rueda de modulación de tu teclado mientras tocas.",
           "Bend 2 st y Vibrato 50 %.");

    w.level = 3;
    w.add ("Drive y Morph",
           "Drive satura el filtro (con un Circuito distinto de Clean). Morph/DJ se mueve entre tipos en Morph, de grave "
           "a agudo en DJ y entre vocales en Vowel.",
           w.params ({ P::FltDrive, P::FltMorph }),
           "elige el tipo Vowel y mueve Morph/DJ.",
           "Drive de 3 a 6 dB para dar cuerpo.");
    w.add ("Follower",
           "El propio volumen del sonido mueve el corte: los golpes lo abren. Fol. ataque y Fol. release fijan lo rápido "
           "que sigue.",
           w.params ({ P::FltFollow, P::FltFolAtk, P::FltFolRel }),
           "sube Follower y toca fuerte y suave.",
           "Fol. ataque 5 ms y Fol. release 100 ms.");
    w.add ("Dry/Wet",
           "Mezcla el sonido filtrado con el original: menos de 100 % filtra en paralelo y conserva cuerpo.",
           w.param (P::FltMix),
           "con un Highpass alto, baja Dry/Wet al 50 %.",
           "100 % para filtrar; 50-70 % para dar color.");
}

//==============================================================================
void matrixPage (Writer& w)
{
    const int row1 = P::M1Src, row2 = P::M1Src + P::slotStride, row3 = P::M1Src + 2 * P::slotStride;

    w.level = 1;
    w.add ("La matriz",
           "Ocho filas para conectar una fuente de movimiento con lo que mueve. Cada fila es Fuente → Destino → "
           "Cantidad. Una fila con Fuente — no hace nada.",
           w.section ("MATRIZ DE MODULACIÓN"),
           "", "rellena las filas de arriba abajo.");
    w.add ("Fuente",
           "Lo que mueve: LFO 1-3, Env 2, Env Amp, Velocidad, Nota, Mod Wheel, Aftertouch, Macro 1-4 o Aleatorio "
           "(un valor nuevo en cada nota).",
           w.param (row1),
           "elige LFO 1 en la primera fila.",
           "LFO 1 para movimiento; Velocidad para que responda a tu forma de tocar.");
    w.add ("Destino",
           "Lo que se mueve: Tono, Volumen, Pan, frecuencia, resonancia, drive o morph del filtro, Inicio, Spray, Grano, "
           "Detune unison, la velocidad de los LFO 1 y 2, Transitorio, Arco/aliento o Respiración.",
           w.param (row1 + 1),
           "elige Tono como destino.",
           "empieza por Tono, Volumen o Frecuencia filtro.");
    w.add ("Cantidad",
           "Cuánto y en qué sentido mueve, de -100 % a +100 %. Al 100 %: Tono ±24 st, Volumen ±24 dB, Frecuencia ±60 st, "
           "Drive ±24 dB.",
           w.param (row1 + 2),
           "sube la Cantidad poco a poco mientras tocas.",
           "cantidades pequeñas: un 2 % de Tono ya es medio semitono.");

    w.level = 2;
    w.add ("Receta: vibrato",
           "LFO 1 → Tono con una Cantidad pequeña. La velocidad se ajusta en el LFO 1 de FILTRO · MOD.",
           w.params ({ row1, row1 + 1, row1 + 2 }),
           "fila 1: LFO 1, Tono, Cantidad 2 %.",
           "LFO 1 a unos 5 Hz.");
    w.add ("Receta: wah",
           "Env 2 → Frecuencia filtro: cada nota abre el filtro y se cierra. Necesita el filtro encendido.",
           w.params ({ row2, row2 + 1, row2 + 2 }),
           "fila 2: Env 2, Frecuencia filtro, Cantidad 30 %.",
           "filtro Lowpass con la Frecuencia baja.");
    w.add ("Receta: dinámica",
           "Velocidad → Frecuencia filtro: tocar fuerte abre el sonido. Velocidad → Volumen da más contraste.",
           w.params ({ row3, row3 + 1, row3 + 2 }),
           "fila 3: Velocidad, Frecuencia filtro, Cantidad 25 %.",
           "entre 15 % y 30 %.");

    w.level = 3;
    w.add ("Un macro, varios destinos",
           "Pon la misma fuente en varias filas con destinos distintos: un Macro puede abrir el filtro y subir el "
           "drive a la vez. Un solo mando, un cambio grande y controlado.",
           w.section ("MATRIZ DE MODULACIÓN"),
           "pon Macro 1 en dos filas: Frecuencia filtro y Drive filtro.",
           "cantidades opuestas compensan: +Frecuencia y -Volumen.");
    w.add ("Aleatorio",
           "Un valor nuevo en cada nota: hacia Pan reparte las notas en el estéreo; hacia Inicio hace que cada nota "
           "empiece en otro punto del sample.",
           w.param (row1),
           "Aleatorio → Pan con Cantidad 30 %.",
           "pequeño: lo justo para que suene vivo.");
    w.add ("Modular la modulación",
           "Una fila puede cambiar la velocidad de los LFO 1 y 2: por ejemplo, Mod Wheel → Vel. LFO 1 acelera el "
           "vibrato al subir la rueda.",
           w.param (row1 + 1),
           "Mod Wheel → Vel. LFO 1, con LFO 1 → Tono en otra fila.",
           "comprueba cada fila sola antes de combinarla.");
}

//==============================================================================
void fxPage (Writer& w)
{
    const Pick chains = both (w.section ("MAIN"), both (w.section ("BUS 1"), w.section ("BUS 2")));

    w.level = 1;
    w.add ("Tres cadenas",
           "MAIN procesa todo el sonido, en orden de arriba abajo. BUS 1 y BUS 2 son cadenas en paralelo que reciben "
           "una copia de la señal.",
           chains,
           "", "empieza poniendo efectos sólo en MAIN.");
    w.add ("Elige un efecto",
           "Cada ranura tiene un menú de efectos: Hyper/Dimension, Distorsión, Flanger, Phaser, Chorus, Delay, "
           "Compresor, Reverb, EQ, Filtro, Bode, Utility y los splitters. On lo enciende; Mix mezcla el efecto con el "
           "sonido.",
           w.id ("fxslot:0"),
           "elige Reverb en la primera ranura.",
           "un efecto cada vez, escuchando el resultado.");
    w.add ("Los mandos del efecto",
           "Al pulsar una ranura, sus mandos aparecen aquí debajo.",
           w.id ("fx-editor"),
           "con la Reverb elegida, cambia su tamaño y su decay.",
           "deja los valores de partida y ajusta poco a poco.");
    w.add ("Nivel de Main",
           "El volumen que sale de la cadena MAIN.",
           w.param (P::FxMainLevel),
           "bájalo un poco si los efectos suben el volumen.",
           "al 100 % mientras no sature.");

    w.level = 2;
    w.add ("Buses en paralelo",
           "Envío manda una copia al bus; Nivel es su volumen de salida; → Main lo pasa después por la cadena MAIN.",
           w.section ("BUS 1"),
           "pon un Delay en BUS 1 y sube Envío.",
           "Envío 20-30 %: el eco detrás, el sonido delante.");
    w.add ("Reordenar",
           "Arrastra una ranura por su asa ⋮⋮ para cambiar el orden o moverla a otra cadena. El orden cambia el sonido: "
           "la distorsión antes de la reverb no suena igual que después.",
           both (w.id ("fxslot:0"), w.id ("fxslot:1")),
           "arrastra la ranura 2 encima de la 1.",
           "Distorsión → EQ → Compresor → Delay → Reverb.");
    w.add ("Mix",
           "La barra Mix de cada ranura mezcla el efecto con el sonido sin efecto.",
           w.id ("fxslot:0"),
           "con una Reverb, baja Mix hasta que el sonido vuelva delante.",
           "Reverb al 20-30 %.");

    w.level = 3;
    w.add ("Splitters",
           "Splitter L/H, L/M/H y M/S dividen la señal en graves y agudos, en tres bandas o en centro y lados. Tras un "
           "splitter, los efectos siguientes sólo procesan las bandas marcadas como Procesar.",
           w.id ("fx-editor"),
           "pon Splitter L/H y después una Distorsión sólo en los agudos.",
           "los graves limpios, el color en los agudos.");
    w.add ("Bus → Main",
           "Con → Main encendido, la salida del bus pasa también por MAIN: un compresor o una EQ en MAIN tratan todo "
           "junto.",
           w.param (P::FxBus1ToMain),
           "enciéndelo con un Compresor en MAIN.",
           "apagado si MAIN tiene efectos que no quieres en el eco.");
    w.add ("Sin exceso",
           "Cada efecto suma volumen y quita claridad. Compara con A/B y = nivel: si el sonido con efectos sólo gana "
           "por sonar más fuerte, sobra algún efecto.",
           both (chains, w.area ("ab")),
           "apaga un efecto con On y escucha si lo echas de menos.",
           "pocos efectos y bien ajustados.");
}
} // namespace

Area Targets::area (const juce::String& name) const
{
    const auto found = areas.find (name);
    if (found != areas.end())
        return found->second;
    return [] { return juce::Rectangle<int>(); };
}

juce::String tabName (int tab)
{
    const char* names[] { "SAMPLE", "EDICIÓN", "FILTRO · MOD", "MATRIZ", "FX" };
    return P::u8 (names[juce::jlimit (0, numTabs - 1, tab)]);
}

std::vector<GuideOverlay::Step> forTab (int tab, const Targets& targets)
{
    Writer w { targets, tab };
    switch (tab)
    {
        case 0: samplePage (w); break;
        case 1: editPage (w); break;
        case 2: filterPage (w); break;
        case 3: matrixPage (w); break;
        case 4: fxPage (w); break;
        default: break;
    }
    return std::move (w.steps);
}
} // namespace TabGuides
