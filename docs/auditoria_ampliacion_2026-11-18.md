# Auditoría técnica antes de la ampliación (2026-11-18)

Base: Reanima 0.9.5 (sha 2d15bb45 instalado). Nada de esto está programado todavía, salvo la herramienta de prueba de estrés (`ReanimaAudit estres`), que solo mide.

Leyenda: **CONFIRMADO** = medido con una prueba reproducible. **HIPÓTESIS** = deducido del código o de fuentes; falta medirlo.

---

## 5. Notas que se rompen al tocar muchas (prioridad 1: motor de audio)

### Prueba diseñada
`ReanimaAudit estres <wav> <prefijo> [parámetro=valor …] notes=N gap=ms held=s fresh=1 wav=1`

- Toca N notas seguidas, cada una en su propia tecla, a través del procesador completo (motor, FX y etapa master), con bloques de 64 muestras a 48 kHz.
- Después renderiza cada nota sola, en un procesador nuevo, y suma esas notas.
- Si las voces no se molestan entre sí, el resultado de todas juntas es idéntico a esa suma. La calibración da un residuo de −206 dB.
- Cada 20 ms informa de:
  - el residuo frente a la suma;
  - las voces que suenan;
  - el pico;
  - el tiempo de cada bloque frente al tiempo real.

### Resultados
| Caso | Hasta 8 notas a la vez | 9 notas o más |
|---|---|---|
| LOOP **Complex** (Bungee), loop de sintetizador 122 bpm | residuo −138 dB (idéntico) | **residuo −6,4 dB desde la 9.ª nota**: se rompe |
| LOOP **Complex Pro** (Signalsmith) | −139 dB a 4 notas, −43 dB a 8 | **−6,4 dB desde la 9.ª** |
| LOOP **Beats** | −137 dB hasta 16 notas | sin fallo |
| Motor A, 28 notas | −137 dB | sin fallo |

**CONFIRMADO — causa 1 (la más probable de lo que oyes):**
- El motor tiene solo **8 estiradores de tiempo** compartidos por todas las notas (`numStretchers = 8`, `SynthEngine.h`).
- Una nota que suena en su release sigue ocupando el suyo.
- La 9.ª nota no encuentra ninguno libre y pasa en silencio a otro lector («Textura»: granos superpuestos). Suena distinto y degradado.
- El sonido vuelve cuando terminan las notas anteriores y se libera un estirador. Coincide exactamente con lo que describes: «empieza a sonar roto y se recupera cuando terminan las voces».
- Afecta a Complex, a Complex Pro y a las colas de Beats por debajo de 1x, que también usan Bungee.

**CONFIRMADO por el código — causa 2 (misma familia):**
- El motor F (Drone) tiene 16 flujos para 32 voces.
- Desde la 17.ª nota a la vez, la nota sale sin su motor: se pierde o queda vacía.

**HIPÓTESIS — causa 3 (picos de CPU al empezar notas):**
- Los motores B y D, con 28 notas en 64 muestras, gastan de media un 27–34 % del tiempo del bloque.
- Pero algún bloque suelto llega al **139–158 %**: es el arranque de varias notas en el mismo bloque.
- En Ableton, con un búfer pequeño y otros plugins, eso es un chasquido o una caída de audio, que también suena «digital».
- Falta medirlo dentro de Live.

**Descartado:**
- Saturación por acumulación de voces (con Volumen −24 dB el pico se quedó en −14 dBFS).
- Robo de voces: con 28 notas no se llega al límite de 32.
- Asignaciones de memoria en tiempo real (RtCheck: 0).
- Interferencia entre voces en el motor A.

**Nota sobre la etapa master:**
- Con el Volumen por defecto (−12 dB) y muchas notas fuertes, la suma entra en la saturación de la etapa master, que empieza a −4 dBFS. Es el comportamiento diseñado (suave, con sobremuestreo), no un fallo.
- Aun así, comprobaré que con 32 voces a tope no suene «rota».

### Solución propuesta (sin bajar polifonía ni calidad)
1. **Un estirador por cada voz física** (40), reservados al preparar el plugin, nunca en el hilo de audio. Igual para los flujos del motor F (40).
   - Memoria estimada: ~1 MB por estirador. Lo mediré antes de decidir.
2. **Nunca cambiar de algoritmo en silencio.** Si algo se agota, se cede el recurso de la voz más silenciosa en su release, con un fundido de 10 ms. Así la nota nueva suena siempre como debe.
3. **Repartir el arranque de las notas** en B y D (como ya hace Bungee con su presupuesto por bloque) para quitar los picos de CPU, sin cambiar el sonido.
4. **Validación:**
   - la misma prueba con 32 notas (residuo ≤ −100 dB frente a la suma);
   - en bloques de 32, 64, 128, 512 y 1024, a 44,1, 48 y 96 kHz;
   - sin bloques por encima del tiempo real;
   - escucha en Ableton.

**Lo que necesito de ti:** el sample y los ajustes exactos con los que lo oíste (motor, PLAY LOOP o no, algoritmo). Con eso lo reproduzco tal cual.

---

## 1. Marcar y desmarcar transitorios a mano (LOOP)

**Código implicado:**
- `ReanimaProcessor::updateSlices`: corta por Transitorios, Rejilla o A mano.
- `setSliceMarks`, el estado `sliceMarks` y `WaveformView`, donde se arrastran las marcas.

**Situación actual (confirmada en el código):**
- Mover una marca hoy cambia el modo a «A mano» y congela la lista.
- A partir de ahí, Sensibilidad ya no actúa, y un análisis nuevo se olvida de lo hecho.

**Propuesta:**
- Una capa de **correcciones** sobre lo detectado: transitorios **desactivados** y transitorios **añadidos**, guardados en el proyecto y en el preset (estado `hitEdits`).
- Se identifican por su posición con una tolerancia de ±10 ms. Así sobreviven a volver a analizar y a cambiar Sensibilidad.
- En la forma de onda:
  - un clic en una marca la apaga (gris, discontinua) o la enciende;
  - doble clic en un hueco añade una;
  - Alt + clic quita una añadida.
- «A mano» sigue existiendo, igual que ahora.

**Reutiliza:** `cutPoints`, que ya comparten el corte, «Por golpe» y la reproducción. Al cambiarlos, todo queda coherente sin tocar el motor.

**Riesgo:** bajo. Los proyectos antiguos no tienen correcciones y suenan igual.

---

## 2. «Por golpe» en TIMBRE

**Código implicado:**
- `Voice::render`: `perHit` y `hitArr`, que marcan cada golpe en el bloque.
- La envolvente de amplitud por golpe (`hitT`, `hitFrom`) y ENV 2 (`e2Stage = 0` en cada golpe).
- `postProcess`: el oscurecimiento (`darkOct`).

**Qué hace TIMBRE hoy:**

| Control | Qué hace | ¿Por golpe? |
|---|---|---|
| Brillo (VelBright) | más oscuro con poca velocidad | **Sí:** el nivel de cada golpe (su transitorio) actúa como su «velocidad». |
| Arco (Bow) | oscurece mientras la envolvente está por debajo del original | **Sí:** sigue al Ataque de cada golpe. |
| Oscurecer al soltar (RelDark) | oscurece al caer | **Sí:** oscurece según la caída de cada golpe. |
| Bits, Muestreo | degradación fija | No: no depende del tiempo. |
| Mecanismo | ruido de tecla al pulsar y soltar | **Opcional** (te pregunto): un golpe de mecanismo en cada transitorio. |

