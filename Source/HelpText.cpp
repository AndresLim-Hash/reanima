#include "HelpText.h"
#include "FxRack.h"

namespace
{
const char* const onlyLegacy = " Sólo motores B, C y D.";
const char* const onlyGrainEngines = " Sólo motores B y D.";
const char* const onlyResynth = " Sólo motor C.";

const char* controlHelp (int index)
{
    using namespace P;

    switch (index)
    {
        case Engine:        return "Cómo se genera el sonido.\nA · Reconstrucción: toca el sample tal cual y, cuando tu ADSR pide más de lo que el sample tiene, lo completa con audio reconstruido. Es el más realista.\nB · Reenvolvente: estira el sample con granos.\nC · Resíntesis: lo rehace con senos y ruido.\nD · Híbrido: igual que B, pero conserva el transitorio original.\nE · Granular: una nube de granos.\nF · Drone Maker: estira el sample hasta mil veces y lo convierte en un drone.";
        case SustainMode:   return "Qué mantiene viva la nota en los motores B y D.\nLoop: bucle sin costuras.\nGranular: granos del sample.\nFreeze: sonido congelado que respira.";
        case SustainType:   return "Natural: la nota cae como el sample, igual que con el pedal de un piano.\nSostenido: se mantiene mientras pulsas la tecla, como un sintetizador. En este modo no se aplica Caída sus.";
        case Stretch:       return "Estira o comprime el sample para que ataque y decay duren lo que marcan los mandos, sin cambiar la afinación. Apagado, sólo cambia el volumen.";
        case Flatten:       return "Quita los altibajos de volumen del sample antes de aplicar tu envolvente. A 0 % respeta su forma; a 100 % queda plano.";
        case GrainMs:       return "Duración de cada grano. Corto suena más granulado; largo, más suave. Motores B, D y E.";
        case Spray:         return "Cuánto se dispersan los granos alrededor de su posición. Más spray, sonido más difuso. Motores B, D y E.";
        case GranSpeed:     return "Hacia dónde y a qué velocidad avanzan los granos mientras la nota se sostiene. A 0 se quedan en su sitio (congelado); a +100 % avanzan a la velocidad del sample.";
        case Breath:        return "Movimiento lento de volumen y afinación mientras la nota se sostiene, como un arco o el aliento. Sólo actúa en modo Sostenido.";
        case TransientMs:   return "Duración del golpe original que se conserva al principio de la nota.";
        case TransientGain: return "Volumen de ese golpe original. Súbelo para más pegada, bájalo para suavizar la entrada.";
        case StartPos:      return "Punto del archivo donde empieza la nota, en porcentaje. También puedes arrastrar la línea INICIO sobre la forma de onda.";
        case Reverse:       return "Toca el sample al revés. Reanaliza el sonido y pone los mandos en los valores detectados.";
        case Transpose:     return "Sube o baja el tono en semitonos.";
        case Fine:          return "Afinación fina en centésimas de semitono.";
        case SnapC:         return "Apagado: Do3 toca el sample tal cual. Encendido: el sample se transporta al Do más cercano a su nota (un La2 sube a Do3) y todo el teclado queda afinado.";
        case Bits:          return "Resolución del sonido. A 24 no hace nada; cuanto más baja, más sucio y digital.";
        case Downsample:    return "Reduce la frecuencia de muestreo para un sonido lo-fi. A 1 no hace nada. Suena igual a cualquier frecuencia del proyecto.";
        case Variation:     return "Hace cada nota un poco distinta en afinación, volumen y brillo, para que las repeticiones no suenen a metralleta. No toca el ataque ni la envolvente.";

        case Bow:           return "Cuanto más lento es el ataque, más oscuro empieza el sonido y se abre al llegar arriba, como un arco o un soplido. Con el motor A actúa si alargas el ataque.";
        case RelDark:       return "Los agudos se apagan antes que los graves, como en una cuerda o un piano: al soltar la tecla y, en la parte que Reanima sostiene, mientras la mantienes.";
        case VelBright:     return "Cuanto más suave tocas, más oscura suena la nota, como en un instrumento real. A 0 todas suenan igual de brillantes.";
        case RelNoise:      return "Volumen del gesto del instrumento (elige el tipo en el menú de TIMBRE): un golpe al pulsar y otro al soltar y, con Arco y Soplo, su roce o aire mientras suena. A un cuarto apenas se oye; al máximo queda unos 9 dB bajo el sample. Entrada y Salida ajustan cada golpe; On/Off lo apaga para comparar.";
        case MechType:      return "Qué gesto real acompaña a la nota (grabaciones de instrumentos). El menú marca con » el que mejor encaja con este sample; puedes elegir cualquiera.\nPiano: tecla y macillo, apagador y pedal (CC64). Va mejor con sintes y plucks.\nPiano real: macillo de pianos de verdad. Para pianos, Rhodes, Wurlitzer y clavinet.\nPúa / Púa guitarra: la púa al atacar y los dedos al parar.\nPulsado real: dedo sobre cuerda (arpa, pizzicato).\nArco / Arco real: el arco al morder, su roce mientras suena y al dejarlo.\nTecla: contacto de órgano o martinete de clave.\nSoplo / Soplo real: lengua y aire al empezar, aire mientras suena.\nDel sample: el propio ataque del sample, a la altura de la nota.\nLas versiones «real» suelen ir mejor con instrumentos grabados. Un Ataque lento suaviza los golpes.";
        case MechAdapt:     return "Cuánto se funde el gesto con el sample: cae en el primer pico del ataque, con su polaridad, toma su color y resuena en la nota que suena. A 0, la grabación tal cual.";
        case MechDown:      return "Volumen del golpe de entrada: lo que suena al pulsar la tecla (el macillo, la púa, la mordida del arco, la lengua del soplo, el pedal al pisarlo), en dB sobre lo que marca Mecanismo. A -60 dB no suena.";
        case MechUp:        return "Volumen del golpe de salida: lo que suena al soltar la tecla (el apagador, los dedos parando la cuerda, el arco al dejarla, el aire al terminar, el pedal al soltarlo), en dB sobre lo que marca Mecanismo. A -60 dB no suena.";
        case MechOn:        return "Enciende o apaga el Mecanismo sin tocar sus ajustes, para comparar el sample solo con el sample y su gesto.";
        case WarpMode:      return "Qué pasa con la duración y el timbre al tocar lejos de la nota del sample.\nRe-Pitch: sampler clásico; más agudo es más rápido y más corto.\nTonos: misma duración y mismo timbre (formantes). Para notas sueltas con tono claro: voces, vientos, cuerdas, bajos, teclas. Si al cargar el sample Reanima ve que es un acorde o no tiene un tono estable, usa Textura y el menú lo indica («Tonos → Textura»).\nTextura: misma duración; el timbre sube y baja con la nota. Para pads, acordes y ruidos.\nEl ataque siempre suena entero. En el motor E, la nube avanza a su velocidad sea cual sea la nota.";
        case Partials:      return "Número de senos usados para rehacer el sonido. Menos suena más simple y gasta menos.";
        case NoiseLevel:    return "Volumen de la parte de ruido del sonido: aire, roce, soplido.";

        case AmpA:          return "Tiempo hasta el máximo volumen. Empieza en el valor detectado en el sample; más largo crea una entrada progresiva.";
        case AmpCurve:      return "Forma del ataque. Por debajo de 1 sube rápido al principio; por encima, arranca lento y acelera.";
        case AmpD:          return "Tiempo que tarda en caer desde el máximo hasta el nivel de Sustain.";
        case AmpS:          return "Nivel que se mantiene mientras la tecla sigue pulsada. Si es mayor que el del sample, el motor A completa lo que falta.";
        case AmpFade:       return "Cuántos dB por segundo cae el sustain, como una nota de piano que se apaga. A 0 no cae. No se aplica en modo Sostenido.";
        case AmpR:          return "Tiempo que tarda en apagarse la nota al soltar la tecla. Con el motor A, si has cambiado la envolvente nunca baja de 60 ms, para que el final no golpee.";
        case VelAmp:        return "Cuánto influye la fuerza de la nota en el volumen. A 0 todas suenan igual.";

        case PolyMode:      return "Poly: varias notas a la vez.\nMono: una sola nota.\nLegato: una sola nota que no vuelve a atacar si la anterior sigue pulsada.";
        case Glide:         return "Tiempo que tarda el tono en deslizarse de una nota a la siguiente.";
        case Unison:        return "Voces de Unison, como en Serum: copias de cada nota sonando a la vez, de 1 a 16. Dan cuerpo y anchura; cada voz gasta CPU.";
        case UniDetune:     return "Cuánto se separan en afinación las voces de Unison. Fino al principio y amplio al final: a 100 % las exteriores quedan a ±50 cents.";
        case UniBlend:      return "Volumen de las copias exteriores de Unison respecto a la central.";
        case UniWidth:      return "Separación estéreo de las copias de Unison.";
        case Volume:        return "Volumen general, antes de la etapa de salida. De serie a -6 dB para que quepan acordes y efectos. En el medidor de SALIDA: hasta 0 dB, limpio; de 0 a +6 dB, saturación suave y progresiva; la salida nunca pasa de 0 dBFS. La expresión (CC11) también lo baja.";
        case Pan:           return "Posición en el estéreo.";
        case MonoBass:      return "Por debajo de esta frecuencia el sonido sale en mono: los graves suenan sólidos y no se cancelan al sumar a mono. Sólo se quita la diferencia entre los canales en los graves; lo común no se toca. 0 lo desactiva.";
        case GrainFade:     return "Motores B y D: cuánto tarda el sample en pasar a su parte granular (o al bucle o al congelado), con un fundido de potencia constante.";
        case PlayMode:      return "One shot: el sample suena como una nota, con su ataque, su caída y el motor elegido.\nLoop: el sample como loop, en Normal (en bucle mientras mantienes la tecla, o solo con Play del DAW) o Slice (un trozo por tecla). Al cargar un sample se elige solo cuando está claro que es un loop; si hay duda queda en One shot.";
        case LoopWarp:      return "Loop con Warp: la duración la marca el tempo y el tono lo marca la tecla, cada uno por su lado. Apagado, como un sampler clásico: más agudo es más rápido.";
        case LoopSync:      return "El loop se estira o se encoge para durar lo mismo en tiempos al tempo del proyecto. Apagado, conserva su tempo original (y las teclas siguen cambiando sólo el tono).";
        case LoopBeats:     return "La duración musical del loop. Auto usa la detectada (el nombre, la duración exacta o el ritmo). Corrígela si el loop va al doble o a la mitad del tempo, o si la detección no la encontró.";
        case LoopAlgo:      return "Cómo se estira el loop con Warp. En su tono y tempo originales suena tal cual con cualquier opción.\nAuto: Beats si el loop es rítmico, Complex si no.\nBeats: cada golpe intacto en su sitio; más lento, la cola se alarga sin huecos. Baterías y ritmos.\nComplex: alta calidad para melodías, acordes y mezclas, con poca CPU.\nComplex Pro: conserva los formantes al transponer (voces). Puede quedar 1-3 dB más bajo; en bajos, mejor Complex.\nRubber Band: más pegada en golpes y guitarras. Se calcula en segundo plano en un segundo o dos; mientras, suena Complex.";
        case LoopKind:      return "Normal: el loop entero suena en bucle mientras mantienes la tecla, ajustado al tempo con Warp; Do3 lo toca en su tono original y las demás teclas sólo cambian el tono.\nSlice: el loop se corta en trozos y cada tecla, desde Primera nota (Do3), toca uno entero, a su velocidad original.";
        case LoopBpm:       return "El tempo original del loop. En 0 (Auto) usa el detectado (por el nombre, la duración o el ritmo). Escríbelo si el loop sigue mal el tempo del proyecto.";
        case SliceBy:       return "Cómo se corta el loop en Slice.\nTransitorios: en cada golpe que se detecta (Sensibilidad decide cuántos).\nRejilla: en partes iguales del compás (División).\nA mano: los marcadores que pongas en la forma de onda (doble clic añade o quita, arrastra para mover).";
        case SliceDivision: return "Slice con Rejilla: el tamaño de cada trozo. 1/16 da cuatro trozos por tiempo. Cada corte se adelanta hasta el golpe que empiece justo antes, para no partir un ataque.";
        case SliceSensitivity: return "Slice con Transitorios: más alto corta también en los golpes suaves (más trozos); más bajo, sólo en los fuertes.";
        case SliceStart:    return "La tecla que toca el primer trozo en Slice (Do3 de serie). Los siguientes van en las teclas consecutivas hacia arriba.";
        case LoopPlay:      return "Normal: el loop suena solo con el Play del DAW, alineado al compás (su principio en el compás 1), y se para al parar. Sigue los saltos y cambios de tempo. Mientras suena, las teclas solo cambian su tono, sin reiniciarlo. Apagado, suena mientras mantienes una tecla.";
        case SliceThru:     return "Slice: cada tecla toca desde su trozo hasta el final del sample, en vez de pararse en el corte siguiente (como poner la aguja en ese punto del disco). Varias teclas siguen sonando a la vez; repetir una tecla la reinicia.";
        case SliceFade:     return "Slice: el fundido antes del final de cada trozo (con Thru, antes del final del sample), para que un trozo cortado en mitad de un sonido no chasquee. De serie 2 ms: no se oye y no toca los golpes. Súbelo si oyes un clic al final de trozos largos; en 0 no hay fundido.";
        case LoopHitEnv:    return "Envolvente y Env 2 (pestaña FILTRO) por golpe.\nNormal: vuelven a empezar en cada golpe del loop (sin cortes), así Ataque, Decay y Sustain dan forma a cada golpe y el filtro se abre con cada uno. Los golpes son los cortes de Slice; Sensibilidad decide cuántos.\nSlice: cada trozo lleva su envolvente entera y suelta con Release al levantar la tecla.\nApagado: la envolvente actúa sobre el loop entero.";
        case GrainBlend:    return "Motores B y D: cuánta parte granular hay al final del fundido. Por debajo del 100 % sigue sonando también el sample original.";
        case EnvLock:       return "Candado cerrado: al cargar otro sample, la envolvente se queda como la tienes. Abierto: toma los valores detectados en el sample nuevo.";
        case EnvMode:       return "Remodelar: tu ADSR cambia la forma del sample y conserva sus altibajos; con los valores detectados suena igual que el original.\nSintetizador: el sample se vuelve un tono continuo y plano, y la ADSR lo modela desde cero, como en un Minimoog o un Prophet. Al elegirlo, los mandos pasan a valores típicos de sinte.\nLa forma de onda dibuja el resultado. Motores A a D.";
        case Punch:         return "Si pones un ataque más rápido que el del sample, crea el golpe que le falta con los agudos del propio sonido. Sólo motor A.";
        case AtAmount:      return "Cuánto responde el sonido al aftertouch (también al poli aftertouch y al soplo, CC2): apretar la tecla lo abre, lo sube de volumen y añade vibrato.";
        case VibDepth:      return "Profundidad del vibrato que hace la rueda de modulación (y el aftertouch). Al máximo, ±50 cents.";
        case VibRate:       return "Velocidad del vibrato.";
        case BendRange:     return "Semitonos que sube o baja la rueda de pitch bend en su extremo.";
        case GrainPos:      return "Motores E y F: punto de la parte que suena del sample (sin el silencio del principio ni del final) del que salen los granos o desde el que lee el drone. La zona GRANULAR o DRONE de la forma de onda lo muestra.";
        case DroneStretch:  return "Motor F: cuántas veces más despacio se lee el sample. A x1 avanza a su velocidad; a x1000 un segundo dura más de un cuarto de hora.";
        case DroneFreeze:   return "Motor F: detiene la lectura. El drone se queda en un solo instante del sonido.";
        case DroneSize:     return "Motor F: duración de cada trozo que analiza el drone. Pequeño conserva algo de movimiento y suena más áspero; grande es más liso y difuminado. Los tiempos son a 48 kHz; a 96 kHz o más el mayor se limita a 0,7 s.";
        case DroneHarm:     return "Motor F: deja sólo los armónicos de la nota del sample, si tiene una nota clara. Convierte ruidos y texturas en un drone afinado.";
        case DroneHarmWidth: return "Motor F: ancho de cada armónico que deja Armónicos, en cents. Estrecho suena puro y cristalino; ancho, más natural.";
        case DroneTonal:    return "Motor F: hacia la derecha deja sólo lo tonal (los picos del espectro); hacia la izquierda, sólo el ruido. En el centro no cambia nada.";
        case DroneSpread:   return "Motor F: ensancha cada pico del espectro. El drone se vuelve más difuso, como un coro o el viento.";
        case DroneSub2:     return "Motor F: añade una copia dos octavas por debajo. El volumen total se mantiene.";
        case DroneSub1:     return "Motor F: añade una copia una octava por debajo. El volumen total se mantiene.";
        case DroneFifth:    return "Motor F: añade una copia una quinta por encima. El volumen total se mantiene.";
        case DroneUp1:      return "Motor F: añade una copia una octava por encima. Con +2 oct da el efecto shimmer.";
        case DroneUp2:      return "Motor F: añade una copia dos octavas por encima. El volumen total se mantiene.";
        case DroneShift:    return "Motor F: desplaza todas las frecuencias los mismos hercios. Rompe la relación entre armónicos: suena metálico e inarmónico.";
        case DroneWidth:    return "Motor F: separación estéreo. A 0 los dos canales comparten fases y suena igual en mono; a 100 % son independientes y muy anchos.";
        case DroneCurveOn:  return "Motor F: enciende el filtro dibujado. Dibuja la curva con el ratón en el panel DRONE MAKER.";
        case GrainDensity:  return "Motor E: granos que nacen por segundo. Pocos suena a partículas sueltas; muchos, a una nube continua.";
        case GrainPitch:    return "Motor E: cada grano se desafina al azar hasta estos semitonos, arriba o abajo. Da brillo y movimiento.";
        case GrainReverse:  return "Motor E: parte de los granos que suenan al revés.";
        case GrainWidth:    return "Motor E: cuánto se reparten los granos por el estéreo.";
        case GrainMix:      return "Mezcla una nube de granos del propio sample en el sonido de los motores A, B, C y D: el toque granular sin cambiar de motor. Sigue a la nota y a tu envolvente.";
        case EndPos:        return "Punto del archivo donde termina el sample. Con el motor A, si tu envolvente pide más, Reanima sigue el sonido desde ahí.";
        case FadeIn:        return "Fundido al empezar la nota, desde el Inicio. Quita clics al empezar a mitad de un sonido.";
        case FadeOut:       return "Fundido antes del Fin. Sin bucle, el sample se apaga suave en vez de cortarse.";
        case LoopOn:        return "Repite la zona del bucle mientras suena la nota, como un sampler clásico. Sólo motor A.";
        case LoopStart:     return "Dónde empieza la zona que se repite. Arrastra la línea verde de la forma de onda.";
        case LoopEnd:       return "Dónde termina la zona que se repite. Arrastra la línea verde de la forma de onda.";
        case LoopXfade:     return "Fundido cruzado entre el final y el principio del bucle, para que el salto no se oiga.";
        case SampleGain:    return "Volumen del sample antes de todo lo demás.";
        case Normalize:     return "Sube o baja el sample para que su pico llegue a 0 dB.";
        case PitchEnvAmt:   return "Cuántos semitonos cambia el tono al empezar la nota antes de volver a su sitio: con valores positivos cae como un 808 o un tom; con negativos, sube.";
        case PitchEnvTime:  return "Tiempo que tarda el tono en volver a su sitio.";
        case MonoOut:       return "Suma los dos canales en uno, después de los efectos. Apagado, el estéreo queda tal cual. Sirve para comprobar cómo suena en mono o para que ocupe el centro.";

        case FltOn:         return "Enciende el filtro de cada voz.";
        case FltType:       return "Tipo de filtro.\nLowpass: deja pasar los graves.\nHighpass: deja pasar los agudos.\nBandpass y Notch: una banda sí o no.\nMorph: pasa por todos.\nDJ: grave a un lado, agudo al otro.\nComb y Vowel: resonancias y vocales.";
        case FltSlope:      return "Lo pronunciado que es el corte: 12 dB suave, 24 dB marcado.";
        case FltCircuit:    return "Carácter del filtro. Clean es limpio; los demás añaden saturación y color analógico.";
        case FltFreq:       return "Frecuencia de corte del filtro.";
        case FltRes:        return "Realce alrededor de la frecuencia de corte. Muy alta llega a silbar.";
        case FltDrive:      return "Saturación a la entrada del filtro. No actúa con el circuito Clean.";
        case FltMorph:      return "En Morph mueve entre tipos de filtro; en DJ, entre grave y agudo; en Vowel, entre vocales; en Comb cambia el signo de la resonancia.";
        case FltKey:        return "Hace que el corte siga la nota tocada. A 100 % sube una octava por octava.";
        case FltEnvAmt:     return "Cuánto mueve Env 2 la frecuencia de corte, hacia arriba o hacia abajo.";
        case FltFollow:     return "Cuánto mueve el corte el propio volumen del sonido.";
        case FltFolAtk:     return "Rapidez con la que el seguidor reacciona cuando el sonido sube.";
        case FltFolRel:     return "Rapidez con la que el seguidor vuelve cuando el sonido baja.";
        case FltMix:        return "Mezcla entre el sonido sin filtrar y el filtrado.";

        case E2A:           return "Ataque de la envolvente 2, que se usa para modular el filtro u otros destinos.";
        case E2D:           return "Decay de la envolvente 2.";
        case E2S:           return "Sustain de la envolvente 2.";
        case E2R:           return "Release de la envolvente 2.";

        case Macro1: case Macro2: case Macro3: case Macro4:
            return "Mando libre. Asígnalo en la matriz para controlar varios destinos a la vez.";

        case FxMainLevel:   return "Nivel de la señal que entra en la cadena Main.";
        case FxBus1Send:    return "Cuánta señal se manda en paralelo al Bus 1.";
        case FxBus2Send:    return "Cuánta señal se manda en paralelo al Bus 2.";
        case FxBus1Level:   return "Nivel de salida del Bus 1.";
        case FxBus2Level:   return "Nivel de salida del Bus 2.";
        case FxBus1ToMain:  return "Pasa el Bus 1 por la cadena Main en lugar de ir directo a la salida.";
        case FxBus2ToMain:  return "Pasa el Bus 2 por la cadena Main en lugar de ir directo a la salida.";
        default:            return "";
    }
}

const char* lfoHelp (int part)
{
    switch (part)
    {
        case 0:  return "Velocidad del LFO en hercios.";
        case 1:  return "Sincroniza el LFO con el tempo del proyecto. La velocidad pasa a marcarla División.";
        case 2:  return "Duración de un ciclo del LFO cuando Sync está encendido.";
        case 3:  return "Forma de la onda del LFO. S&H da un valor aleatorio en cada ciclo.";
        default: return "Encendido, cada nota reinicia el LFO. Apagado, todas las notas siguen un LFO que no se detiene.";
    }
}

const char* matrixHelp (int part)
{
    switch (part)
    {
        case 0:  return "Qué modula: un LFO, una envolvente, la fuerza de la nota, la nota tocada, la rueda de modulación, el aftertouch, una macro o un valor aleatorio por nota.";
        case 1:  return "Qué se modula. Spray, Grano y Transitorio no actúan en el motor A.";
        default: return "Cuánta modulación se aplica y en qué sentido. A 0 no hay ninguna.";
    }
}

const char* suffixFor (int index)
{
    using namespace P;
    switch (index)
    {
        case Stretch: case Flatten:
            return onlyLegacy;
        case SustainMode: case GrainMs: case Spray: case GranSpeed:
            return onlyGrainEngines;
        case TransientMs: case TransientGain:
            return " Sólo motores C y D.";
        case Partials: case NoiseLevel:
            return onlyResynth;
        default:
            return "";
    }
}
} // namespace

