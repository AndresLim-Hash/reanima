# Reanima 0.9.1 · Diagnóstico profesional (fase 1)

Fecha: 13-11-2026. Alcance: todo el plugin (código, motores, interfaz, estabilidad, distribución), comparado con los
samplers profesionales. Esta fase no cambia el plugin: describe, mide, compara y propone.

**Cómo leer este documento.** Marco como **[medido]** lo que sale de pruebas sobre el binario actual, **[código]** lo
comprobado leyendo la implementación, **[fuente]** lo que viene de documentación externa (con enlace) y
**[criterio]** las conclusiones técnicas. Yo no puedo oír: toda afirmación sobre cómo *suena* algo es una medida o
una hipótesis que tu oído tiene que confirmar.

---

## 0. Resumen en una página

**Qué es Reanima hoy.** Un instrumento de *un solo sample*: cargas un sonido y lo convierte en un instrumento
tocable. Lo analiza (nota, envolvente, ataque, región sostenible, modelo de parciales, perfil de excitación) y lo
toca con seis motores que van desde «el archivo tal cual» hasta un drone estirado mil veces. Encima: filtro,
modulación, efectos y un sistema de articulaciones grabadas (Mecanismo).

**Dónde está por delante** [criterio]: convertir *cualquier* one-shot en un instrumento sostenible y expresivo sin
preparar nada. En Kontakt, HALion o Falcon eso exige preparar bucles, crossfades y zonas a mano, o librerías hechas
por terceros. Tiene análisis automático de ADSR, sostén sin punto de bucle (lechos reconstruidos), resíntesis, drone,
y gestos de instrumento que se adaptan al sample. A eso se suma una disciplina de pruebas poco habitual en un
producto de este tamaño: más de 100 pruebas del motor, 315 de auditoría, validadores oficiales, AddressSanitizer y
sesiones de resistencia.

**Dónde está por detrás** [criterio + fuente]:
1. **Un solo sample por instrumento.** No hay zonas, capas de velocidad, *round robin* ni mapeo de teclado. Es la
   carencia estructural más grande frente a cualquier sampler profesional, incluido el Sampler de Ableton.