**Propuesta:**
- Pasar a `postProcess` el nivel de la envolvente por golpe que ya calcula el motor.
- Usar los mismos `hitArr`: misma detección, misma selección (con tus correcciones del punto 1) y mismo instante que las envolventes.
- Sin «Por golpe», el código va por el camino de siempre y el resultado es idéntico bit a bit (lo compruebo).

**Riesgo:** bajo. El filtro de oscurecimiento ya se suaviza en 5 ms y no hará clic en cada golpe.

---

## 3. Ventana de loop móvil (1/32 a 4 compases)

**Código implicado:**
- `renderLoop`: hoy el loop es el sample entero (`loopInfo.length`).
- `loopBeatsNow`, la reproducción con el host (`LoopPlay`, `transportPpqNow`) y Snap a C.

**Propuesta:**
- Dos parámetros nuevos, automatizables:
  - **Duración**: 1/32, 1/16, 1/16T, 1/8, 1/8T, 1/8., 1/4, 1/4T, 1/4., 1/2, 1/2., 1, 2 y 4 compases;
  - **Posición**: dónde empieza la ventana, que se arrastra sobre la forma de onda.
- La duración se calcula con el tempo propio del loop (sus beats detectados), así que la ventana mide lo mismo en tiempo musical a cualquier tempo del DAW.
- Con PLAY con el host, la ventana se alinea con el compás del DAW.
- **Dibujo:** zona resaltada con el color del módulo, bordes de inicio y fin con asas, y la duración escrita encima («1/4», «2 compases»).
- «Completo», el valor por defecto, se comporta como hoy, así que los proyectos antiguos no cambian.

**Riesgo:**
- Medio. Afecta al lector de loop: Bungee, Signalsmith, Beats y Rubber Band.
- Rubber Band tendrá que renderizar de nuevo al mover la ventana; lo hará en segundo plano, como ahora.
- Es la mejora con más casos que probar.

---

## 4. Macros: doble aro en los rotativos

**Código implicado:**
- `Look.h` (cómo se dibuja el rotativo).
- La matriz de modulación: M1–M8 Src/Dst/Amt, fuentes Macro1–4, destinos `DstCutoff`, `DstRes`, etc.
- `Voice::mod`, que aplica la modulación.

**Propuesta:**
- **Aro interior:** el valor del parámetro, como ahora. **Aro exterior:** el valor efectivo, con el color del panel de macros (violeta).
- El valor efectivo se calcula en la interfaz (30 veces por segundo) con la misma fórmula que el motor: macro × cantidad, con su signo, y la escala de cada destino (semitonos en el corte, dB en el volumen…).
- No toca el valor base, la automatización, MIDI Learn ni los presets: es solo dibujo.
- **Limitación real:** solo una parte de los rotativos son destinos de la matriz (unos 16, del corte al aliento). El resto no se puede modular hoy y no tendrá aro exterior.

**Pregunta:** ¿solo las macros, o también los LFO y las envolventes? Esos cambian en cada voz: mostraría la última nota tocada.

**Riesgo:** bajo, solo interfaz. Hay que vigilar el CPU de redibujar con 30 fps.

---

## 6. Pestaña ARPEGIADOR / SECUENCIADOR

### Referentes estudiados
Resumen de las funciones que conozco de estos productos. Antes de diseñar en detalle las contrastaré con sus manuales actuales.
- **Pigments (Arturia):** secuenciador por pasos con modo Arpegiador y modo Secuenciador.
  - Carriles por paso: altura, velocidad, longitud, probabilidad, repetición («ratchet») y desplazamiento de octava.
  - Botón de «generar» con densidad y variación.
  - Tonalidad global.
- **Stepic (Devicemeister):** secuenciador MIDI de 8 carriles con dados por carril, recorridos alternos (adelante, péndulo, aleatorio) y longitud distinta por carril (polirritmia).
- **Sugar Bytes Thesys:** carriles independientes de nota, octava, velocidad, gate y controladores.
  - Cada carril con su longitud y su dirección, y patrones encadenables.
  - Generador aleatorio con reglas musicales.
- **Ableton Arpeggiator** y **Logic Arpeggiator:** como referencia de interpretación en directo (latch, retrigger, estilos).

### Tres enfoques
**A. Arpegiador avanzado (interpretación)**
- Tocas acordes y él los recorre: estilos, octavas, latch, retrigger, swing, probabilidad, ratchets y acento.
- Ventaja: inmediato y musical en directo.
- Limitación: no compone frases propias; depende de lo que tocas.

**B. Secuenciador por pasos avanzado**
- 1–64 pasos con carriles de nota, velocidad, gate, probabilidad, ratchet y deslizamiento.
- Cada carril con su longitud (polirritmia), patrones guardables, transposición por la tecla que pulsas y generación aleatoria según la escala.
- Ventaja: frases completas y repetibles.
- Limitación: más edición; menos espontáneo.

**C. Híbrido (mi recomendación)**
- El mismo motor de pasos en dos modos:
  - **Arp:** los pasos eligen *qué nota del acorde*;
  - **Seq:** los pasos son *notas* de la escala, transpuestas por la tecla.
- Los dos usan los mismos carriles de probabilidad, ratchet, swing y gate.
- Ventaja: un único sistema de tiempo y una sola interfaz.
- Limitación: más trabajo inicial. Lo haría por fases: primero el motor y el modo Arp, luego Seq y el generador.

**Común a los tres:**
- Sincronizado al BPM y a la posición del DAW (ya tenemos la posición del host para PLAY), o con un tempo libre propio.
- Arranca y para con el transporte, cuadrado al compás.
- Cambios en directo aplicados en el siguiente paso, para que no salgan notas a medias.

### ¿Notas solo para Reanima o también para otras pistas?
- **Interno:** funciona en todos los DAW.
- **Arrastrar el patrón como clip MIDI:** ya lo hacemos con SLICE; funciona en todos.
- **Salida MIDI en directo hacia otras pistas:** el VST3 puede declarar salida MIDI.
  - Ableton permite elegir la salida MIDI de un plugin como entrada de otra pista, según su documentación y la experiencia de otros fabricantes. Con VST3 hay informes contradictorios.
  - En AU (Mac) no lo he comprobado.
  - **Antes de prometerlo, lo pruebo en tu Ableton** con un VST3 de prueba.

---

## 7. Tonalidad del proyecto

| Fuente | ¿La da el host? | Cómo conseguirla | Fiabilidad |
|---|---|---|---|
| Tonalidad global del proyecto | **No en Ableton.** Live 12 tiene escala por clip, pero no se la pasa a plugins de terceros (no está documentado). VST3 tiene un campo de acordes que usan Cubase y Studio One. | Leer el campo de VST3 y probar en tu Live si llega algo. | Exacta si llega; en Live, probablemente nunca. |
| Acordes que suenan | No | Analizar las notas MIDI que entran (acordes por plantillas). | Alta con MIDI claro. |
| Notas MIDI entrantes | Sí (las notas) | Histograma de notas con decaimiento, comparado con los perfiles de tonalidad (Krumhansl). | Media; sube con más notas. Mostrar % de confianza. |
| Tonalidad del sample | No | Cromagrama del sample (el análisis ya detecta la nota de los one-shots) y los mismos perfiles. | Alta en loops tonales; nula en percusión (se dirá «sin tonalidad»). |
| Audio externo | Por sidechain | Entrada lateral del instrumento y cromagrama en tiempo real. **HIPÓTESIS:** no sé si Ableton deja enviar audio por sidechain a un *instrumento* VST3. Lo pruebo en tu Live. | Media; tarda unos segundos en estabilizarse. |