namespace Help
{
juce::String forParam (int index)
{
    using namespace P;

    if (index >= FxSlotBase)
    {
        switch ((index - FxSlotBase) % fxSlotStride)
        {
            case 0:  return u8 ("Efecto de esta ranura.");
            case 1:  return u8 ("Enciende o apaga el efecto de esta ranura.");
            case 2:  return u8 ("Mezcla entre el sonido sin efecto y con efecto.");
            default: return {};
        }
    }

    if (index >= Lfo1Rate && index < Macro1)
        return u8 (lfoHelp ((index - Lfo1Rate) % lfoStride));

    if (index >= M1Src && index < FxMainLevel)
        return u8 (matrixHelp ((index - M1Src) % slotStride));

    return u8 (controlHelp (index)) + u8 (suffixFor (index));
}

juce::String forFxType (int type)
{
    switch (type)
    {
        case Fx::HyperDim:   return P::u8 ("Hyper añade voces desafinadas y Dimension abre el estéreo. Da anchura y cuerpo.");
        case Fx::Distortion: return P::u8 ("Satura o rompe el sonido, con 16 modos. Trabaja con sobremuestreo para no ensuciar los agudos.");
        case Fx::Flanger:    return P::u8 ("Barrido metálico de un retardo muy corto.");
        case Fx::Phaser:     return P::u8 ("Barrido suave de muescas en el espectro.");
        case Fx::Chorus:     return P::u8 ("Dobla el sonido con pequeñas variaciones de tono. Da anchura y movimiento.");
        case Fx::Delay:      return P::u8 ("Repeticiones, libres o al tempo del proyecto.");
        case Fx::Compressor: return P::u8 ("Iguala el volumen. En multibanda trabaja por separado graves, medios y agudos.");
        case Fx::Reverb:     return P::u8 ("Espacio y cola, con cinco tipos de sala.");
        case Fx::EQ:         return P::u8 ("Dos bandas para corregir o dar color al sonido.");
        case Fx::Filter:     return P::u8 ("El filtro del plugin, aplicado a toda la mezcla en lugar de a cada voz.");
        case Fx::Bode:       return P::u8 ("Desplaza todas las frecuencias la misma cantidad de hercios. Crea sonidos metálicos e inarmónicos.");
        case Fx::Utility:    return P::u8 ("Ganancia, pan, anchura estéreo, graves en mono e inversión de polaridad.");
        case Fx::SplitLH:    return P::u8 ("Divide el sonido en graves y agudos. Los efectos siguientes sólo procesan las bandas marcadas.");
        case Fx::SplitLMH:   return P::u8 ("Divide el sonido en graves, medios y agudos. Los efectos siguientes sólo procesan las bandas marcadas.");
        case Fx::SplitMS:    return P::u8 ("Divide el sonido en centro y lados. Los efectos siguientes sólo procesan lo marcado.");
        default:             return P::u8 ("Elige un efecto para esta ranura.");
    }
}

juce::String forFxParam (int type, int k)
{
    static const std::vector<std::vector<const char*>> text = []
    {
        std::vector<std::vector<const char*>> t ((size_t) Fx::NumTypes);

        t[Fx::HyperDim] = { "Velocidad del movimiento de las voces.", "Cuánto se desafinan las voces.", "Número de voces de Hyper.",
                            "Nivel de las voces Hyper.", "Tamaño del espacio de Dimension.", "Nivel de Dimension. No cambia la suma en mono." };

        t[Fx::Distortion] = { "Tipo de distorsión. Cada modo tiene un carácter distinto.", "Cantidad de distorsión.",
                              "Desplaza la señal antes de distorsionar, para un carácter asimétrico.",
                              "Filtro antes o después de la distorsión, o apagado.", "Tipo del filtro: grave, banda o agudo.",
                              "Frecuencia del filtro.", "Resonancia del filtro." };

        t[Fx::Flanger] = { "Velocidad del barrido.", "Amplitud del barrido.", "Realimentación: más metálico y resonante. Negativa cambia el color.",
                           "Desfase del barrido entre izquierda y derecha." };

        t[Fx::Phaser] = { "Velocidad del barrido.", "Amplitud del barrido.", "Frecuencia central del barrido.",
                          "Realimentación: más marcado y resonante.", "Desfase del barrido entre izquierda y derecha.",
                          "Número de etapas. Más etapas, más muescas." };

        t[Fx::Chorus] = { "Velocidad de la modulación.", "Retardo de la primera voz.", "Retardo de la segunda voz.",
                          "Cuánto varía el tono de las voces.", "Realimentación de las voces.", "Quita agudos del sonido procesado." };

        t[Fx::Delay] = { "Sigue el tempo del proyecto.", "Tiempo del canal izquierdo sin Sync.", "Tiempo del canal derecho sin Sync y sin Link.",
                         "Tiempo del canal izquierdo con Sync.", "Tiempo del canal derecho con Sync y sin Link.",
                         "Usa el tiempo izquierdo en los dos canales.", "Cuántas repeticiones suenan.",
                         "Normal: cada canal repite por su lado. Ping-Pong: las repeticiones saltan de lado. Tap→Delay: añade un retardo previo.",
                         "Frecuencia central del filtro de las repeticiones.", "Ancho de ese filtro en octavas. Más ancho, más limpio." };

        t[Fx::Compressor] = { "Nivel a partir del cual empieza a comprimir.", "Cuánto reduce lo que supera el Threshold.",
                              "Rapidez con la que empieza a comprimir.", "Rapidez con la que deja de comprimir.", "Sube el nivel de salida.",
                              "Comprime por separado graves, medios y agudos.", "En multibanda, cuánto sube lo que queda por debajo del Threshold." };

        t[Fx::Reverb] = { "Carácter de la sala.", "Tamaño del espacio.", "Tiempo que tarda en apagarse la cola.",
                          "Tiempo antes de que empiece la reverb.", "Cuánto se apagan los agudos en la cola.",
                          "Quita graves de la reverb.", "Quita agudos de la reverb.", "Anchura estéreo de la reverb." };

        t[Fx::EQ] = { "Forma de la banda grave.", "Frecuencia de la banda grave.", "Ganancia de la banda grave.", "Anchura de la banda grave.",
                      "Forma de la banda aguda.", "Frecuencia de la banda aguda.", "Ganancia de la banda aguda.", "Anchura de la banda aguda." };

        t[Fx::Filter] = { "Tipo de filtro.", "Lo pronunciado que es el corte.", "Carácter del filtro. Clean es limpio.", "Frecuencia de corte.",
                          "Realce alrededor del corte.", "Saturación a la entrada. No actúa en Clean.", "Mueve entre tipos, entre grave y agudo, o entre vocales." };

        t[Fx::Bode] = { "Cuántos hercios se desplaza el sonido, hacia arriba o hacia abajo.",
                        "Desplaza izquierda y derecha en sentidos distintos.", "Vuelve a desplazar lo ya desplazado, creando una escalera." };

        t[Fx::Utility] = { "Ganancia.", "Posición en el estéreo.", "Anchura estéreo. 0 es mono, 1 lo deja igual y 2 es el doble de ancho.",
                           "Por debajo de esta frecuencia el sonido queda en mono. A 0 no actúa.",
                           "Invierte la polaridad del canal izquierdo.", "Invierte la polaridad del canal derecho." };

        t[Fx::SplitLH] = { "Frecuencia que separa graves y agudos.", "Nivel de los graves.", "Nivel de los agudos.",
                           "Los efectos siguientes procesan los graves.", "Los efectos siguientes procesan los agudos." };

        t[Fx::SplitLMH] = { "Frecuencia que separa graves y medios.", "Frecuencia que separa medios y agudos.", "Nivel de los graves.",
                            "Nivel de los medios.", "Nivel de los agudos.", "Los efectos siguientes procesan los graves.",
                            "Los efectos siguientes procesan los medios.", "Los efectos siguientes procesan los agudos." };

        t[Fx::SplitMS] = { "Nivel del centro.", "Nivel de los lados.", "Los efectos siguientes procesan el centro.", "Los efectos siguientes procesan los lados." };

        return t;
    }();

    if (type < 0 || type >= (int) text.size() || k < 0 || k >= (int) text[(size_t) type].size())
        return {};
    return P::u8 (text[(size_t) type][(size_t) k]);
}
} // namespace Help