2. **El cambio de tono es solo de lectura** (más rápido = más agudo). Lejos de la nota original suena «chipmunk» o
   apagado. No hay preservación de formantes, que HALion (AudioWarp) o Serato (Pitch 'n Time) sí ofrecen.
3. **Sin tempo ni bucles musicales.** Muchos samples de tu carpeta son loops con tempo. No hay sincronía con el DAW,
   ni troceado (*slicing*) ni detección de tempo, que en Serato Sample son la base.
4. **Expresión moderna.** No hay MPE ni aftertouch polifónico; HALion 7.5 y Omnisphere 3 ya lo tienen.
5. **Memoria.** Unos 12 MB por sample analizado más 29 MB de la librería de golpes. Una instancia ronda los 45-60 MB,
   y en una sesión larga el proceso se estabiliza en unos 350 MB.
6. **Solo Windows y solo en español.** Sin firma de código ni actualizador.

**Lo que propongo** [criterio]:
- **No convertirlo en un Kontakt.** Ese camino ya lo recorren otros productos con 20 años de ventaja.
- **Convertirlo en el mejor «sample-a-instrumento» del mercado.** Para eso:
  1. reforzar la calidad del tono lejos de la nota original (formantes y multimuestra *automática* a partir del
     mismo sample);
  2. añadir un modo loop/tempo para el material rítmico;
  3. añadir expresión MPE;
  4. reducir memoria;
  5. pulir la experiencia (navegador de presets, A/B con nivel igualado, idioma).

  La hoja de ruta (sección 10) ordena todo esto por fases que se pueden medir.

---

## 1. Inventario de funciones

### 1.1 Arquitectura [código]
- JUCE 9.0.4, C++20, CMake + Ninja, MSVC. Unas 15 000 líneas propias.
- Módulos y responsabilidad:
  - `SampleAnalysis`: carga y análisis.
  - `Reconstruct`: lechos del motor A.
  - `SynthEngine`: voces, los 6 motores y MIDI.
  - `DroneStretch`: motor F.
  - `Mechanism`: articulaciones.
  - `FxRack`: 15 módulos de efectos y 3 divisores.
  - `Filters`: filtro de voz.
  - `PluginProcessor`: estado, historial, edición y carga en segundo plano.
  - Interfaz: `PluginEditor`, `WaveformView`, `EditPage`, `FxPage`, `Displays`, `Guide`, `Look`.
- **Separación de hilos:**
  - el análisis va en un hilo de baja prioridad;
  - el audio no reserva memoria (auditado: «0 reservas tocando»);
  - el cambio de sample usa un intercambio con *try-lock*;
  - el sample viejo se libera fuera del audio.
- **Herramientas de prueba propias:** EngineTests, Audit, Snapshot (interfaz), HostCheck (carga VST3 y resistencia),
  Batch (barridos por carpeta), Demos y Release.

### 1.2 Muestra y análisis [código + medido]
| Función | Qué hace | Estado |
|---|---|---|
| Carga | WAV, AIFF, FLAC, MP3, OGG; máx. 2 canales y **2 minutos** | Correcta; el límite de 2 min y de 2 canales es una limitación |
| Detección de nota | YIN + refinado; «Snap a C» | 352/448 aciertos en notas sueltas con nombre fiable (78 %); falla en graves y en ecos [medido] |
| ADSR detectado | Ataque por llegada a 1 dB, decay, sustain, caída, release | Bueno; probado sobre 2 024 samples |
| Perfil de excitación | Golpe, sostén, ruido/tono, color, registro, polaridad | Nuevo en 0.9; alimenta Mecanismo |
| Modelo de parciales | Senos + ruido en 24 bandas, hasta 128 parciales | Base del motor C |
| Lechos (motor A) | Reconstrucción de 8 s sin costura, brillante y oscuro coherentes | Bueno tras la 0.8.2 |
| Fuentes de sostén (B/D) | Loop ecualizado, granular y *freeze* aditivo | Correcto; picos sobre 0 dBFS al pedir 70 % de sostén en golpes secos (conocido) |
| Sample dentro del proyecto | FLAC 24 bit en base64, codificado en segundo plano | Correcto |
| Edición destructiva | Recortar, borrar, silencio, cortar/pegar, invertir, fundidos, normalizar, ganancia, bucle sin costura | Correcto; 100 pasos de deshacer |

### 1.3 Motores [código + medido]
| Motor | Algoritmo | Fuerte | Débil |
|---|---|---|---|
| A Reconstrucción | El archivo leído con *sinc* (24-96 taps, 512 fases, 13 cortes), con relevo hacia los lechos | Fiel al original; sostén infinito sin bucles | El tono se cambia solo por velocidad de lectura |
| B Reenvolvente | Granular sincronizado al periodo + fuente de sostén | Estira el ataque y el decay | Granos audibles en material ruidoso [criterio] |
| C Resíntesis | Aditivo (hasta 128 parciales) + ruido por bandas | Notas infinitas, timbre maleable | Sin preservación de formantes; transitorios aproximados |
| D Híbrido | Golpe original + cuerpo estirado | El preferido de tu oído (aprobado en la 0.4) | Mismos límites de tono que A |
| E Granular | Nube con Posición, Deriva, Spray, Tono, Ancho | Texturas | Sin sincronía a tempo; sin congelado por gesto |
| F Drone Maker | FFT de fase aleatoria (estilo PaulStretch) en 5 etapas, planificación por plazo | Muy eficiente (1,5 ms de peor bloque) | Latencia propia de varios bloques antes del primer cuadro |

### 1.4 Voz, envolventes, modulación [código]
- **Voces:** 32 de polifonía y 8 de reserva para fundidos; Unison de hasta 16 voces con Detune, Blend y Ancho; Mono,
  Legato y Glide; Variación.
- **Envolventes:** ADSR principal en dos modos (Remodelar y Sintetizador), Golpe, candado y «Detectado»; Env 2.
- **Modulación:** 3 LFO (forma, división, sincronía, retrigger), 4 macros y matriz de 8 ranuras con 15 fuentes y
  16 destinos.
- **Expresión MIDI:** velocidad, CC1, CC2 (soplo), CC11, aftertouch de canal y polifónico (lectura), pitch bend
  con rango y pedal CC64. **Sin MPE.**

### 1.5 Filtro y efectos [código]
- **Filtro de voz** estilo Auto Filter:
  - tipos LP, HP, BP, Notch, Morph, DJ, Comb y Vowel;
  - circuitos Clean, OSR, MS2 (SVF), SMP y PRD (escalera con saturación);
  - Drive, Keytrack, Env 2, seguidor de envolvente y Dry/Wet.
- **Rack de efectos** al estilo Serum: cadena Main de 6 ranuras y 2 buses de 4.
  - Módulos: Hyper/Dimension, Distorsión (con sobremuestreo), Flanger, Phaser, Chorus, Delay, Compresor, Reverb
    (FDN de 8 líneas, 5 tipos), EQ, Filtro, Bode y Utility.
  - Divisores: L/H, L/M/H y M/S.

### 1.6 Mecanismo (articulaciones) [código + medido]
- **Gestos y material:** 461 golpes CC0 para Piano (con pedal), Púa, Arco, Tecla, Soplo y Del sample.
- **Encaje y recomendación:** porcentaje de encaje con el sample y recomendación en el menú.
- **Integración con el sample:**
  - alineación del golpe a menos de 2 ms;
  - polaridad;
  - color del ataque;
  - corte por registro;
  - resonancia en la nota;
  - roce y aire continuos.
- **Mandos:** Entrada, Salida, Adaptar y On/Off.
- **Coste:** 11 % de un núcleo para 32 voces con soplo continuo.

### 1.7 Proyecto, interfaz y distribución [código + medido]
- **Estado:** sample incrustado, versión de estado 4 con migraciones y migración comprobada.
- **Historial:** Deshacer/Rehacer global por gestos, 100 pasos.
- **Presets:** 12 plantillas de fábrica y presets de usuario.
- **Interfaz:** 5 pestañas, escala adaptada a monitores 4K, guía de 12 pasos, ayudas emergentes en todos los mandos
  (auditado), arrastrar para exportar el WAV y medidor de salida.
- **Distribución:** VST3 y aplicación independiente para Windows, instalador .bat con copia de seguridad y vuelta
  atrás. **Sin firma de código, sin actualizador, sin macOS compilado, sin AU.**
- **Rendimiento** [medido, 48 kHz, un núcleo]:
  - A con 32 voces: 11,5 %;
  - D con 16 voces × 4 de unison: 31 %;
  - C con 16 voces: 16 %;
  - caso extremo de 32 voces × 8 de unison: 110 %;
  - latencia declarada: 0.
- **Memoria** [medido]: entre 9,9 y 18,9 MB por sample analizado (unos 10 MB fijos de lechos y fuentes), más 29 MB
  de la librería de golpes (compartida entre instancias). En una sesión de 400 minutos, el proceso de prueba se
  estabiliza en unos 350 MB.

---

## 2. Estado técnico y funcional por módulo

| Módulo | Estado | Pruebas que lo cubren | Comentario |
|---|---|---|---|
| Análisis | Sólido | EngineTests, Batch sobre 2 024 samples | La nota acierta un 78 %; graves y ecos son el punto débil |
| Motor A | Muy sólido | SNR 71,8 dB contra el original; prueba de «nunca vuelve a subir» | Es la referencia de fidelidad |
| Motor B | Correcto | Sostén, nivel | Picos sobre 0 dBFS con sostén alto en golpes secos |
| Motor C | Correcto | Nivel ±0,02 dB, afinación | El timbre cambia lejos de la nota (sin formantes) |
| Motor D | Muy sólido | SNR 73 dB intacto | Aprobado por tu oído |
| Motor E | Correcto | Nivel compensado +12 dB | Sin tempo |
| Motor F | Sólido | 1,5 ms de peor bloque, nivel, octavas | — |
| Filtro | Correcto | Estabilidad por circuito | Sin sobremuestreo en la escalera: posible aliasing con mucho Drive en agudos [hipótesis por medir] |
| Efectos | Correcto | 15 módulos sin explosiones ni DC | Reverb FDN propia: calidad por juzgar a oído frente a una reverb de convolución |
| Mecanismo | Nuevo, medido | 9 pruebas propias, A/B en 11 familias | Falta tu valoración a oído de la 0.9.1 |
| Estado e historial | Sólido | Migraciones, restauración por paso | El historial guarda el audio de cada edición (con samples largos, mucha memoria) |
| Interfaz | Sólida | Captura de cada pantalla y comprobación de solapes | Solo español; nombres cortados en algún motor |
| Estabilidad | Muy sólida | Steinberg 537/537, pluginval nivel 10, AddressSanitizer limpio, 400 min de resistencia | El peor bloque fue del 36 % |

---

## 3. Comparativa con productos profesionales

Versiones comprobadas en noviembre de 2026 [fuente]:
- [Kontakt 8](https://community.native-instruments.com/discussion/39/official-update-status-kontakt-current-version-8-1-0):
  8.12.1, de agosto de 2026.
- [Falcon 2027](https://sonicstate.com/news/2026/10/12/uvi-releases-falcon-2027/): octubre de 2026.
- [HALion 7.5](https://synthanatomy.com/2026/08/steinberg-halion-7-flagship-sampler-plugin-gets-an-fm-and-spectral-makeover.html):
  agosto de 2026, con MPE.
- [Omnisphere 3](https://djlifemag.com/2026/03/the-o3-spectrasonics-drops-omnisphere-version-3/): octubre de 2025.
- [Serato Sample 2](https://www.musicradar.com/reviews/serato-sample-2-review): 2023, sin novedades encontradas en
  2026.
- [TAL-Sampler](https://www.synthtopia.com/content/2015/06/29/tal-sampler-an-analog-modeling-synth-with-sampling-sound-engine/):
  sin novedades encontradas en 2026.

| Aspecto | Reanima | Referencias | Posición |
|---|---|---|---|
| De un sample a un instrumento sostenible | Automático: lechos, resíntesis, drone | Kontakt/HALion: bucles y crossfades manuales; Omnisphere: importa audio pero [sin zonas ni capas](https://support.spectrasonics.net/manual/Omnisphere2/user_audio/page02.html) | **Por delante** |
| Multimuestra, capas de velocidad, *round robin* | No | Kontakt, HALion, Falcon, [Logic Sampler](https://www.audeobox.com/learn/logic-pro/sampler-quick-sampler-tutorial/) y Ableton Sampler | **Muy por detrás** |
| Calidad de interpolación | Sinc con ventana de Kaiser, 24-96 taps, corte adaptado al cambio de tono | Kontakt y HALion ofrecen modos de calidad seleccionables | **A la altura** [criterio] |
| Cambio de tono con formantes | No | [HALion AudioWarp](https://steinberg.help/halion/v6/en/halion/topics/editing_zones/sound_editor_audiowarp_section_r.html) (Solo con formantes), Serato Pitch 'n Time | **Por detrás** |
| Estiramiento de tiempo | B/D granular por periodos, F tipo PaulStretch | HALion AudioWarp con sincronía al tempo, Serato | Bueno para sostener; **sin tempo** |
| Detección de ataques | Precisa y por periodos (2 024 samples) | Serato: detección de tonalidad y tempo | **A la altura** en ataques; **por detrás** en tempo |
| Troceado (*slicing*) | No | Serato (estilo MPC), Kontakt | **Por detrás** |
| Separación de pistas | No | [Serato Sample 2](https://www.musicradar.com/news/serato-sample-2-stem-separation) (4 pistas en tiempo real) | Fuera de objetivo [criterio] |
| Articulaciones y gestos | Mecanismo adaptativo con porcentaje de encaje | Librerías de Kontakt con keyswitches y ruidos de soltar grabados; SWAM, modelado | **Diferencial propio**, pero sin keyswitches |
| Modulación | 3 LFO, Env 2, 4 macros, matriz de 8 | Falcon/HALion: modulación ilimitada y scripting | Suficiente; por detrás en profundidad |
| MPE y aftertouch polifónico | No | HALion 7.5, Omnisphere 3, Kontakt 8.10 (herramienta Aftertouch) | **Por detrás** |
| Color «vintage» | Bits y Muestreo | [TAL-Sampler](https://www.kvraudio.com/forum/viewtopic.php?p=9150487): emulación de convertidores (Emu II, S1000…) con filtro de reconstrucción | Por detrás en carácter |
| Efectos | 15 módulos y 2 buses | Omnisphere/Falcon: más de 50 efectos de alta calidad | Correcto; reverb por validar |
| CPU | Bajo (A: 11 % con 32 voces) | Kontakt y HALion también eficientes, con *disk streaming* | A la altura en un sample; no hay *streaming* (no hace falta con 2 min) |
| Memoria | Unos 12 MB por sample + 29 MB compartidos | Kontakt precarga solo el inicio del sample (DFD) | Mejorable |
| Presets y navegación | 12 plantillas + presets de usuario | Navegadores con etiquetas, búsqueda y favoritos | **Por detrás** |
| Pruebas y estabilidad | Excepcionales para un producto así | No publicadas | **Por delante** (proceso) |
| Plataformas | Windows VST3 y standalone | Mac/Win, AU/VST3/AAX | **Por detrás** |

---

## 4. Puntos fuertes y débiles

**Fuertes** [criterio, sobre medidas]:
- convierte un one-shot en instrumento sin preparación;
- fidelidad del motor A y del D;
- sostén infinito sin costuras;
- drone muy eficiente;
- articulaciones que se adaptan al sample (no conozco otro sampler que lo haga);
- sample dentro del proyecto;
- Deshacer global;
- validación y estabilidad;
- bajo consumo de CPU.

**Débiles:**
- un solo sample por instrumento;
- el tono lejos de la nota original suena «chipmunk»;
- el loop rítmico no tiene sentido musical (no hay tempo);
- sin MPE;
- memoria fija por sample;
- detección de nota en graves;
- navegación de presets mínima;
- plataformas e idioma;
- falta validación a oído de C, E, F y Mecanismo 0.9.

---

## 5. Matriz de decisiones

| Función | Decisión | Por qué |
|---|---|---|
| Motor A, Motor D | **MANTENER** | Fieles y medidos; tu oído aprobó D |
| Interpolación sinc | **MANTENER** | Ya es de nivel profesional |
| Lechos y relevo de la cola | **MANTENER** | Corregidos y medidos en la 0.8.2 |
| Motor F | **MANTENER** | Eficiente y estable |
| Mecanismo (Piano, Tecla) | **MANTENER** | Son tus preferidos |
| Mecanismo (Púa, Arco, Soplo) | **INVESTIGAR** | Medidos, pero sin tu juicio a oído en la 0.9.1 |
| Cambio de tono (todos los motores) | **MEJORAR** | Formantes y «multimuestra virtual» (ver 7.1) |
| Detección de nota | **MEJORAR** | 78 %: falla en graves y ecos (ver 7.6) |
| Motor B | **MEJORAR** | Picos con sostén alto en golpes secos |
| Filtro escalera con Drive | **INVESTIGAR** | Medir aliasing; sobremuestrear solo si se demuestra |
| Reverb FDN | **INVESTIGAR** | A/B con nivel igualado frente a referencias |
| Multimuestra (zonas, capas, *round robin*) | **IMPLEMENTAR** (fase alta) | Mayor carencia frente a cualquier sampler |
| Modo loop/tempo con troceado | **IMPLEMENTAR** (fase alta) | Tu carpeta tiene muchos loops |
| MPE y aftertouch polifónico | **IMPLEMENTAR** (media) | Estándar en 2026 |
| A/B con nivel igualado | **IMPLEMENTAR** (crítica, barata) | Para juzgar sin el engaño del volumen |
| Navegador de presets con etiquetas | **IMPLEMENTAR** (media) | Flujo de trabajo |
| Bits y Muestreo | **SUSTITUIR** (media) | Por una emulación de convertidor con filtro de reconstrucción, que suena más auténtica |
| Mandos de motor que se repiten entre motores | **SIMPLIFICAR** | Revisar Respiración, Variación y Textura por motor |
| Separación de pistas por IA | **ELIMINAR** de los planes | Mucho coste y modelos externos, poco encaje con un one-shot |
| Límite de 2 min y 2 canales | **MEJORAR** | Subir a 10 min sin que crezca la memoria (requiere 7.4) |
| Memoria por sample | **MEJORAR** | Lechos a demanda por motor y fuentes compartidas |
| macOS, AU, firma, actualizador | **IMPLEMENTAR** (cuando lo decidas) | Ya está preparado en CMake |
| Interfaz en inglés | **INVESTIGAR** | Según a quién va dirigido |

---

## 6. Tecnologías que merece la pena investigar

| # | Tecnología | Problema que resuelve | Integración | Coste | CPU, RAM, latencia | Cómo validarlo |
|---|---|---|---|---|---|---|
| T1 | **Envolvente espectral cepstral** ([Röbel y Rodet 2005](https://www.mathworks.com/help/audio/ref/shiftpitch.html)) para conservar formantes | Tono «chipmunk» lejos de la nota | Ya hay modelo aditivo (C) y lechos espectrales: se aplica la envolvente al reconstruir; en A/D, filtro de corrección por nota precalculado | Medio | C: casi nada; A/D: un filtro corto por voz | Distancia espectral de formantes antes/después; A/B a ±7 y ±12 st |
| T2 | **[Signalsmith Stretch](https://signalsmith-audio.co.uk/code/stretch)** (MIT, C++ en cabeceras) | Estiramiento y cambio de tono de calidad para el modo loop/tempo | Nueva fuente de lectura para un motor «Loop» | Medio | Moderado; latencia de unos 50-100 ms en modo tiempo real (asumible en loops con precálculo) | Batería de loops, artefactos y transitorios |
| T3 | **[Rubber Band](https://breakfastquay.com/rubberband/license.html)** | Lo mismo | GPL o licencia comercial | Licencia | Similar | — Lo descarto frente a T2 por la licencia |
| T4 | **Multimuestra virtual** (propio) | Tono lejos de la nota con un solo sample | Pre-renderizar el sample a -12/-7/+7/+12 st con T1/T2 en segundo plano y elegir el más cercano por nota (como las zonas de un multisample) | Medio | +RAM (×3-5 por sample, mejor a 16 bit) y cero CPU extra al tocar | A/B frente a la lectura directa, a nivel igualado |
| T5 | **Detección de tempo y onsets** (flujo espectral, SuperFlux) | Loops y troceado | Análisis en segundo plano, como el actual | Bajo-medio | Solo al cargar | Contra tempos conocidos en los nombres de tu carpeta |
| T6 | **Emulación de convertidores** (tipo [TAL-DAC](https://www.toolfarm.com/buy/tal-dac): sample & hold + filtro de reconstrucción) | Lo-fi más auténtico que Bits y Muestreo | Sustituye a Bits y Muestreo en la voz | Bajo | Mínimo | A/B frente a referencias de S950/SP-1200 que tengas |
| T7 | **Sobremuestreo selectivo del filtro escalera** | Posible aliasing con Drive alto | Solo si se mide el problema | Bajo | ×2 en el filtro cuando el Drive pase de un umbral | Aliasing con un barrido de seno a 8-16 kHz |
| T8 | **NMF y HPSS en el análisis** | Separar golpe y cuerpo para el motor D y el Mecanismo | Ya se usa HPSS en la extracción | Medio | Solo al cargar | Golpe y cuerpo separados frente a la referencia |
| T9 | **DDSP y redes neuronales** | Timbre y resíntesis | Modelos externos y dependencias | Alto | Alto | **Descartado** por ahora: una solución convencional (T1 + T4) cubre lo mismo con menos riesgo |

---

## 7. Propuestas concretas (las importantes)

**7.1 Tono natural lejos de la nota (T1 + T4).**
- **Problema:** lejos de la nota original el sonido cambia de carácter (formantes desplazados y tiempos acelerados).
- **Solución:** una «multimuestra virtual» renderizada en segundo plano, con formantes conservados, de la que cada
  nota toma la versión más cercana.
- **Alternativas:** formantes en tiempo real (más CPU) o nada (lo actual).
- **Afecta a:** `SampleAnalysis` (renders), `SynthEngine` (elección de fuente) y memoria.
- **Riesgos:** más memoria; transiciones entre zonas.
- **Validación:** distancia espectral a nivel igualado y tu oído a ±12 st.
- **Dificultad:** media-alta.
- **Beneficio:** muy alto [criterio].

**7.2 Multimuestra real.**
- **Problema:** con un solo sample no se puede construir un piano o una guitarra de verdad.
- **Solución:** zonas por nota y velocidad, *round robin* y crossfades. Los motores seguirían funcionando por zona.
- **Cambio de arquitectura:** `SampleData` pasa a ser un `SampleSet`. Afecta a la carga, el análisis, el estado, la
  interfaz (editor de mapa) y el Mecanismo (que ya elige por nota).
- **Riesgos:** alto (toca casi todo); proyectos antiguos (con migración).
- **Validación:** importar un multisample real y medir continuidad de nivel y timbre entre zonas.
- **Dificultad:** alta.
- **Beneficio:** alto, si quieres tocar instrumentos acústicos completos.

**7.3 Modo Loop/Tempo.**
- **Problema:** los loops no siguen el tempo del proyecto.
- **Solución:** detección de tempo y onsets (T5), estiramiento (T2), sincronía con el DAW y troceado a teclas.
- **Afecta a:** un motor nuevo «G Loop», análisis e interfaz.
- **Dificultad:** media.
- **Validación:** tempos de los nombres de archivo de tu carpeta y estabilidad de fase con el DAW.

**7.4 Memoria.**
- **Problema:** unos 10 MB fijos por sample más 29 MB compartidos.
- **Solución:**
  - construir las fuentes de B/D y los lechos solo del motor que se usa (en parte ya existe: `forAllEngines`);
  - lechos de longitud proporcional y no siempre de 8 s;
  - golpes del Mecanismo en 16 bit en memoria (de 29 a 15 MB).
- **Riesgo:** pequeños retrasos al cambiar de motor.
- **Validación:** memoria por instancia con 10 instancias y la prueba de resistencia.

**7.5 A/B a nivel igualado (crítica y barata).**
- **Problema:** al comparar ajustes, el más fuerte parece mejor.
- **Solución:** botón que iguala la sonoridad (LUFS a corto plazo) entre el estado guardado y el actual.
- **Dificultad:** baja.

**7.6 Detección de nota en graves.**
- **Problema:** 78 % de aciertos en notas sueltas; los fallos se concentran en bombos, graves y ecos.
- **Solución:** combinar YIN con un estimador espectral (suma armónica) y el modelo de parciales, que ya existe.
- **Validación:** la misma batería de 448 notas.

---

## 8. Material que me puedes aportar

**Imprescindible**
1. **Tu escucha de la 0.9.1:** Púa, Arco y Soplo; ¿suenan integrados?; ¿el nuevo volumen te va bien? Sin tu oído,
   las medidas no bastan.
2. **De 3 a 5 samples que hoy suenen mal lejos de su nota** (voz, piano, cuerda), con la nota a la que los tocas.
   Serán la batería de T1/T4.
3. **Si quieres multimuestra (7.2): un multisample real** de algo que te importe (por ejemplo, un piano con varias
   velocidades). Si no tienes ninguno, buscaré uno CC0 y te pediré permiso para descargarlo.

**Útil**
4. **De 3 a 5 loops con tempo conocido** de tu uso habitual (si los de tu carpeta no son representativos).
5. **Uno o dos plugins de referencia** cuyo resultado te guste para cada objetivo, con un bounce de audio de la misma
   nota o loop en ambos plugins a nivel igualado.
6. **Tu equipo:** procesador, RAM e interfaz de audio, y el tamaño de búfer que usas en Ableton, para fijar los
   límites de CPU y latencia.

**Opcional**
7. Un vídeo corto de cualquier comportamiento raro que veas al tocar.
8. Referencias de samplers vintage (S950, SP-1200, MPC60) si quieres T6.

---

## 9. Preguntas pendientes

**Imprescindibles** (cambian el plan)
1. **Posicionamiento:** ¿Reanima debe seguir siendo un instrumento de *un sample* (el mejor convirtiendo un sonido en
   instrumento) o quieres que también sea un sampler multimuestra completo? *Es la decisión estructural más grande:
   la 7.2 toca casi todo el código.*
2. **Uso:** ¿para qué lo usas sobre todo? ¿Melodías y acordes con one-shots tonales, bajos y 808, pads y texturas,
   loops rítmicos, recreación de instrumentos acústicos? *Ordena las prioridades de motores y funciones.*
3. **El tono lejos de la nota:** ¿te molesta hoy al tocar uno o dos octavas lejos del sample? *Decide si la 7.1 va
   primero.*
4. **Distribución:** ¿solo para ti o para vender o compartir? *Cambia la importancia de macOS, firma, actualizador,
   idioma y protección.*

**Importantes**
5. **Loops:** ¿quieres que sigan el tempo del DAW y trocearlos a teclas? ¿O usas los loops de tu carpeta solo como
   material para estirar?
6. **MPE:** ¿tienes o piensas tener un controlador MPE (ROLI, Linnstrument, Osmose…)?
7. **Funciones que no usas:** ¿hay mandos o motores que nunca tocas o que te confunden? Candidatos a simplificar:
   Respiración, Variación, Textura, divisores de efectos y Bode.
8. **Sonido de referencia:** ¿qué producto te suena como objetivo para cada uso (por ejemplo, Keyscape para piano,
   Omnisphere para pads)?

**Opcionales**
9. Límite de recursos: ¿cuántas instancias sueles abrir a la vez y con qué búfer?
10. ¿Te interesa el carácter vintage (emulación de convertidores) o prefieres siempre limpio?

---

## Decisiones tomadas

- **13-11-2026 · Posicionamiento:** Reanima sigue siendo un instrumento de *un sample* y busca ser el mejor en eso
  («Un sample, el mejor»). La multimuestra real (7.2, opción A de la fase 2) queda fuera; las fuerzas van a la
  calidad con un solo sample: tono natural lejos de la nota (7.1), detección de nota (7.6), memoria (7.4) y, si
  aplica, el modo Loop/Tempo (7.3).

- **13-11-2026 · Uso:** todo: melodías y acordes, bajos y 808, pads y texturas, loops rítmicos.
- **13-11-2026 · Tono lejos de la nota:** prioritario, y como una opción **Warp** al estilo del Simpler de Ableton
  ([modos de warp de Live](https://www.ableton.com/en/manual/audio-clips-tempo-and-warping/)): el tono deja de
  depender de la velocidad. Esto une 7.1 y 7.3 en un mismo sistema: el mismo motor de estiramiento (Signalsmith
  Stretch, MIT) da notas sin «chipmunk» en un one-shot y loops que siguen el tempo. Con modos al estilo de Live:
  *Re-Pitch* (lo actual), *Tonos* (formantes conservados, para melodías y voces), *Textura* (pads) y *Beats*
  (loops, respetando los golpes).
- **13-11-2026 · Distribución:** para el usuario y, como mucho, para compartir gratis; tiene que instalarse en varios
  ordenadores de casa, **entre ellos dos MacBook**. macOS pasa de opcional a necesario (VST3 y AU). La firma y el
  actualizador siguen siendo opcionales, y el español basta.
- **13-11-2026 · Loops:** sí, modo Loop/Tempo con troceado a teclas.

## 10. Hoja de ruta propuesta (cada fase se mide antes de pasar a la siguiente)

**Fase 0 · Crítica** (antes de nada nuevo; bajo riesgo)
- A/B a nivel igualado (7.5).
- Corregir los picos del motor B con sostén alto.
- Medir el aliasing del filtro escalera (T7) y corregirlo solo si existe.
- Tu escucha de la 0.9.1 (Mecanismo y volumen).
- **Éxito:** ningún pico sobre 0 dBFS en la auditoría de B; aliasing por debajo de -60 dB; tu aprobación.

**Fase 0 hecha (0.9.2, 13-11-2026):** A/B con nivel igualado; techo en el pico del sample para B, C y D (casos
de más de 3 dB sobre su pico: 95/120/94 → 14/0/14 de 506); filtro a 4x en los circuitos que saturan (aliasing en el
peor caso de -3 a -39 dB). Falta tu escucha. Detalles en DESIGN.md, 0.9.2.

**Fase 1 hecha (0.9.3, 13-11-2026):** Warp (Re-Pitch, Tonos, Textura) dentro de los seis motores, sin latencia ni
memoria extra. A ±12 st la duración queda entre ×0,94 y ×1,06 de la nota en su tono (Re-Pitch: ×0,52 a ×2,0) y
Tonos acerca los formantes 6-10 dB. Falta tu escucha. Detalles en DESIGN.md, 0.9.3.

**Hoja de ruta revisada con tus respuestas (sustituye a la de abajo donde difieran)**
1. **Fase 0 · Crítica:**
   - A/B a nivel igualado;
   - picos del motor B;
   - medir el aliasing del filtro;
   - tu escucha de la 0.9.1.
2. **Fase 1 · Warp:**
   - integración de Signalsmith Stretch;
   - modos Re-Pitch, Tonos, Textura y Beats en los motores A/D (y como fuente de B/E);
   - formantes conservados en Tonos.
   - **Éxito:** a ±12 st, la duración se mantiene (±2 %) y la envolvente de formantes dentro de 2 dB, sin clics; tu
     oído.
3. **Fase 2 · Loop/Tempo:**
   - detección de tempo y onsets;
   - sincronía con el DAW (Beats);
   - troceado a teclas.
   - **Éxito:** en fase con el DAW durante 10 minutos; cortes sobre los onsets reales.
4. **Fase 3 · macOS:**
   - compilar VST3 y AU para tus MacBook.
   - Yo no puedo compilar para Mac desde Windows: hace falta hacerlo en uno de los MacBook (con `build_mac.sh` y
     Xcode, guiado paso a paso) o con un servidor de compilación macOS en la nube.
5. **Fase 4:**
   - detección de nota en graves;
   - memoria;
   - MPE;
   - navegador de presets;
   - emulación de convertidores.

**Fase 1 · Alta: la calidad que nos diferencia** (propuesta original)
- Tono natural lejos de la nota (T1 + T4).
- Detección de nota en graves (7.6).
- Memoria (7.4).
- **Éxito:** a ±12 st, la envolvente de formantes dentro de 2 dB de la original; nota correcta en el 90 % o más de
  la batería de 448; memoria por instancia por debajo de 25 MB más la librería; tu aprobación a oído.

**Fase 2 · Alta o media, según tus respuestas**
- Opción A, multimuestra (7.2).
- Opción B, modo Loop/Tempo (7.3).
- **Éxito (A):** un multisample importado sin saltos de nivel entre zonas (±1 dB).
- **Éxito (B):** loops en fase con el DAW durante 10 minutos; troceado sobre los onsets reales.

**Fase 3 · Media**
- MPE y aftertouch polifónico.
- Navegador de presets con etiquetas.
- Emulación de convertidores (T6) en lugar de Bits y Muestreo.
- Simplificación de mandos.
- **Éxito:** MPE con un controlador real; presets encontrables en dos clics.

**Fase 4 · Distribución** (si lo vas a compartir)
- macOS, AU, firma, actualizador y versión en inglés.

**Experimental**
- NMF para separar golpe y cuerpo (T8).
- Modelado físico de excitación más profundo para el Mecanismo (cuerda o tubo excitado por los golpes reales).
- Modelos neuronales solo si lo convencional se queda corto (T9).

Cada fase se entrega por separado, con su batería A/B (mismos samples y nivel igualado), sus pruebas automáticas y
tu escucha, para detectar regresiones antes de seguir.

---

## Fuentes
- Kontakt 8: [estado de versiones de NI](https://community.native-instruments.com/discussion/39/official-update-status-kontakt-current-version-8-1-0), [nota de prensa](https://www.native-instruments.com/en/press-area/music-production/kontakt-8/)
- Falcon: [Falcon 2027, Sonicstate](https://sonicstate.com/news/2026/10/12/uvi-releases-falcon-2027/), [Falcon 2026](https://synthanatomy.com/2025/10/uvi-falcon-2026.html)
- HALion: [7.5 con MPE](https://synthanatomy.com/2026/08/steinberg-halion-7-flagship-sampler-plugin-gets-an-fm-and-spectral-makeover.html), [AudioWarp](https://steinberg.help/halion/v6/en/halion/topics/editing_zones/sound_editor_audiowarp_section_r.html)
- Omnisphere: [Omnisphere 3](https://djlifemag.com/2026/03/the-o3-spectrasonics-drops-omnisphere-version-3/), [importación de audio](https://support.spectrasonics.net/manual/Omnisphere2/user_audio/page02.html)
- Serato Sample 2: [análisis de MusicRadar](https://www.musicradar.com/reviews/serato-sample-2-review), [separación de pistas](https://www.musicradar.com/news/serato-sample-2-stem-separation)
- TAL-Sampler: [Synthtopia](https://www.synthtopia.com/content/2015/06/29/tal-sampler-an-analog-modeling-synth-with-sampling-sound-engine/), [modos DAC (KVR)](https://www.kvraudio.com/forum/viewtopic.php?p=9150487), [TAL-DAC](https://www.toolfarm.com/buy/tal-dac)
- Logic Sampler: [tutorial](https://www.audeobox.com/learn/logic-pro/sampler-quick-sampler-tutorial/)
- Tecnologías: [Signalsmith Stretch](https://signalsmith-audio.co.uk/code/stretch), [licencia de Rubber Band](https://breakfastquay.com/rubberband/license.html), [cambio de tono con formantes (MathWorks)](https://www.mathworks.com/help/audio/ref/shiftpitch.html)
- Fase anterior (articulaciones): [síntesis conmutada, CCRMA](https://ccrma.stanford.edu/~jos/waveguide/Commuted_Synthesis.html), [Timbre Toolbox](https://www.mcgill.ca/mpcl/files/mpcl/peeters_2011_jasa.pdf)