**Propuesta:**
- Un selector de tonalidad (raíz + escala) con origen: Manual, Sample, MIDI entrante o Audio externo.
- Siempre con % de confianza y la posibilidad de fijarla a mano.
- Una detección dudosa (< 60 %) no se aplica sola: se muestra como sugerencia.
- La usarán el arpegiador, el secuenciador y Snap a C (opcional).

---

## 8. Cargar desde Splice

**CONFIRMADO (Splice y Ableton lo documentan):**
- Arrastrar desde la pestaña Splice del navegador de Live a plugins de terceros **no está admitido**; solo a pistas y a Simpler o Sampler.
- Además, Live no avisa al plugin de que hay un arrastre en curso: no es un fallo nuestro y no se puede arreglar desde el plugin.
- Los samples que Live descarga de Splice quedan en la **User Library > Samples > Splice**; esa carpeta se cambia en las preferencias de Live.

**Propuesta:**
1. **Explorador integrado** en Reanima: carpetas favoritas, buscador, escucha previa y carga con doble clic.
   - La carpeta de Splice se añade una vez. Reanima busca la User Library donde la tenga Live (lee la preferencia de Live si existe; si no, te la pide). Sin rutas fijas.
2. **Arrastrar desde una pista de Live** a Reanima (el clip de audio): ya debería funcionar. Lo pruebo.
3. «Últimos descargados»: lista de lo más reciente de esa carpeta.

---

## 9. Resíntesis (motor C): ruido, caídas y resultados irregulares

**Medición rápida:** 29 one-shots variados, nota sostenida, motor C frente al motor A. Columnas: peor caída de nivel en 20 ms y cambio de «ruidosidad».

**CONFIRMADO en algunos samples:**
- **Caídas de volumen:** voz al revés −21,6 dB; voz destrozada −13,7 dB; sintetizador modular −6 dB.
  - Lo más probable: el seguimiento de tono pierde la fundamental unos instantes y los parciales se apagan.
- **Ruido de fondo de más:** sintetizador modular +2,8 dB y voz al revés +3,9 dB.
- **Al revés en los bombos:** el motor C les quita casi todo el ruido (de −21 a −29 dB): suenan más «tonales» de lo que son.
- **Ruido puro** (tetera hirviendo): el motor C se desmorona (caída de −58 dB).

**HIPÓTESIS sobre las causas (`Reconstruct.cpp`, `SampleAnalysis`):**
1. Ventana fija: resolución corta para graves y larga para transitorios.
2. Seguimiento de parciales sin «memoria»: un hueco de un fotograma apaga el parcial.
3. Separación tonal/ruido por umbral fijo: en voces y FX se mezclan.
4. Fases inventadas en las uniones: cancelaciones cuando hay parciales cercanos.
5. La envolvente de nivel se reconstruye desde los parciales y no desde la señal original.

**Propuesta:**
- Primero un banco de pruebas como el de los estiradores: 40 samples por familia, con medidas de caídas, ruido, error espectral y transitorios, y tu escucha.
- Después atacar cada etapa por separado:
  - seguimiento con continuidad (rellenar huecos cortos);
  - ventanas multirresolución;
  - separación tonal/ruido adaptativa;
  - corrección final de la envolvente contra el original.
- Sin reducción de ruido ni normalización global.

**Riesgo:** alto; es el cambio más delicado del sonido. Va el último, con comparación A/B de oído.

---

## 10. Imán (Snap to grid) en EDICIÓN

**Código implicado:**
- `EditView`: la selección se guarda en fracciones de doble precisión, mucho más fina que una muestra.
- Ya existe `snapsToZero` (ajuste al cruce por cero), con zoom y desplazamiento.

**Propuesta:**
- **Botón de imán** (también con la tecla G) y un menú de resolución: Auto, 4, 2 y 1 compás, 1/2, 1/4, 1/8, 1/16, 1/32, 1/64 y tresillos.
- **Auto** elige la división que deja unos 12–40 píxeles entre líneas según el zoom.
- **Origen de la rejilla:**
  - con tempo: los beats del sample, si es un loop detectado o le das BPM;
  - sin tempo: segundos y milisegundos (1 s, 100 ms, 10 ms, 1 ms) y, como opción, los transitorios.
- **Al arrastrar:**
  - el borde salta a la línea más cercana dentro de 10 px;
  - si no hay ninguna tan cerca, queda libre (sin saltos bruscos);
  - con Alt se ignora el imán mientras se arrastra.
- Si el imán y el cruce por cero coinciden, primero va la rejilla y después el cruce por cero más próximo, dentro de 1 ms, para no hacer clic.
- Hacer zoom no mueve la selección: solo cambia el dibujo.
- **Integración con LOOP:**
  - la rejilla de EDICIÓN usa los mismos beats que LOOP;
  - los transitorios se dibujan como marcas aparte;
  - la ventana de loop (punto 3) se ajusta a la misma rejilla.

**Riesgo:** bajo.

---

## Orden de trabajo propuesto (por fases, cada una probada y escuchada antes de la siguiente)
1. **Punto 5:** el fallo de las notas. Es estabilidad y no cambia ningún sonido correcto.
2. **Puntos 10 y 1:** imán y edición de transitorios. Comparten la rejilla y las marcas.
3. **Punto 2:** «Por golpe» en TIMBRE. Usa las marcas del punto 1.
4. **Punto 3:** ventana de loop.
5. **Punto 4:** doble aro de las macros.
6. **Punto 8:** explorador de samples.
7. **Puntos 7 y 6:** tonalidad y después el arpegiador/secuenciador, según el enfoque que elijas.
8. **Punto 9:** resíntesis, con banco de pruebas y tu escucha.

## Riesgos generales
- **Compatibilidad:**
  - todo lo nuevo arranca en «como hoy»;
  - los parámetros nuevos se añaden al final, así que no cambian los IDs de la automatización ya grabada;
  - los presets antiguos se cargan igual (lo cubre la prueba de estado).
- **CPU:**
  - 40 estiradores reservados no gastan si no suenan, pero 32 notas Complex sí; lo mediré y lo repartiré;
  - el secuenciador y la tonalidad gastan poco;
  - el análisis de audio externo, algo.
- **Memoria:** unos 40 MB más con 40 estiradores (estimado; se mide).
- **Sonido:** solo los puntos 3 y 9 tocan cómo suena algo que ya funciona. Irán con A/B.

## Fuentes
- Splice: [How to download Splice samples in Ableton Live 12.3](https://support.splice.com/en/articles/12059504-how-to-download-splice-samples-in-ableton-live-12-3) (no se admite arrastrar a plugins de terceros; carpeta User Library > Samples > Splice).
- Ableton: [Splice integration in Ableton Live FAQ](https://help.ableton.com/hc/en-us/articles/22060471916316-Splice-integration-in-Ableton-Live-FAQ), [Accessing the MIDI output of a VST plug-in](https://help.ableton.com/hc/en-us/articles/209070189-Accessing-the-MIDI-output-of-a-VST-plug-in), [Keys and Scales in Live 12 FAQ](https://help.ableton.com/hc/articles/11425083250972).
- Foros de Ableton y JUCE sobre arrastres desde el navegador de Live y MIDI de VST3 ([forum.ableton.com](https://forum.ableton.com/viewtopic.php?p=1515954), [forum.juce.com](https://forum.juce.com/t/vst3-midi-plugins-wont-load-in-ableton-live/36323?page=2)).

## Decisiones del usuario (2026-11-18)
- Arpegiador/secuenciador: **híbrido**, por fases (primero Arp).
- Notas generadas: **motor interno + arrastrar el patrón como clip MIDI**. Sin salida MIDI en directo.
- «Por golpe» en TIMBRE: Brillo, Arco y Oscurecer al soltar, más **un interruptor opcional para que Mecanismo dé su golpe en cada transitorio**.
- Doble aro: **macros + LFO y envolventes**; lo que cambia en cada voz sigue a la última nota tocada.

---

## Fase 1 hecha: notas que se rompían (2026-11-18)

### Cambios
1. **Un estirador por cada voz** (40, antes 8) para LOOP Complex, Complex Pro y las colas de Beats, y **un flujo de motor F por voz** (40, antes 16).
   - Los estiradores solo se crean cuando se elige LOOP (fuera del hilo de audio), así que sin LOOP el plugin ocupa 16 MB *menos* que antes. Con LOOP ocupa unos 76 MB más (unos 2 MB por estirador).
   - Si cambia la frecuencia de muestreo, se rehacen en `prepare`, con el audio parado.
2. **Semilla aleatoria propia en cada nota.**
   - Antes, cada voz seguía con su propia secuencia de una nota a otra. Lo que salía (granos, ruido, fases del motor F, Mecanismo) dependía de lo que esa voz hubiera tocado antes, y no había forma de distinguir una voz molestada por otra de una simple casualidad.
   - Ahora cada nota toma una semilla nueva de la secuencia del motor. En las pruebas se puede fijar por tecla (`keySeeds`). El carácter aleatorio de los motores no cambia.

### Pruebas (`ReanimaAudit estres … fresh=1`: todas las notas juntas frente a la suma de cada nota sola, en un procesador nuevo)
| Caso | Antes | Ahora |
|---|---|---|
| LOOP Complex, 9 / 16 / 32 notas | −6,4 dB (rota desde la 9.ª) | −138 / −137 / −136 dB (idéntico) |
| LOOP Complex Pro, 9 / 32 notas | −6,4 dB | −44 / −27,5 dB; por bandas de 1/3 de octava, 0,1–0,2 dB de media: solo fase |
| Motores A–F, 28 notas (sintetizador pluck y metales) | no comparable (azar por voz) | ninguna ventana peor de −45 dB; residuo total entre −56 y −139 dB |
| Búferes de 32, 128, 512 y 1024; 44,1 / 48 / 96 kHz (Complex, 12 notas) | — | −137 / −138 dB |

### CPU (bloques de 64 a 48 kHz, un núcleo de este PC, notas de LOOP estiradas a la vez)
- Complex y Complex Pro: 8 y 16 notas, ningún bloque por encima del 80 %.
- 24 notas: 3 y 5 bloques por encima del 80 %.
- 32 notas: Complex, 38 bloques por encima del 80 % (peor 108 %); **Complex Pro va justo de CPU** (242 bloques por encima del 80 %; peor 157 %).
- Antes de este arreglo, esas notas costaban poco porque sonaban rotas.
- Los picos de los motores B y D que aparecieron en la primera medición no se repiten en tres pasadas: eran ruido del sistema.

### Hallazgo aparte: Complex Pro a 88,2 / 96 kHz
- Signalsmith recibe el audio a la frecuencia del archivo. Con un sample de 44,1 kHz en un proyecto a 96 kHz, la biblioteca ve un estiramiento de 2,18× aunque no cambie el tempo.
- Por encima de 2× activa a propósito su modo de fase aleatoria (con semilla del sistema): suena algo más difuso y no se repite exacto.
- Propuesta: entregarle el audio ya a la frecuencia del proyecto, como ya se hace con Bungee. Pendiente de tu aprobación (cambia el sonido de Pro a 88,2 y 96 kHz).

### Arreglo aprobado: Complex Pro recibe el loop a la frecuencia del proyecto (2026-11-18)
- Signalsmith recibe ahora el loop leído a la frecuencia del proyecto con interpolación band-limited (como Bungee). Solo ve el estiramiento del tempo y la transposición de la tecla.
- A la frecuencia del propio archivo no cambia nada: lee las muestras tal cual (resultado idéntico).
- Medido con un loop de 44,1 kHz a +7 y −7 semitonos, frente al resultado a 44,1 kHz (dif. media por bandas de 1/3 de octava / correlación de envolventes):

| Tasa | Antes | Ahora |
|---|---|---|
| 48 kHz | 0,17–0,22 dB / 0,97–0,98 | 0,05–0,08 dB / 0,98 |
| 88,2 kHz | 2,1–2,3 dB / 0,82–0,89 | 0,01 dB / 0,998–0,999 |
| 96 kHz | 2,4–2,5 dB / 0,78–0,88 | 0,05–0,08 dB / 0,98 |

### Validación final de la fase 1
- ReanimaTests: todo pasa. El test de tono del motor F mide ahora la mediana de varios instantes; antes medía uno solo, y las fases aleatorias del motor F podían dejar más fuerte el 2.º armónico justo en ese instante.
- RtCheck: todo pasa (0 asignaciones en tiempo real).
- Auditoría: 425/425. LOOP: 57/57.
- HostCheck del VST3 instalado: OK.
- Estrés con 16 notas (Complex y Complex Pro): idéntico a cada nota sola (−137 dB) a 48 kHz/64, 96 kHz/256 y 88,2 kHz/1024. Un bloque al 102 % en Complex a 96 kHz/256.
- Instalado en Program Files/Common Files/VST3/Blackpanda. La copia anterior está en backups/instalado_antes_voces_2026-11-18.

- **Escucha del usuario en Ableton (2026-11-19):** «Suena bien». Fase 1 aprobada.

---

## Fase 2: transitorios a mano en LOOP e imán en EDICIÓN (2026-11-22)

### Transitorios a mano (punto 1)
- **Correcciones** (`Source/HitEdits.h`): golpes **apagados** y **añadidos**, guardados por posición en el estado del proyecto y del preset (`hitEdits`, p. ej. `off:31991|on:16457`).
  - Se reconocen con una tolerancia de ±10 ms.
  - Se aplican a todas las listas de cortes que salen de los golpes: SLICE por Transitorios, Por golpe y **Beats**. Beats ahora lee sus cortes del motor (`beatSlices`), con las correcciones aplicadas.
  - Sin correcciones, todo usa los cortes de siempre (el mismo código y el mismo resultado).
  - Un sample nuevo empieza sin correcciones. Al reanalizar el mismo sample (por ejemplo, al cambiar de motor) se conservan.
  - Al activar Reverse no se reflejan: las posiciones quedan donde estaban.
- **Uso (forma de onda de SAMPLE):**
  - En LOOP Normal: clic en un golpe lo apaga (gris, discontinuo, con una ×). Otro clic lo enciende. Doble clic en un hueco añade uno (se pega al golpe detectado más cercano; con Mayús, libre). Los añadidos llevan un punto encima.
  - En SLICE por Transitorios: clic en un corte lo apaga; doble clic en un hueco añade uno, sin pasar a «A mano». Arrastrar un corte sigue moviéndolo y entonces los cortes pasan a «A mano», como antes.
  - En LOOP Normal, el doble clic ya no pone el inicio de la nota, porque el loop siempre empieza en 0.
- **Pruebas** (`ReanimaAudit golpes`, 14/14):
  - lectura y escritura de las correcciones;
  - quitar y añadir golpes;
  - mover Sensibilidad;
  - guardar y reabrir el proyecto;
  - reanálisis al cambiar de motor;
  - sample nuevo;
  - Beats a 100 BPM con un loop de 120: con el 4.º golpe apagado, el 4.º tono llega con el trozo anterior (0,852 s; con el golpe, 0,902 s; esperado 0,85 y 0,90).
- **Hallazgo aparte (sin cambiar):** al afinar la posición de un golpe, el análisis retrocede mientras el nivel siga por encima del 10 % del pico. Encima de la cola fuerte de una nota anterior, puede situarlo hasta unos 20 ms antes de tiempo; la regla de Transitorios («el nivel sube en 15 ms») lo descarta entonces. Pendiente de medir con loops reales y de proponerlo.

### Imán en EDICIÓN (punto 10)
- **Botón «Imán»** (tecla G) y **menú de rejilla**: Auto, que según el zoom deja al menos 40 px entre líneas; 4, 2 y 1 compás; 1/2 a 1/64; tresillos 1/4T, 1/8T y 1/16T. Ambos se guardan con el proyecto.
- **Rejilla:**
  - Con tempo, los tiempos del loop (en LOOP o si el análisis está seguro de que es un loop) o, si no, el tempo del proyecto, contados desde el principio del archivo. Se dibujan con compases, tiempos y subdivisiones, y etiquetas compás.tiempo.
  - Sin tempo: tiempo, de 60 s a 1 ms según el zoom.
- **Comportamiento con el imán:**
  - Crear una selección, arrastrar sus **bordes** (nuevo, también sin imán) y arrastrar las líneas (inicio, fin, bucle, fundidos) va siempre a la línea más cercana, en la muestra exacta. Alt deja el borde libre mientras se arrastra.
  - No se mueve al cruce por cero (precisión).
  - El zoom no cambia la selección.
  - La cabecera muestra la duración de la selección en tiempos y la rejilla activa.
- **Pruebas** (`ReanimaAudit iman`, 12/12): Auto con el loop entero (1/16) y al acercar (1/64); 1/16 y 1/4T exactos a la muestra; el zoom no mueve la selección; se guarda con el proyecto; sin tempo, de 100 ms a 1 ms.
- **Capturas** al 100 % y al 75 %: release/capturas_fase2_2026-11-22.

### Escucha del usuario de la fase 2 (2026-11-22)
«Funciona bien». Pidió llevar la edición de golpes también a EDICIÓN.

### Golpes a mano también en EDICIÓN
- En LOOP, una franja al pie de la forma de onda de EDICIÓN muestra los mismos cortes que SAMPLE: los activos, los apagados (gris, con ×) y los añadidos (con un punto).
  - Clic en uno: se apaga, o vuelve a encenderse si estaba apagado.
  - Doble clic en un hueco de la franja: añade un golpe. Con el imán va a la rejilla; sin él, al golpe detectado más cercano; con Mayús, queda libre.
- La lógica es compartida en el procesador (`getLoopCuts`, `hitsEditable`, `toggleHit`, `addHit`): SAMPLE y EDICIÓN no pueden contradecirse. Prueba: `golpes` 15/15.

---

## Fase 3: «Por golpe» en TIMBRE (2026-11-22)
- Usa exactamente los golpes de «Por golpe»: los mismos cruces de corte (`hitArr`, `hitT`), con las correcciones del punto 1.
- **Con «Por golpe» en un loop:**
  - **Brillo:** cada golpe se oscurece según su fuerza, como la velocidad de una tecla. La fuerza es el pico de sus primeros 15 ms frente al de los golpes fuertes del loop; las tres cuartas partes de los golpes quedan por debajo de esa referencia.
  - **Arco:** oscurece el comienzo de cada golpe mientras avanza su Ataque, como un ataque lento en una nota.
  - **Apagado:** oscurece cada golpe a medida que cae hacia el Sustain, hasta 2,5 octavas.
  - **Mecanismo por golpe** (parámetro nuevo `mechPerHit`, apagado de serie): Mecanismo da su golpe de tecla en cada golpe del loop, más fuerte en los golpes fuertes.
  - Arco/aliento ya no aparece atenuado en LOOP con «Por golpe», porque ahí actúa.
- **Sin «Por golpe»:** nada cambia. Medido bit a bit con `procloop` frente a la versión anterior, en un loop de batería y otro de sintetizador: diferencia 0,0.
- **Interfaz:**
  - Interruptor «Por golpe» en la fila de Mecanismo de TIMBRE; solo se ilumina donde actúa.
  - El consejo de Mecanismo ocupa dos líneas si no cabe en una, y la información completa está en el tooltip.
  - La ayuda de «Por golpe» explica lo que hace en TIMBRE.
- **Pruebas** (`ReanimaAudit timbre`, 5/5, con un loop de 8 golpes de ruido, fuertes y flojos alternos):
  - Brillo: los flojos quedan 6,1 dB más oscuros en agudos (sin «Por golpe», −0,1 dB);
  - Arco: −4,1 dB de agudos al empezar cada golpe;
  - Apagado: −11,3 dB al final de cada golpe y −4,7 dB al principio;
  - Mecanismo: golpe en 7 de 7 golpes, y ninguno sin «Por golpe».

### Escucha del usuario de la fase 3 (2026-11-22)
«Suena bien».

---

## Fase 4: ventana de loop móvil (punto 3, 2026-11-22)
- **Parámetros nuevos**, al final de la lista para no mover la automatización ya grabada:
  - **Ventana** (`loopWinLen`): Completo, 4, 2 y 1 compás, 1/2, 1/4., 1/4, 1/4T, 1/8., 1/8, 1/8T, 1/16, 1/16T y 1/32. De serie, Completo.
  - **Posición** (`loopWinPos`): en tiempos desde el principio del loop.
- **Motor** (`SynthEngine::loopWindow`): en LOOP Normal, la parte elegida pasa a ser el loop.
  - Su principio es el 0 de la lectura y sus cortes son los que caen dentro.
  - Sirve con Beats, Complex, Complex Pro y Rubber Band. Rubber Band renderiza solo esa parte.
  - También funciona con «Por golpe» (cuenta los golpes de dentro), con TIMBRE por golpe y con Play con el host (repite sobre los tiempos del DAW).
  - La duración se calcula con el tempo propio del loop, así que mide lo mismo a cualquier tempo del proyecto.
  - Sin tempo conocido, o en SLICE, se usa el loop entero.
- **Mover o cambiar la ventana mientras suena:**
  - la parte anterior se apaga en 3 ms y la nueva entra en otros 3 ms;
  - el estirador empieza de nuevo;
  - la lectura directa del archivo (a la velocidad propia del loop) también hace el fundido de entrada; antes no lo hacía y el cambio sonaba con clic.
- **Interfaz:**
  - En MOTOR, el menú «Ventana» va junto a Modo loop, Duración y Calidad, y el rotativo «Posición» junto a BPM original; solo se ilumina con una ventana activa.
  - En la forma de onda, lo de fuera se oscurece y la ventana muestra sus bordes y una etiqueta («VENTANA 1 comp. · desde 2.1»). Se arrastra por el cuerpo, pegándose a semicorcheas (con Alt, libre).
  - La forma de onda se redibuja al cambiar Ventana, Posición o «Por golpe».
- **Pruebas** (`ReanimaAudit ventana`, 12/12, con un loop de 8 tonos de una corchea cada uno):
  - Completo suena entero;
  - 1/4 desde el tiempo 2 da solo los tonos 3 y 4, con Beats, Complex, Complex Pro y Rubber Band, a 120 y a 100 BPM;
  - 1/8 desde el tiempo 2,5 da solo el tono 6;
  - moverla cada 0,4 s no produce clics (esquina máxima 0,036 frente a 0,058 del propio tono);
  - con Play, sigue los tiempos del DAW (12 de 12).
- **Sin ventana (Completo):** bit a bit igual que antes, con `procloop` en un loop de batería y otro de sintetizador.

### Escucha del usuario de la fase 4 (2026-11-22)
«Funciona bien».

---

## Fase 5: doble aro de modulación en los rotativos (punto 4, 2026-11-22)
- **Qué se ve:** en un mando que mueve la matriz, el aro **interior** es el valor puesto, con el color de su panel. El aro **exterior**, verde como el panel de macros, va del valor puesto a donde lo lleva la modulación, con un punto al final.
  - Sin modulación, el mando se dibuja igual que siempre.
  - Se ve en todos los mandos que son destino de la matriz: Frecuencia, Resonancia, Drive, Morph, Volumen, Pan, Inicio/Posición, Spray, Grano, Detune, Transponer, la velocidad de los LFO 1 y 2, Gan. trans., Arco y Respiración.
- **Qué modulación:**
  - La de **la última nota tocada**: sus LFO, envolventes, velocidad, nota, rueda, presión, macros y azar. La publica el motor en cada bloque (`modShown`).
  - **Sin notas sonando, solo las macros.**
  - El valor del aro se calcula con la misma fórmula que aplica el motor a cada destino (`SynthEngine::modulated`). Por ejemplo, en el corte, 60 semitonos por unidad.
  - Se actualiza 30 veces por segundo, solo en los mandos visibles.
- **No toca nada más:** el valor base, la automatización, MIDI Learn, los presets y el estado son los de siempre; el aro solo se dibuja.
- **Pruebas** (`ReanimaAudit aros`, 6/6):
  - sin notas, solo las macros;
  - el corte se modula como lo aplica el motor (1000 Hz → 2828 Hz);
  - sigue a la última nota (velocidad 1,0 y después 0,3);
  - un LFO se mueve en directo (de −0,4 a +0,4);
  - el valor base no cambia;
  - cada destino tiene su mando.
- **Captura:** release/capturas_fase2_2026-11-22/aros_1_filtro_mod.png.

### Valoración del usuario de la fase 5 (2026-11-22)
«Se ve bien».

---

## Fase 6: explorador de samples y acceso a Splice (punto 8, 2026-11-22)
- **Por qué no se puede arrastrar desde la pestaña Splice de Live:** Splice y Ableton documentan que esa pestaña solo deja soltar en pistas, Simpler y Sampler, no en plug-ins de otras marcas. Live ni siquiera avisa al plug-in de que hay un arrastre en curso. No depende de Reanima. Arrastrar archivos desde el Explorador de Windows o el Finder, desde los Places de Live o desde la app de Splice sí funcionaba ya.
- **Explorador** (`Source/SampleBrowser.*`): se abre con un botón con icono de carpeta junto a «Cargar» y aparece encima de las pestañas.
  - **Fuentes**, leídas de la configuración de Live (`Library.cfg` de la versión más reciente, en Windows y en Mac):
    - la carpeta donde Live descarga lo de su pestaña Splice (`User Library/Samples/Splice`, o la carpeta propia si se ha elegido otra);
    - los Places del usuario (en este PC, PROYECTOS, WAVS y `Documents\Splice`, la de la app de Splice);
    - la User Library.
  - Sin rutas fijas. Primero aparece lo de Splice y **«Recientes de Splice»**: los 100 archivos más nuevos de las carpetas de Splice.
  - Se pueden añadir **carpetas favoritas** («+ Añadir carpeta…»; clic derecho en una favorita para quitarla). Se guardan en las preferencias de Reanima. Si no hay Live, el explorador lo dice y basta con añadir carpetas.
  - **Lista:** carpetas y archivos de audio, ↑ para subir, buscador por palabras en toda la fuente (hasta 500 resultados, en segundo plano).
  - **Clic o flechas:** escucha previa. **Doble clic o Intro:** carga el sample. **Esc:** cierra.
- **Escucha previa** (en el procesador): los primeros 15 s del archivo, a −6 dB, mezclados tras el instrumento, sin tocar el sample cargado.
  - El audio llega al hilo de audio por un buzón sin bloqueos y la memoria se libera en el hilo de mensajes: nada se crea ni se libera en el hilo de audio.
  - Hay un fundido de 5 ms al parar y un botón «Escuchar» para desactivarla.
- **Pruebas** (`ReanimaAudit explorador`, 4/4):
  - un `Library.cfg` de prueba con descargas de Splice en una carpeta propia, un Place inexistente que se descarta y la User Library;
  - el de este PC: PROYECTOS, WAVS, Splice y User Library;
  - la escucha previa suena sin nota, se para y no cambia el sample;
  - un archivo tras otro, sin que se acumulen.
- **Captura:** release/capturas_fase2_2026-11-22/explorador_1.png.

### Prueba del usuario de la fase 6 (2026-11-22)
«Funciona bien».

---

## Fase 7: tonalidad (punto 7, 2026-11-22)
- **Detector** (`Source/KeyDetect.*`):
  - **Por el nombre:** «…_G#min», «_A#m», «_Dm», «_Bbmaj», «Fs minor», «_A». Sin m/min se toma como mayor. Una letra suelta lejos del final no cuenta.
  - **Por el sonido:** cromagrama de los picos espectrales (55 Hz–2 kHz, ventanas de 8192 muestras), comparado con los perfiles de Krumhansl y Kessler. Los picos que parecen armónicos de otro más grave (×3, ×5) pesan menos. La confianza combina el encaje, la ventaja sobre la siguiente tonalidad y lo tonal que es el sonido.
  - **Por el MIDI:** histograma de las notas tocadas, ponderado por velocidad. Pierde la mitad de su peso cada 30 s y hace falta tocar unas 16 notas para que la estimación sea segura.
- **Medición** con 1129 samples de Splice que llevan su tonalidad en el nombre (`ReanimaBatch tonalidad`; resultados en research/tonalidad_2026-11-22):
  - Una primera versión acertaba al azar (4 %) por un fallo: el denominador de la interpolación de picos estaba mal limitado y los NaN caían en «Mi». Corregido.
  - Con la corrección y la atenuación de armónicos: confianza < 20 %, 29 % de tonalidad o relativa; 20–40 %, 47 %; 40–60 %, 58 %; 60–80 %, 74 %; ≥ 80 %, 75 %.
  - Solo uno de cada cinco samples supera el 60 %. El error típico es la quinta, por los armónicos.
  - **Conclusión:** por el sonido no es lo bastante fiable para aplicarse sola. Se muestra como sugerencia con su fiabilidad medida («acierta unas 7 de cada 10 veces») y se aplica con «Fijar».
- **Parámetros nuevos** (al final de la lista): `keySource` (A mano | Sample | MIDI), `keyRoot` y `keyScale` (mayor o menor).
- **Tonalidad en uso** (`ReanimaProcessor::keyInUse`):
  - A mano: la elegida.
  - Sample: la del nombre si la dice; si no, la última segura, y la del sonido como sugerencia.
  - MIDI: la estimada si llega al 60 %; si no, la última segura, marcada como dudosa.
  - Nunca se aplica una estimación dudosa.
- **Las notas MIDI** pasan del hilo de audio al de mensajes por una cola sin bloqueos. La estimación por el sonido se calcula en el hilo de análisis y su resultado vuelve con una referencia débil, por si el procesador ya no existe.
- **Interfaz:** pestaña nueva **ARP · SEQ** con el panel TONALIDAD.
  - Contiene la fuente, la tónica y la escala; la tonalidad en uso en grande; lo que dicen el SAMPLE y el MIDI, con su fiabilidad; y el botón «Fijar».
  - El resto de la pestaña queda reservado para el arpegiador y secuenciador.
  - Tiene su guía de tres niveles y su texto en el tour de bienvenida.
- **Pruebas** (`ReanimaAudit tonalidad`, 6/6):
  - 8 nombres;
  - una progresión en La menor por el sonido (La menor, 92 %);
  - 3 notas MIDI dan una estimación dudosa que no se aplica;
  - una escala de Re mayor, segura (100 %), sí se aplica;
  - «Fijar» la pasa a mano;
  - se guarda con el proyecto.
  - Las capturas de la interfaz, 209 sin fallos, tras corregir dos supuestos de la herramienta de capturas que daban por hecho 5 pestañas.
- **Sidechain (decidido por el usuario: «Sí, añádela», 2026-11-22).**
  - **Entrada lateral** «Sidechain» estéreo (vale mono o apagada). En VST3 es auxiliar (kAux, vía `VST3ClientExtensions::getPluginHasMainInput() = false`): Live la muestra en el panel Sidechain del dispositivo. No existe en la aplicación independiente (sería el micrófono).
  - **Recorrido del audio:** el hilo de audio lo copia en mono, antes de que el motor escriba el búfer, a un anillo sin bloqueos (2^17 muestras). El hilo de mensajes lo analiza con `KeyDetect::AudioStream`: ventanas de 8192 con solape de la mitad, las mismas cuentas que el análisis del sample (función `analyseFrame` compartida), y un olvido de media vida de 30 s. La confianza crece con los segundos de sonido con notas: completa a los 20 s.
  - **Fuente nueva** «Audio externo» en «Tonalidad de», añadida al final, así que los valores guardados no cambian. Se aplica cuando es segura (≥ 60 %), igual que el MIDI.
  - **Medición** (`ReanimaBatch tonalidad_flujo`, research/tonalidad_2026-11-22/flujo_sidechain.txt): 240 «pistas» hechas de samples de Splice de la misma tonalidad, seguidos.
    - A los 10 s nunca es segura.
    - A los 20 s: 77 de 238 seguras, y de ellas el 90 % con la tonalidad o su relativa.
    - A los 40 s: 106 de 236 seguras, con un 92 % de acierto.
    - Esto justifica aplicarla sola cuando es segura. Es una aproximación a una canción, no canciones reales.
  - **El análisis del sample no cambia:** en los 227 archivos comparados con la medición anterior, el resultado es idéntico.
  - **Pruebas** (`ReanimaAudit tonalidad`, 11/11):
    - la entrada existe y es auxiliar;
    - puede ir apagada o en mono;
    - el audio lateral no sale por Reanima (−200 dB sin notas);
    - a los 5 s la estimación es dudosa;
    - tras 30 s de una progresión en La menor da «La menor 89 %», segura, y se usa.
  - **Sin comprobar:**
    - cómo abre Ableton los proyectos guardados con la versión anterior (el estado no guarda buses; debería abrirlos igual);
    - el AU con entrada en macOS (auval, en GitHub cuando se suba).

## Fase 8: arpegiador y secuenciador híbrido (punto 6, 2026-11-22)
- **Referentes contrastados** (búsqueda 2026-11-22):
  - Pigments: carriles de altura, octava, velocidad, probabilidad, gate y slide, de longitudes independientes; modos Arp y Seq; dados por carril; direcciones; generador en la escala.
  - Thesys: carriles de octava, velocidad, gate, acorde y roll, de longitudes distintas; aleatorizador.
  - Stepic: secuencias de longitud propia por sección y muchas opciones de azar.
- **Motor** (`Source/StepSeq.h/.cpp`): un solo motor de pasos, que va antes del motor de sonido.
  - **Arp:** cada paso toca la siguiente nota del acorde mantenido. Estilos: Arriba, Abajo, Arriba y abajo, Abajo y arriba, Como tocas, Aleatorio y Acorde; con 1 a 4 octavas.
  - **Seq:** cada paso es un grado de la tonalidad en uso, contado desde la tecla pulsada, que transporta la frase. Direcciones: Adelante, Atrás, Péndulo y Aleatoria.
  - **Siete carriles de hasta 16 pasos, cada uno con su longitud (polirritmo):** PASO, NOTA (grados), OCTAVA, VELOCIDAD, GATE, PROBABILIDAD y REPETIR (1–4).
  - **Ajustes generales:** Ritmo de 1/1 a 1/32 (con puntillo y tresillo), Swing (100 % = tresillo), Gate global y Latch.
  - **Tempo DAW:** al tempo del proyecto y, con el transporte en marcha, cuadrado con sus compases; un paso que empezó hace menos de 30 ms suena al momento. **Tempo Propio:** su BPM, desde la primera tecla.
  - **Cambios en directo:** el patrón se lee en cada paso.
  - **Apagado:** el MIDI pasa sin tocar.
  - **Sin notas colgadas** al soltar, al saltar el DAW y al apagar.
- **Patrón:**
  - se guarda con el proyecto y el preset (propiedad `seqPattern`);
  - Guardar.../Cargar... en Documentos/Blackpanda/Reanima/Patrones (.reanimapat);
  - Generar (y un dado por carril) en la tonalidad, con Densidad;
  - Borrar.
- **ARRASTRAR MIDI:** el patrón tocado offline desde el compás 1, con el último acorde (Arp) o la última tecla (Seq) pulsados; sin ninguno, Do mayor. Dura hasta que los carriles coinciden, de 1 a 8 compases, con semilla fija.
- **No incluido:**
  - **slide (portamento por paso):** el motor de voces no tiene ligado por nota;
  - **salida MIDI en directo:** decisión del usuario;
  - **encadenar patrones.**
- **Pruebas:**
  - `ReanimaAudit arp`, 17/17:
    - notas exactas al sample (1/16 a 120 = cada 6000 muestras);
    - gate, swing, repetición, probabilidad (49,6 % con 50 %);
    - polirritmo, grados en Re mayor y transporte;
    - tempo propio, latch, sin notas colgadas;
    - patrón guardado y generado válido;
    - en el plugin: negras a −24,5 dB y −100 dB entre notas;
    - clip MIDI de 16 negras en 4 compases.
  - `ReanimaRtCheck`: 0 reservas de memoria con los 7 estilos, las 4 direcciones y repeticiones ×4.

## Fase 9: calidad de la resíntesis, motor C (punto 9, 2026-11-22)
- **Banco** (`ReanimaBatch resintesis <lista> <csv> [carpeta wav]`; script `research/resintesis_2026-11-22/comparar.py`):
  - 321 one-shots de 35 familias de la biblioteca (10 por familia, 0,3–6 s);
  - cada uno tocado a su altura y durante su duración por el motor A (con la envolvente detectada, el sample intacto) y por el motor C, comparados cada 20 ms donde el original está a menos de 40 dB de su pico;
  - medidas: peor caída y peor subida de nivel, error mediano, error espectral en 24 bandas, «ruidosidad» (planitud espectral, C frente al original) y ataque.
- **Corrección de la medición previa:** las caídas de −20 a −50 dB de la auditoría del 18-11 no eran del motor C. Ese banco forzaba una envolvente distinta de la del sample, y el motor A rellenaba el principio de los samples que empiezan casi en silencio.
- **Punto de partida:** el motor C ya era bueno en la mayoría de familias (error mediano 0,6 dB, error espectral 1,3 dB). Los fallos estaban en tres sitios:
  - el ruido puro (tetera: caída de −54 dB);
  - los ataques lentos (FX y barridos: hasta −25 dB);
  - 39 sonidos más ruidosos que el original (hasta +19 dB de planitud).
- **Arreglos, medidos uno a uno contra el punto de partida:**
  1. **Ataques lentos** (`SynthEngine::renderResynth`): con la envolvente detectada y sin aplanar ni sostener, los parciales y el ruido suenan al nivel medido, igual que el motor A con el sample intacto. Antes se dividía la envolvente por su propio modelo con un suelo de −60 dB que solo tenía el denominador, y en un ataque de 5,5 s con curva la nota quedaba muda medio segundo (−113 dB donde el sample está a −55). Resultado: 9 mejoran, 0 empeoran.
  2. **Tope de energía** (`analysePartials`): en cada fotograma, los parciales no pueden sumar más potencia que el sonido en la misma ventana. Resultado: 45 mejoran, 3 empeoran (poco).
  3. **Ruido medido también entre los parciales:** cada banda toma el menor de dos valores, el residuo («original menos parciales») o el original en los huecos entre parciales; como mucho 6 dB por debajo del residuo. Sin ese límite: 82 mejoran y 65 empeoran.
  4. **Descartado:** soltar los parciales cuya frecuencia salta al azar (separación tonal/ruido por continuidad). Resultado: 32 mejoran y 130 empeoran.
- **Resultado final frente al punto de partida:**
  - 106 sonidos mejoran y 34 empeoran (por poco: +4 puntos de error como mucho);
  - tetera: de −54 a −8 dB;
  - FX: peor caída mediana de −7,0 a −4,0 dB;
  - sonidos más ruidosos que el original (más de 3 dB): de 39 a 26;
  - mediana de planitud: de −0,6 a −1,0 dB (C apenas más limpio que el original);
  - el bombo ya no pierde su ruido.
- **Escucha A/B:** `release/ab_resintesis_2026-11-22`, 12 tríos (original, motor C antes, motor C ahora).
- **Sin hacer** (hipótesis de la auditoría sin demostrar): ventanas multirresolución y fases en las uniones. Las caídas que quedan (voz −4,5 dB de mediana) no justifican el riesgo sin una medida que las señale.

### Valoración del usuario de la fase 9 (2026-11-22)
- Tras la escucha A/B: «Sí, se queda». El nuevo motor C queda aprobado.

## Golpes antes de tiempo y acordes (2026-11-22)
### Golpes sobre la cola fuerte del anterior (hallazgo de la fase 2)
- **Causa** (`LoopDetect::onsets`): el inicio de cada golpe se buscaba retrocediendo desde su pico mientras el nivel siguiera por encima del 10 % del pico. Sobre la cola fuerte de la nota anterior, el nivel nunca baja de ese 10 %, y el retroceso atravesaba la cola.
- **Prueba nueva** (`ReanimaAudit golpes`, «Golpes sobre la cola fuerte del anterior»): 16 golpes (80 Hz, 300 Hz, 1,2 kHz y ruido) con colas lentas, en dos casos: colas a −9 dB al llegar el siguiente, y colas a −3,5 dB sumadas.
  - **Antes:** hasta 24,6 ms antes de tiempo.
  - **Ahora:** de −0,7 a +0,4 ms en los dos casos.
- **Regla nueva:**
  - la subida se cuenta desde lo que sonaba en los 20 ms anteriores al fotograma, no desde el silencio;
  - el recorrido se hace sobre el nivel y sobre la pendiente (la diferencia entre muestras): el ataque de un golpe aparece en la pendiente aunque la cola grave lo tape; se toma el inicio más temprano si el pico de la pendiente destaca;
  - el retroceso solo termina tras 10 ms por debajo del umbral, porque un tono grave pasa por cero cada medio ciclo;
  - si el golpe no destaca sobre lo anterior (pico menor que 1,15 veces la cola: un tono seguido de otro igual de fuerte), se aplica la regla de antes, que validan las pruebas de SLICE y EXPORTAR.
- **Sin cambiar:** el detector no encuentra un golpe de 80 Hz justo después de una ráfaga de ruido (2 de 16 en el caso realista). Ya pasaba antes; la prueba lo informa sin darlo por fallo.
- **Loops reales** (150 de la biblioteca, `ReanimaBatch loops`): 104 conservan el mismo número de cortes de Beats y 46 cambian (41 con menos, 5 con más). Los cambios son de un 4 % como mucho y casi todos en loops tonales (que no se trocean en Beats). No cambia el tempo ni el tipo de ningún loop.

### Tonalidad por acordes
- **Acordes por las notas** (`KeyDetect::chordFromNotes`): las teclas pulsadas, vistas desde el hilo de audio (máscara de 128 notas).
  - Tipos: triadas (mayor, menor, disminuida, aumentada, sus2, sus4), 7, maj7, m7, m7b5, dim7, 6, m6, add9, madd9, mmaj7, 9, maj9, m9 y quinta sola.
  - El bajo da las inversiones («Do/Mi»); en los acordes de cuatro notas la quinta puede faltar.
- **Acorde por el sonido de la entrada lateral** (`KeyDetect::chordFromChroma`, cromagrama del último medio segundo): triadas y las séptimas 7, maj7 y m7. Las sus se descartan, porque los armónicos las imitan.
  - **Medición** (`ReanimaBatch acordes`, 65 one-shots de Splice con el acorde en el nombre): tónica y mayor/menor bien el 65 % en total, y unas 7 de cada 10 veces con confianza ≥ 20 %. Muchos fallos son inherentes al sonido: Dom9 y Mi♭maj7 comparten casi todas las notas.
  - Por eso se muestra como sugerencia, con esa cifra.
- **Panel TONALIDAD:**
  - ACORDE: el tocado o, sin teclas, el oído; con su grado en la tonalidad en uso (vi7, V7, vii°, bVII);
  - PROGRESIÓN: los últimos 8, con sus grados; entra uno cuando se mantiene 0,2 s, y Borrar la vacía.
- **Pruebas** (`ReanimaAudit tonalidad`, acordes: 6/6): 12 acordes por notas, grados, La menor y Sol7 por el sonido, teclas y progresión Do–Lam–Fa–Sol = I–vi–IV–V, y Fa mayor por la entrada lateral.
