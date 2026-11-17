# BlackPanda Reanima — Documento de diseño

Sampler VST3 que trata un sample como si fuera un sonido sintetizado: analiza su
envolvente real (ataque, decay, sustain, release), coloca los mandos ADSR en esos
valores y reconstruye el sample cuando los cambias.

Proyecto personal de investigación (no comercial). "Clon" significa aquí replicar
**el comportamiento y el flujo de trabajo** de Serum 2 y del Auto Filter de Ableton,
no sus gráficos, código ni marcas.

## Idea central: reconstrucción relativa

1. Al cargar el sample se mide su envolvente real y se ajusta un modelo ADSR
   (ataque con curva, decay exponencial, nivel de sustain, caída del sustain en dB/s,
   release).
2. Los mandos ADSR se colocan en esos valores. **Sin tocar nada suena como el original.**
3. Al sonar, la ganancia aplicada es `ADSR_tuyo(t) / ADSR_detectado(posición leída)`.
   Así se conservan los matices naturales (vibrato, batidos) y sólo cambia la forma.
   La "posición leída" es la media ponderada (por la ventana de cada grano) de donde están
   leyendo los granos en ese momento, y se compensa la pérdida de nivel cuando los granos
   suman fuera de fase (estiramiento, spray). Así el nivel sigue tu envolvente también al estirar
   y no hay bombeo en los puntos de loop.
4. Si alargas el ataque o el decay con **Stretch** activado, el timbre de esa zona
   también se estira (lector granular coherente: con estiramiento 1:1 es transparente).
5. Cuando el sample llega a su **zona de sustain**, uno de tres modos lo mantiene vivo
   mientras la nota esté pulsada: **Loop**, **Granular** o **Freeze espectral**.

### Motores (se eligen en el plugin)

| Motor | Qué hace | Versión |
|---|---|---|
| **A · Reenvolvente** | Todo el sample pasa por la reconstrucción relativa. Ideal para pads, plucks y tonos. | v1 |
| **C · Híbrido** | El transitorio (golpe) suena intacto, sin estirar; el cuerpo se reconstruye. Ideal para pianos, bajos y baterías. | v1 |
| **B · Resíntesis espectral** | El sample se convierte en parciales + ruido; envolventes por parcial, morphing, formantes. | v2 |

## Comparativa de funciones y decisión

✅ = en el prototipo v0.1 · 🔜 = siguiente iteración · ❌ = descartado por ahora

### Productos que reconstruyen samples

| Función | Visto en | Reanima |
|---|---|---|
| Analizar un sample y convertirlo en parámetros editables | Synplant 2, Samplab Resynthesizer, Synclavier V | ✅ (ADSR detectado) |
| Resíntesis aditiva, envolvente por armónico | Alchemy, Harmor, Iris 2, Synclavier V | 🔜 motor B |
| Redibujar la evolución con envolvente multipunto + loop de sustain | Materia | ✅ loop · 🔜 envolvente dibujable |
| Spectral freeze | Alchemy, Iris 2, Falcon | ✅ |
| Estiramiento granular / time-stretch | Falcon (IRCAM Stretch), Kontakt (Time Machine), HALion (AudioWarp) | ✅ |
| Separación transitorio / cuerpo | Envolution, transient shapers | ✅ motor C |

### Samplers clásicos

| Función | Visto en | Reanima |
|---|---|---|
| Detección automática de nota raíz | Logic Sampler, Serato Sample, HALion | ✅ |
| Botón **Snap a C** (llevar el sample a Do, octava más cercana) | — (petición propia) | ✅ |
| Auto-loop con crossfade buscando puntos de loop | Logic Quick Sampler, Kontakt, Ableton Sampler | ✅ |
| Auto-trim del silencio inicial | Simpler, Quick Sampler | ✅ |
| Start offset modulable | Kontakt, Serum 2, Simpler | ✅ |
| Reverse | Todos | ✅ |
| Emulación vintage (bits, sample rate) | TAL-Sampler, Kontakt | ✅ |
| Round-robin / humanizar | Kontakt, Battery, XO | ✅ (variación aleatoria) |
| Arrastrar y soltar para importar | Todos | ✅ |
| Arrastrar para exportar el sonido renderizado | XO, Serato Sample | ✅ |
| Slicing | Simpler, Serato Sample, Serum 2 | 🔜 |
| Multisample / zonas / capas de velocidad | Kontakt, HALion, Sampler | ❌ (un solo sample, decidido) |
| Formant shift | Serum 2, HALion | 🔜 motor B |

### Clon de Serum 2 (funciones de sintetizador)

| Función | Serum 2 | Reanima v0.1 |
|---|---|---|
| Unison: voces, detune, blend, ancho estéreo | hasta 16 | ✅ hasta 8 |
| Polifonía | — | ✅ 32 voces |
| Mono / legato / portamento (glide) | ✅ | ✅ |
| Envolventes AHDSR con curva | 4 | ✅ Amp (reconstruida) + Env 2 |
| LFOs con sync, retrigger/libre | 10 dibujables | ✅ 3 con formas fijas · 🔜 dibujables |
| Matriz de modulación | arrastrar para asignar | ✅ 8 ranuras · 🔜 arrastrar |
| Macros | 8 | ✅ 4 |
| Rack de efectos | 13+ | ✅ Distorsión, Chorus, Delay, Reverb · 🔜 EQ, Compresor, Phaser, Flanger, Hyper/Dimension |
| Interfaz oscura con visualización grande | ✅ | ✅ |
| Arpegiador / clip sequencer | ✅ | 🔜 |

### Clon del Auto Filter de Ableton

| Función | Auto Filter | Reanima v0.1 |
|---|---|---|
| Tipos LP, HP, BP, Notch, Morph | ✅ | ✅ |
| Tipos DJ, Comb, Vowel | ✅ (Live 12) | ✅ |
| Pendiente 12 / 24 dB | ✅ | ✅ |
| Circuitos Clean, OSR, MS2, SMP, PRD | modelos analógicos | ✅ aproximaciones propias |
| Frecuencia, resonancia, drive, morph | ✅ | ✅ |
| Envelope follower (cantidad, ataque, release) | ✅ | ✅ (sigue la voz) |
| LFO del filtro con sync y formas | ✅ | ✅ (vía LFOs + matriz) |
| Envolvente ADSR de filtro y keytrack | — (sintetizador) | ✅ |

## Análisis al cargar un sample

1. Mezcla mono, envolvente RMS (ventana adaptada al tono).
2. Onset (−40 dB), pico, curva de ataque.
3. Ajuste por búsqueda en rejilla del modelo `S·10^(−fade·t/20) + (1−S)·e^(−5t/D)` en dB.
4. Nota raíz con YIN (mediana de varias ventanas); si no hay tono claro → "sin tono", raíz C3.
5. Zona de sustain: después del codo del decay, mientras la señal siga por encima de −30 dB.
6. Puntos de loop por autocorrelación (múltiplos del periodo) y paso por cero.
7. Buffer de freeze: espectro medio de la zona de sustain con fases aleatorias
   (IFFT larga → buffer periódico, loop perfecto).

## Snap a C

- Raíz detectada en Do → no cambia nada.
- Si no: el sample se afina **exactamente** al Do más cercano (corrige también los cents)
  y esa tecla Do pasa a ser la raíz. El export arrastrado queda en Do.
- Convención de Ableton: C3 = nota MIDI 60.

## v2 (implementado)

- **Motor B · Resíntesis**: análisis senos + ruido + transitorio (STFT Blackman-Harris, seguimiento de parciales
  hacia delante y atrás, residuo en 24 bandas). Cada parcial tiene su envolvente y su caída natural, así que la nota
  se puede prolongar más allá del final del sample (Natural) o sostener con respiración (Sostenido).
- **Timbre ligado a la envolvente**: arco/aliento (oscuro y con aire si el ataque es más lento que el original),
  release oscuro (agudos antes), velocidad → brillo, ruido de release. Por parcial en B; con un filtro de inclinación en A/C.
- **Fuentes de sustain mejoradas** (A/C): loop con la envolvente espectral igualada y cruce compensado por correlación;
  granular desde ese loop con granos sincronizados con el periodo; freeze aditivo periódico (sin cruce) con respiración.
- **Natural vs Sostenido**: Natural usa una región al final del sample y deja sonar el original hasta allí.
- **Motor A**: granos síncronos con el tono al estirar (sin "phasing").
- **Afinación**: comprobación de subfundamental (un C2 con armónicos fuertes de G ya no se detecta como G).
- **Rack FX estilo Serum 2**: Main (6 ranuras) + Bus 1 y Bus 2 (4 cada uno) con envío, nivel y "→ Main";
  Hyper/Dimension, Distorsión (16 modos, con oversampling y filtro pre/post), Flanger, Phaser, Chorus, Delay
  (Normal, Ping-Pong, Tap→Delay), Compresor (con multibanda tipo OTT), Reverb (Plate, Hall, Vintage, Nitrous, Basin),
  EQ, Filtro, Bode y Utility; splitters L/H, L/M/H y M/S. Los algoritmos son diseños propios que imitan el
  comportamiento, no los de Serum.

## v3: motor D · Reconstrucción

Los motores A, B y C sonaban poco realistas por razones medidas: subían la cola silenciosa del sample hasta +36 dB
(ruido, saturación), repetían granos de forma periódica (zumbido) y B reducía sonidos densos a parciales fijos casi mono.

- **Lecho**: al cargar el sample se generan dos continuaciones de 8 s en bucle sin costura (`Source/Reconstruct.cpp`):
  una con el cuerpo brillante tras el ataque y otra con una zona posterior más oscura.
- **Cómo se genera**: STFT estéreo de la región sin su caída de nivel. Los picos tonales conservan la coherencia de
  fase, con una pequeña desviación proporcional a su frecuencia; el resto del espectro recibe fases nuevas con
  magnitudes estables. Cada bin conserva su relación entre canales, y la región se recorre despacio.
- **Reproducción**: la voz lee audio con un sinc de 24 puntos que limita la banda al transponer. La ganancia es
  `ADSR_tuyo / ADSR_detectado`; a partir de +2 dB empieza a entrar el lecho y a +10 dB lo sustituye del todo. Con los
  mandos sin tocar sólo suena el sample (131 dB de SNR en la prueba).
- **Timbre**: los pasajes más silenciosos usan el lecho oscuro, así que un decay largo pierde agudos y un ataque lento se abre.
- **Salida**: limitador de seguridad en el máster, inactivo salvo si la señal supera 0 dBFS.
- **Descartado por medición**: Signalsmith Stretch a 1/8 de velocidad (9 dB de oscilación de nivel; rinde bien con
  estiramientos moderados) y el modo totalmente difuso tipo PaulStretch (2-4 dB de oscilación en graves).
- **Estéreo**: la diferencia de fase entre canales sólo se hace girar si gira de forma estable y al menos media vuelta
  dentro de la región; después se iguala el balance centro/lateral con el de esa región.
- **Prueba masiva**: `ReanimaBatch` pasa el motor por una carpeta entera y escribe un CSV por archivo. Con 1075 one shots
  no hubo cuelgues ni valores inválidos. Encontró la nota muda al acabarse samples cortados a nivel alto, ya corregida.
- **Limitaciones conocidas**: en percusión el sustain queda hasta unos 10 dB por debajo de lo pedido, porque un ruido
  sostenido necesita picos más altos que un golpe para sonar igual de fuerte y el lecho no supera los del sample.

### IA generativa (experimento, sin integrar)

Entorno en `ia/` (Python 3.10, PyTorch CUDA). Los pesos oficiales de Stable Audio 3 requieren licencia aceptada y sesión
de Hugging Face. Medium funciona en la RTX 3060 sin Flash Attention (8,7 GB de VRAM, unos 5 s por sonido).

- Continuar el sample desnudo con Small Music falla: huecos, cambios de nota y ritmo inventado.
- Continuar el render del motor D con Medium (`ia/gen3.py`) mantiene nota y timbre unos 7 s con un prompt genérico.
- `samples_prueba/comparacion` tiene el A/B de 14 sonidos. Si gana la IA, su audio sustituiría a los lechos.

## Alpha 0.4: auditoría

`ReanimaAudit` mide cada promesa a 44,1, 48, 88,2 y 96 kHz. Lo que encontró y cómo quedó:

- **Tiempo real**: los menús de los efectos se analizaban como texto en cada bloque. Ahora ninguna reserva de memoria
  en `process()`, con los 14 efectos activos y 40 notas.
- **Delays**: la posición se guardaba en coma flotante y perdía precisión a los 6 minutos de reproducción, a los 3 a
  96 kHz. Ahora va separada en parte entera y fracción, y el índice da la vuelta.
- **Interpolación**: el filtro se alarga con la transposición y su corte sigue al nuevo Nyquist, en lugar de dejar
  entrar imágenes. Las operaciones van con SSE.
- **Distorsión**: 8x de sobremuestreo (4x por encima de 60 kHz), corrector de continua en 5 Hz en lugar de un corte
  que quitaba graves, y la señal seca pasa por los mismos filtros para que la mezcla no haga peine.
- **Bode**: el retardo iba en la rama equivocada y desplazaba al revés.
- **Determinismo**: las mismas notas y ajustes dan siempre el mismo audio.
- **Lechos**: el equilibrio centro/lados se mide en la región tal como se oye, no aplanada, y la corrección es
  completa. Los picos pasan por un limitador con anticipación en la carga, sin coste de latencia.
- **Realce del original**: no sube por encima de la curva donde el sample ya es más fuerte que su modelo.
- **Limitador de salida**: a +6 dBFS, sólo contra desbocamientos. Antes, a 0 dBFS, comprimía acordes normales.
- **Parámetros**: Volumen, Pan, envíos, niveles y mezclas se mueven en rampa; el rack trabaja en bloques de 128
  muestras; los LFO libres aceptan modulación de velocidad.

### Inicio

`Inicio` es una fracción del archivo; al cargar se pone en el inicio detectado. La envolvente del usuario cuenta
desde la nota, así que un inicio tardío conserva el ataque elegido. El motor D aplica la relación entre las dos curvas
al nivel real del punto de partida, para que el lecho entre al nivel correcto. Los proyectos anteriores guardaban una
fracción de la región sonora: `stateVersion` permite convertirlos al abrirlos.

### Release y fase

Con la envolvente cambiada, el motor D puede estar sosteniendo algo que el sample nunca tuvo, y el release detectado
puede ser el de un golpe. Por eso:

- La caída empieza suave, con una rampa de 4 a 30 ms. Una caída que arranca a toda velocidad se oye como un golpe.
- Una nota remodelada nunca cae en menos de 60 ms. Sin tocar la envolvente, el release es el del mando.
- Tras soltar, la mezcla se congela. Si el sample se acaba, el lecho entra al nivel que tenga el original en ese momento:
  una nota soltada no vuelve a subir.
- Cada lecho se lee en el punto donde sus ondas coinciden con las del sample, calculado al cargar por correlación.
  Avanzan a la misma velocidad, así que siguen juntos. Antes la posición era aleatoria en cada nota, y los tonos graves
  se cancelaban una cantidad distinta cada vez. El fundido usa esa correlación: igual potencia para sonidos sin
  relación, menos a medida que coinciden.
- Unison sin desafinar suma como una sola voz: mismo nivel y sin filtro de peine.

## Alpha 0.5

- **Detección de nota**: YIN a lo largo de toda la nota, con el difference function por FFT. Gana el tono que más
  suena, pesado por energía, para que el glide de un 808 o la caída de un bombo no decidan. Después se prueban como
  fundamental el resultado de YIN, los picos más fuertes y sus submúltiplos, puntuando qué parte del espectro explican
  sus 12 primeros armónicos (por amplitud). De los que explican casi lo mismo gana el más agudo. Así aparece la
  fundamental aunque el sample no la tenga. Con nota en el nombre del archivo y sin efectos, voces ni acordes,
  acierta 81 de 97 (falla sobre todo en bombos y 808 con el tono cayendo).
- **Snap a C**: apagado, Do3 toca el sample sin transportar. Encendido, se mueve al Do más cercano a su nota.
- **ADSR Sintetizador** (motor D): se divide por el nivel medido del sample y se multiplica por la ADSR. Donde el
  sample no llega o se acaba entra el lecho, y también tras soltar la nota.
- **Golpe**: con un ataque más rápido que el del sample, una ráfaga de los agudos del lecho brillante (paso alto a
  1,5 kHz) que cae en 12 ms, proporcional a cuántas octavas de ataque faltan.
- **Motor E**: nube de hasta 64 granos por voz con ventana Hann, normalizada en potencia por el solapamiento.
- **Edición**: Fin, fundidos y bucle con fundido cruzado de igual potencia (motor D), ganancia, normalizar y
  envolvente de pitch exponencial para todos los motores.

## Alpha 0.6

- **Motores renombrados**: A Reconstrucción, B Reenvolvente, C Resíntesis, D Híbrido, E Granular, F Drone Maker. Los
  proyectos anteriores (estado < 3) se traducen al abrirlos y conservan su motor.
- **Motor F · Drone Maker**: idea de PaulStretch (Nasca Octavian Paul) y de las funciones de PaulXStretch, escrita de
  cero (PaulXStretch es GPLv3; no se usa su código). Tramos con ventana Hann al 75 % de solape y fases al azar, de
  2048 a 65536 muestras a 48 kHz. Los dos canales van en una sola FFT compleja (izquierdo real, derecho imaginario)
  y se separan por simetría. Cinco etapas por tramo (leer, FFT, tratar, fases, FFT inversa) que el motor reparte
  entre bloques por urgencia. Tratamiento sobre magnitudes: armónicos (campanas gaussianas en cents alrededor de
  cada múltiplo de la fundamental del sample), tono/ruido (sobre o bajo la media local del espectro), spread
  (media en anchura proporcional a la frecuencia), copias a otras proporciones (afinación, intervalos y voces de
  Unison, con 1/√r para conservar la energía), desplazamiento en Hz, y el nivel se devuelve al del sample antes del
  filtro dibujado (16 puntos de 20 Hz a 20 kHz, de -48 a +12 dB). Ganancia de salida 1/√(0,375 · 1,5).
- **Mecanismo**: 461 golpes grabados (CC0: VCSL, VSCO-2-CE, Growlybass; `Resources/Mechanism/CREDITOS.txt`), de los
  que se quitó la nota con HPSS (filtros de mediana en el espectrograma: 41 tramas en tiempo, 11 a 17 bandas en
  frecuencia; la máscara se calcula sobre L+R y se aplica igual a los dos canales, así que no cambia la imagen) y se
  recortaron al ruido. Lados limitados a 3 dB bajo el centro (correlación L/R ≥ 0,33: nada se anula en mono). FLAC
  16 bit 48 kHz dentro del binario (juce_add_binary_data, 5,7 MB), decodificados una vez en `SynthEngine::prepare`.
  Cada golpe se normaliza a 0 dB RMS en sus 50 ms más fuertes (sin subir más de 6 dB sobre la mediana de su grupo).
  Todos los golpes de pulsar y soltar llevan algo de la nota en que se grabaron (cuerda que se apaga, final de un
  golpe de arco o de un soplo): cada nota toma el golpe cuya nota es la suya en cualquier octava (coste 24 por
  semitono de clase, 1/12 por semitono de registro), lo mueve como mucho ±1,5 st (Hermite de 4 puntos), capa de
  fuerza por velocidad, uno al azar sin repetir el anterior, ±0,5 dB. Solo el pedal suena tal cual. Las notas de
  cada librería se comprobaron en el audio (mechpitch.py, mechlabels.py: 0 errores en viento y órgano): VSCO nombra
  una octava abajo, Growlybass es la parte escrita (suena una octava abajo); un error de octava no importa al plegar.
  Nivel: mando² · RMS de pico del sample · 0,35 (tecla) o 0,28 (soltar) · (0,45 + 0,55 · velocidad); 0,22 el pedal.
  En Púa, Arco y Soplo, el golpe de soltar de una nota corta baja (30 % a 30 ms, entero a 250 ms). El pedal (CC64,
  solo Piano) va en el motor, no en la voz. *Del sample* usa el propio ataque, a la altura de la nota. La voz sigue
  viva mientras suena.
  **Integración** (0.9, `Mechanism.cpp`): el sample se describe al cargarlo (`SampleData::Excitation`: ataque,
  golpe propio = RMS 0-15 ms contra 60-250 ms, cuánto se sostiene, planitud espectral, parte armónica, centroide,
  registro grave al 3 % de la energía, instante y polaridad del golpe = primer ascenso a la mitad de la envolvente de
  1 ms, color del ataque en 7 bandas). Cada grabación guarda el instante y la polaridad de su golpe (misma regla) y
  cada grupo su color medio. Al tocar: el golpe de la grabación cae en el del sample (espera o salta dentro de la
  grabación, con 1 ms de fundido), misma polaridad en los gestos de golpe si el sample tiene golpe propio (>3 dB),
  paso alto en 0,7·f0 de la nota (Púa 0,85·f0 y 36 dB/oct; Piano sin corte; sin nota, 0,8 del registro grave),
  ecualizador de 7 bandas hacia el color del ataque del sample (Púa 85 %, Arco 65 %, Soplo 40 %, Tecla 35 %, Piano
  20 %, por Adaptar), conservando la potencia de la grabación; un peine de realimentación en la nota que suena
  (T60 de 0,05 a 0,12 s, mezcla de 10 % a 55 % por Adaptar, normalizado en potencia, paso alto en 0,6·f0 porque un
  peine también resuena en 0 Hz). Un ataque lento (25 a 200 ms) suaviza y abre los golpes (-65 %, fundido de hasta
  30 ms) y hace crecer arco y soplo (fundido de la mitad del ataque); si el sample golpea fuerte, la púa y la tecla
  bajan hasta un 30 %. Arco y Soplo siguen con granos de 64 ms (ventana de Hann, solape 4) de sus grabaciones, con
  nivel = mando² · 0,18 (arco) o 0,4 (soplo) · RMS de pico · (nivel de la nota / RMS de pico)^0,8. Compatibilidad
  (`Mechanism::fit`): reglas graduales, no exclusiones (rápido = 12-90 ms, suave = 15-150 ms, golpe propio,
  sostenido, tono, ruido, registro), con el ataque del mando Ataque y Sostenido/Sustain; el menú lleva el porcentaje
  y » el mejor. On/Off es el parámetro `mechOn` (0.9.1; antes un A/B sin guardar). Entrada y Salida (`mechDown`,
  `mechUp`, -60 a +12 dB) multiplican los golpes de pulsar (y pedal pisado) y de soltar (y pedal soltado); el roce
  continuo de arco y soplo sigue solo a Mecanismo.
- **Sintetizador** (A a D): Aplanar al 100 % y nota sostenida; el cambio de modo pone valores de sinte (o los
  detectados al volver), salvo con el candado cerrado.
- **Edición destructiva**: sobre el sample tal como se ve (invertido o no), con 30 pasos de deshacer. El audio editado
  viaja en el estado como FLAC de 24 bit en base64, escalado si pasa de 0 dBFS.
- **Presets de fábrica**: plantillas en código (FactoryPresets.cpp) que restablecen todo menos lo que es del sample
  y de la mezcla.

## Alpha 0.7

- **Persistencia**: el estado lleva siempre el sample (FLAC 24 bit en base64, escalado si pasa de 0 dBFS), su nombre
  y si está editado. Se codifica en el hilo de análisis al cargar y se decodifica allí al abrir: guardar y abrir
  no bloquean. Si la copia falla se usa la ruta; si tampoco existe, se avisa y el proyecto sigue.
- **Historial**: pasos con la foto de parámetros antes y después (más la curva del filtro dibujado y, si cambió, el
  audio). Se graba un paso cuando hubo un gesto de parámetro (o un cambio de la interfaz sin gesto) y pasan
  250 ms sin tocar; la automatización sólo mueve el punto de partida. Deshacer devuelve sólo lo que cambió el paso.
  Los cambios que hace el análisis de un sample nuevo se suman al paso de ese sample. 100 pasos.
- **Motores B, C y D**: el realce para seguir la ADSR ya no supera la curva donde el sample es más fuerte que su
  modelo (como en A): límite por el nivel medido, y en la fuente de sustain por el nivel real del bucle. Las fuentes
  de sustain no tienen DC. Medido sobre 90 one shots en Sostenido con S 70 %: error de nivel de -4,7..+7,2 dB a
  -6,4..+0,2 dB; picos sobre +1 dBFS de 63 a 32; DC de 22 % de archivos a 11 %.
- **Motores E y F**: suenan al nivel de la parte más fuerte del sample allí donde leen (hasta +12 dB): Posición
  cambia el color, no el volumen. Error de nivel mediano de -15/-11 dB a -6/-2 dB.
- **macOS**: binario universal arm64 + x86_64, VST3 y AU, camino NEON en la interpolación. Sin probar en un Mac.

## Alpha 0.7.1 (416 one shots nuevos)

- **Material**: los nuevos son tonales (69 808, 32 log drums, 90 bajos, metales, guitarras, teclas, plucks, acordes),
  donde los anteriores eran sobre todo percusión. Informe por archivo en `research/muestras_nuevas_2026-11-08.csv`.
- **Nota**: 808 95 %, bajos 91 %, plucks, metales y guitarras 100 % contra la nota del nombre. Casi todos los fallos son
  etiquetas que no son la nota (una FFT independiente da lo mismo que Reanima: los SS_3SE suenan 1,33 st por encima de
  su nombre), acordes, frases o sonidos inarmónicos.
- **Ataque**: el pico es la primera llegada a 1 dB del máximo, medida sobre los picos de la onda con ventana de un
  periodo (la RMS de tres periodos emborrona el arranque de un grave). 808: 53 → 7 ms; bajos 65 → 25; voces 163 → 46.
  El golpe conservado (Transitorio) dura el ataque más 15 ms o tres periodos, lo que sea mayor.
- **E y F**: techo suave en el pico del sample (rodilla al 80 %): picos sobre +1 dBFS en los 416 de 178/256 a 6/6.

## 0.8.0 RC1 (533 muestras más)

- **Material**: teclas (casi todas loops de acordes), metales, vientos de madera (flautas, quena, bansuri, shakuhachi,
  duduk), arpas, bombos y percusión afinados, tonos Synare. Sin duplicados (SHA256), ninguno ilegible. Informe por
  archivo en `research/muestras_nuevas_2026-11-08_tanda2.csv`.
- **Nota**: los bombos «afinados» dan el tono en el que se asienta su caída (comprobado con FFT); los nombres llevan la
  tonalidad del tema. Probado y descartado: leer la nota a lo largo de todo el sample (arregla una flauta con eco, pero
  352 → 350 aciertos de 448 y un 13 % más de análisis).
- **Varios eventos**: si antes del momento más fuerte el nivel cae a la mitad (un loop, una frase, notas con eco), el
  ataque es el del primer evento que llega a 6 dB del máximo. Un loop de piano de 34 s pasaba a tener 20 s de ataque y
  el motor C, con Stretch, comprimía esos 20 s en 300 ms y se quedaba sin sonido.
- **Posición (E, F)**: se mide sobre la parte que suena (inicio detectado a final útil). Muchos one shots tienen
  segundos de silencio digital al final, y al 30 % del archivo el motor E no sonaba.
- **Resultados en los 533**: A, picos sobre +1 dBFS 83 → 51 y nivel inestable 87 → 46; C, colas que no terminan 2 → 0;
  E, peor 5 % de nivel -23,9 → -12,1 dB.

## 0.8.1 (Mecanismo grabado)

- **Problema**: el Mecanismo era ruido filtrado sintético; no se parecía a ningún instrumento y la Púa hacía una
  ráfaga rugosa de medio segundo al soltar («roce de traste») que no sonaba a púa.
- **Ahora**: 235 golpes de grabaciones CC0 (409 archivos descargados, 112 MB, en `research/mecanismo_fuentes/`, fuera
  del binario), sin nota (HPSS) y recortados. Piano: tecla y macillo (Steinway B, capas 3 y 4), apagador (capas 2 a
  4), pedal derecho (piano vertical) con CC64. Púa: ataque y dedos al parar (bajo eléctrico). Arco: mordida y salida
  (violín y chelo spiccato). Tecla: contacto de órgano y martinete de clave.
- **Niveles** (mando 0,6, pluck de prueba): tecla -11 a -19 dB bajo el ataque, soltar -17 a -24 dB, pedal -24 dB.
  Con el mando a 0 no suena nada, y el pedal solo suena con Piano.
- **Escucha**: `ReanimaBatch mecanismo <wav> <salida.wav> [cantidad]` toca una frase sin Mecanismo y con cada tipo.

## 0.8.2 (cola continua, Mecanismo natural)

- **Cola del motor A con un decay largo**: subía de golpe al acabarse el sample. Dos causas. (1) Los lechos brillante
  y oscuro se hacían por separado: llevan los mismos tonos con fases que derivan y, mientras suenan los dos (un sustain
  bajo, una caída larga), se anulaban un momento cada segundo o así (hasta -20 dB) y volvían. Ahora el oscuro es el
  brillante recoloreado con un filtro de fase cero (espectro medio del final del sample, suavizado a 1/6 de octava,
  -48 a +12 dB) aplicado alrededor del bucle: mismas ondas, solo cambia el color. (2) El lecho solo entraba cuando el
  modelo decía que hacía falta o cuando el sample se acababa; si el sample real cae antes que su ajuste, el original
  (subido como mucho 12 dB) se quedaba corto y el lecho entraba de golpe al nivel de la curva. Ahora también entra en
  cuanto el sample real no llega (0,15 a 1,15 octavas de falta). El dibujo de RESULTADO hace el mismo relevo.
- **Mecanismo**: las notas de la Púa estaban todas en Do 60 (nombres en minúscula y con bemol) y las del Arco una
  octava abajo: se reafinaban hasta 12 semitonos. Además los golpes de soltar no se afinaban y llevaban la nota de
  su grabación: medido con un tono de Do sostenido, al soltar el Arco quedaba a 4,7 st de mediana, la Púa a 3,6 y el
  Soplo a 2,6. Ahora todo golpe de pulsar o soltar toma la grabación de su misma clase de nota (±1,5 st como mucho);
  la variación entre golpes baja (±0,5 dB, 7 dB de la nota más suave a la más fuerte) y el soltar de una nota corta
  es más flojo. *Del sample* no seguía la tecla (+4 st sonaba a la altura del sample); ahora sí. Nuevo tipo
  **Soplo** (flauta, clarinete, saxo tenor; 146 golpes); el estado pasa a la versión 4 (Del sample, 4 → 5).
  Nivel al máximo -9 dB (antes -6).

## 0.9.0 (articulaciones inteligentes)

- **Medido antes de cambiar nada** (11 samples: plucks, madera, bajo pizzicato, piano, arpa, flautas, pad, metal,
  reese; mando a 0,6; `research/mecanismo_scripts/integration.py`): la Púa llegaba 3 a 18 ms antes que el golpe del
  sample (dos ataques), con un color a 15 dB del suyo y sobresaliendo +18 dB en su banda; Arco y Soplo no existían
  después de 150 ms (por eso el soplo «no actúa» en sonidos sostenidos) y en un metal brillante el soplo quedaba
  13 dB tapado.
- **Referencias**: síntesis conmutada / fuente-filtro (J. O. Smith, CCRMA: la excitación toma la resonancia del
  cuerpo); capas de samples (iZotope, Attack Magazine: alinear transitorios, polaridad); ruido de viento ligado a la
  presión (STK Flute, Cook/McGill ICMC 96, SWAM: ruido de aliento continuo, más presente en pianissimo); nivel de los
  ruidos de soltar según duración y fuerza (Tascam GVI, Polyphone, Spectrasonics); EQ match con proporción
  (SpectraLayers); descriptores de timbre (Peeters et al., Timbre Toolbox, JASA 2011). Nada de código ajeno.
- **Resultado** (mediana en las 11 familias, antes → ahora): desfase del golpe Púa 8 → 1,5 ms, Tecla 12,5 → 1 ms,
  Arco 11,5 → 6,5 ms, Soplo 9 → 4 ms; color Púa 15,5 → 11,4 dB, Arco 16,7 → 11,7, Soplo 15,9 → 12; energía una
  octava bajo la nota, Púa -23 → -32 dB (en el pluck de prueba -53 dB); lo que sobresale la Púa en su banda +18 →
  +4,6 dB; Arco y Soplo durante la nota: nada → -5 / -7 dB bajo el sample en su mejor banda; ataques duplicados 0;
  Piano conserva su golpe (+11 frente a +13,5 dB) y su sordo; sin clics nuevos. Coste: 32 voces con soplo continuo,
  11 % de un núcleo más.
- **Panel**: menú con porcentaje por gesto y » el recomendado, línea de consejo (por qué no encaja el elegido), mando
  *Adaptar* (nuevo parámetro, `mechAdapt`, 75 %) y botón *A/B*.

## 0.9.1 (margen de salida, Entrada/Salida, On/Off)

- **Volumen de serie -12 dB** (antes 0): un sample normalizado ya llega a 0 dBFS con una nota, y dos o tres juntas
  pasaban de ahí. Con -12 dB un acorde de 4 notas queda 2,7 dB bajo el pico del propio sample (prueba en
  EngineTests); el limitador interno sigue solo como protección a +6 dBFS. Los proyectos guardados conservan su
  volumen. Las herramientas de medida (tests, Audit, Batch, Demos, Release) usan 0 dB: miden el motor.
- **Entrada / Salida**: volumen separado del golpe al pulsar y al soltar. **On/Off** sustituye al A/B y se guarda.
- **TIMBRE**: fila 1 Brillo, Arco/aliento, Apagado, Bits, Muestreo (nombres cortos en el panel); fila 2 Mecanismo,
  Entrada, Salida, Adaptar; Parciales y Ruido (C) pasan a MOTOR. Guía rápida y ayudas al día.
- **Prioridades**: el análisis (ThreadPool `loader`) y el segundo lecho (`Thread::launch`) van a prioridad baja. La
  prueba larga (`ReanimaHostCheck ... resistencia`) toca ahora en un hilo TIME_CRITICAL, como un DAW: antes tocaba a
  prioridad normal y el análisis del sample siguiente le quitaba tiempo (picos de 119 a 530 % en bloques sin
  ninguna nota nueva, en sitios distintos cada vez). Ahora: 400 min, peor bloque 36 %, memoria ~350 MB estable,
  pico -9 dBFS. La prueba lista cada bloque lento con su sample, motor y mandos movidos.
- **AddressSanitizer**: sin errores en el plugin; sí encontró una lectura fuera de rango en una prueba nueva (nota
  de 1,0 s comparada con una de 1,2 s), corregida.

## 0.9.2 (fase 0 del diagnóstico: crítica)

- **Picos de B, C y D.** Medido en 506 samples (`ReanimaBatch motores`, Sustain 70 %, nota sostenida): B, C y D
  pasaban el pico del propio sample en más de 3 dB en 94-120 casos (hasta +9 dB, tope del limitador de
  protección). Causa: el cuerpo de un golpe seco se sube al nivel medio que pide la envolvente, y el ruido tiene
  picos muy por encima de su media. Ahora todos los motores tienen el techo suave en el pico del sample que ya
  tenían E y F, pero con la rodilla justo en ese pico (todo lo que no lo pase sale bit a bit igual) y como mucho
  2,5 dB por encima. Resultado: A 14 → 0, B 95 → 14, C 120 → 0, D 94 → 14 casos; el nivel sostenido no cambia
  (mediana de error idéntica). Prueba nueva en EngineTests.
- **Aliasing del filtro de voz.** Medido (`ReanimaBatch aliasing`, tono de 6,2 kHz a -6 dBFS, corte 18 kHz): con
  24 dB de Drive, los circuitos OSR y MS2 devolvían -3 dB de energía plegada (casi tan fuerte como el tono), y
  SMP/PRD -15 a -17 dB. Los circuitos que saturan (OSR, MS2, SMP, PRD; no Comb ni Vowel) corren ahora a 4x con
  dos etapas de semibanda IIR polifásica (diseño HIIR de Laurent de Soras, dominio público; `HalfBand.h`):
  10 coeficientes con transición de 0,042 y 6 con 0,1. Peor caso -3 → -39 dB; con 12 dB de Drive, -15 → -50 dB.
  El tono cambia menos de 0,1 dB por debajo de 3 kHz y hasta ±1,7 dB a 6 kHz con el corte en 18 kHz (a 4x la
  respuesta se acerca a la analógica). Coste: 32 voces del motor A con el filtro OSR, de 11,5 % a 33,8 % de un
  núcleo; Clean (y el filtro apagado, por defecto) no cambia. Prueba nueva en EngineTests.
- **A/B con nivel igualado** (cabecera: A, B, = nivel). Dos ajustes completos (no el sample); la primera vez B
  empieza igual que A; cambiar de uno a otro es un paso de Deshacer. La sonoridad de cada uno se sigue con
  ponderación K (BS.1770, media de unos 3 s, solo con señal sobre -70 dB) y el más fuerte se baja al nivel del
  otro, nunca se sube. Prueba en Snapshot: B 7,1 dB más fuerte queda a 0,4 dB. No se guarda en el proyecto.

## 0.9.3 (fase 1 del diagnóstico: Warp)

Pedido: tocar lejos de la nota del sample sin el efecto «chipmunk», como una opción Warp de Simpler, dentro de
todos los motores (no un motor nuevo). Parámetro `warpMode`: Re-Pitch (por defecto, bit a bit igual que antes),
Tonos y Textura.

- **Lector (`WarpReader`, SynthEngine.cpp).** El tiempo avanza al paso propio del sample y el tono lo pone el
  lector. *Tonos*: PSOLA en el dominio del tiempo; granos Hann de dos periodos locales centrados en marcas de
  periodo (`SampleAnalysis::findMarks`: autocorrelación cada dos periodos, ancladas tras el pico), leídos a la
  velocidad original (formantes en su sitio) y colocados cada periodo/ratio. *Textura*: granos de 60 ms leídos a
  la nueva velocidad, cada 15 ms; con un sample con nota, cada grano empieza a un número entero de periodos de
  donde lee el anterior (como WSOLA), si no, un tono puro se anula a sí mismo (medido: duración ×0,43 y -5 st
  antes del arreglo). El centro de cada grano lee el instante en que suena (antes iba 30 ms tarde y a +12 los
  primeros granos leían silencio). El ataque (hasta el pico, 5-30 ms) se lee entero como Re-Pitch y pasa a los
  granos con un fundido de igual potencia en 10 ms, cuando éstos ya lo han dejado atrás (máximo 50 ms). El nivel
  se iguala a la potencia del original por grupo de canales (20 ms, ±18 dB, la corrección se mueve en 40 ms y
  sólo cuando los granos han dejado el ataque): al conservar formantes la fundamental cae en una zona más débil
  (una flauta +12 pierde 9 dB). Sin latencia, sin memoria extra.
- **Motores.** A: el original y los dos lechos leídos por los mismos granos. B y D: los granos del cuerpo, la
  fuente de sustain (Loop y Freeze; Granular ya es independiente) y el transitorio. C: el tiempo de los marcos al
  paso propio; con Tonos cada parcial toma el nivel que la envolvente del marco (trazada sólo por los armónicos,
  en log, 36 dB/oct por encima del último) tiene en su nueva frecuencia, con la potencia del marco conservada; el
  ruido no se desplaza. E: la nube recorre el sample a su velocidad propia. F: ya era independiente del tiempo;
  con Tonos las copias desplazan la estructura fina (espectro / envolvente de un armónico de ancho, plana bajo la
  fundamental) y la envolvente queda, con +18 dB como mucho y nada bajo la parte más grave del sonido.
  Sin nota clara, Tonos actúa como Textura en todos.
- **Medido** (`warpcheck.py`, ±12 st, piano, flauta y pluck reales, motores A, C y D; duración = caída de 20 dB
  frente a la nota tocada en su tono): Re-Pitch ×0,52 a ×2,0; Tonos ×0,94 a ×1,06 (pluck de 0,19 s a +12:
  ×0,75-0,79); Textura ×0,98 a ×1,05. Tono a menos de 0,1 st. Distancia de formantes a +12 (dB, menos es mejor):
  piano 17,5 → 11,6, flauta 19,2 → 9,1, pluck 19,0 → 8,2 (motor A); F: piano 18,8 → 13,9, reese 10,5 → 4,5.
  Al bajar más de media octava Tonos suena pulsado (los granos quedan separados): es lo propio de conservar los
  formantes.
- **Pruebas nuevas** (EngineTests): en A-D, Tonos y Textura a ±12 afinados (±0,3 st), con la duración de la nota
  en su tono (×0,75-1,3), el mismo nivel (±3 dB), bajo el techo, y Tonos con los formantes más cerca que
  Re-Pitch; Re-Pitch idéntico a antes muestra a muestra. CPU de 16 voces: Tonos +1 a +9 %, Textura +0 a +18 % de
  un núcleo (A es el más caro: seis canales).
- **Interfaz.** Menú Warp en la primera fila de MOTOR (en F, junto a Tamaño FFT); ayuda y guía (paso 3).

## 0.9.4 (P1a de la auditoría de la biblioteca: Tonos fiable)

La auditoría de la biblioteca (`docs/auditoria_biblioteca_2026-11-13.md`) midió que Tonos (PSOLA) da la nota
equivocada en mucho material real: a +12, el 53 % de 176 one shots tonales en la nota correcta; a +7, el 80 %.
PSOLA convierte todo en un tono periódico de la nueva nota: un acorde, voces desafinadas o ciclos que no se
repiten iguales salen a la altura original, una octava abajo o con sus notas fundidas en una.

- **Autoprueba al cargar** (`SynthEngine::checkTones`, en el hilo de análisis). Tonos toca el sample a +7 y +12
  (solo el original, como en los motores B y D) y los primeros 0,6 s de cada nota (desde 30 ms) se comparan con
  la misma nota en Re-Pitch, cuyos armónicos son los correctos. Dos formas de fallar, las dos medidas: energía
  fuera de los armónicos de la nota (la prueba de rejilla de la auditoría: los 15 picos más fuertes, a 30 dB del
  máximo, frente a los 30 de la referencia a 50 dB, a menos de 2,7 Hz o 1,2 %) y armónicos de la nota que no
  suenan (los 15 de la referencia frente a los 80 de Tonos: las otras notas de un acorde fundidas en una). Cuenta
  la peor de las cuatro cifras; por debajo de 0,7 (`tonesPass`), `tonesHold` es falso y en A, B y D Tonos toca
  como Textura (el lector recibe `cycles = false`; `WarpPlan::cycles`). C (envolvente espectral) y F (copias
  espectrales) transportan bien por su cuenta y no cambian; su transitorio en C sí sigue la prueba. Coste: 14 ms
  más de análisis de mediana (p90 37 ms) sobre unos 825 ms; nada al tocar.
- **Ciclo frente a nota** (`SampleData::cycle`, `cyclePeriod` en SampleAnalysis). Las marcas de Tonos se ponen
  cada ciclo de la onda, que se comprueba contra la mitad, el tercio y el cuarto del periodo de la nota en los
  primeros 0,5 s tras el pico (correlación ≥ 0,9 y no peor que la del periodo entero). La nota no cambia. En la
  biblioteca no se activa en ningún sample: los casos que la auditoría atribuyó a la octava eran el sesgo de la
  medida de solapamiento, y el bajo de la auditoría (E0/E1) ya pasa la autoprueba.
- **Menú.** Con un sample que no pasa, el elemento Tonos del menú Warp de A-E dice «Tonos → Textura» en A, B y D.
- **Medido** (`warpfail.py` de la auditoría, motor A, 176 one shots de 25 familias; nota correcta = rejilla ≥ 0,5):
  +12, 53,4 % → 95,5 %; +7, 80,1 % → 98,3 %. Pasan a Textura 110 de 176 (acordes, stabs, arpegios, percusión con
  tono, voces habladas); en 80 «monofónicos» de la auditoría, 31 (casi todos acordes, rasgueos y arpegios mal
  nombrados). Lo mejor posible eligiendo por sample entre Tonos y Textura con la medida de la auditoría era
  97,7 % / 99,4 %. Los 8 fallos que quedan a +12 son percusión y campanas, que también fallan en Textura.
  Donde Tonos se mantiene suena igual que en la 0.9.3, muestra a muestra; distancia de formantes (47 one shots,
  `formcheck.py`): +12 8,1 dB (Re-Pitch 16,1, Signalsmith 12,9), -12 8,8 dB (14,5, 10,1).
- **Pruebas nuevas** (EngineTests): un pluck pasa la autoprueba; un acorde de tres notas no, y en A, B y D Tonos
  suena idéntico a Textura; el aviso solo sale donde ocurre. `ReanimaBatch tonos <lista> <csv>` da la prueba por
  archivo.

## 0.9.5 (en curso, sin validar: Mecanismo con notas reales)

- **Banco de pruebas contra instrumentos reales** (`research/referencias_2026-11-13/scripts/mechbench.py`, 47 notas
  de Iowa y VSCO: piano, arpa, pizzicato, violín, violonchelo, vientos). HPSS separa cada nota en H (armónica) y P
  (macillo, púa, mordida, lengua y aire); Reanima recibe H y lo que añade Mecanismo se compara con el P real
  (desfase del primer pico, color en tercios de octava, nivel, forma de la envolvente).
- **El golpe llegaba tarde.** En las notas reales el pico de P llega 6-9 ms antes que el de H en golpes y 35-45 ms
  en arco y soplo, donde la nota empieza a subir; Mecanismo lo ponía en el primer pico del sample. Ahora cae donde
  el ataque del sample alcanza una décima de su altura (`Excitation::riseAt`) más 2 ms, o en su primer pico si llega
  antes; no depende de Adaptar. Arco y soplo ya no entran con un fundido de la mitad del ataque (5 ms como mucho).
  Desfase: Piano 6 → 0 ms, Púa 8 → -1, Arco 18 → 0, Soplo 19 → -3,5; forma del golpe de Piano 0 → 0,41.
- **Piano real** (`Mechanism::RealPiano`, al final del menú: los proyectos guardados no cambian): 170 golpes de
  macillo sacados de notas reales (Iowa en pp/mf/ff y piano vertical de VSCO, 4,5 MB), con el apagador y el pedal de
  Piano. En el banco (sin las notas medidas): color tras el golpe 47,6 → 10,2 dB, forma 0,41 → 0,51, nivel -13 → -9
  dB frente al macillo real. Escucha del usuario: mejor con pianos, Rhodes, Wurlitzer y clavinet; Piano sigue mejor
  con sintes, plucks y leads.
- **Púa real, Arco real, Soplo real** (`RealPick`, `RealBow`, `RealBreath`, al final del menú): el mismo gesto con
  ataques sacados de notas reales: arpa y pizzicato de violín y violonchelo (34 golpes), violín y violonchelo con
  arco (61), flauta, clarinete, trompa, oboe, fagot y trompeta (57); 6,1 MB. Separación HPSS con margen β = 2
  (Driedger, Müller y Disch 2014) y selección: fuera los que conservan su nota (prominencia armónica > +3 dB) o casi
  no destacan de ella (< -35 dB); como mucho 6 por clase de nota, de notas distintas. Púa real se trata como el piano
  (sin paso alto, poco color: con el de Púa perdía lo que la hace un punteo). Arco y Soplo real tocan solo la
  mordida o la lengua (120 ms tras su pico, 40 de fundido): el resto de la grabación es el roce de su propia nota,
  que no sigue a esta y se cortaba de golpe a los 0,5 s; el roce continuo, que sigue a la nota, toma el relevo.
  En el banco, sin las notas medidas, frente al gesto clásico: Púa real forma 0,34 → 0,59, color tras el golpe 16,6
  → 12,5 dB; Soplo real nivel +9,5 → +4,6 dB sobre el aire real, color 12,2 → 10,4; Arco real color tras el golpe
  14,9 → 10,4 pero forma 0,49 → 0,31. Son opciones para elegir de oído.
- **Medido y no adoptado:** quitar la cadena a Piano (sin efecto); HPSS más estricto para el piano (sin efecto).
- **Segunda tanda** (informe completo en `docs/mecanismo_informe_2026-11-13.md`): "Púa real" pasa a llamarse
  **Pulsado real** (arpa y pizzicato) y se añade **Púa guitarra** (48 golpes de guitarras CC0 de Karoryfer: en notas
  reales de guitarra, color tras el golpe 16,5 → 10,1 dB y forma 0,47 → 0,75; mezclar las dos fuentes empeora ambas).
  Tecla vuelve a caer en el primer pico (el órgano real llega tarde). Tope de pico de cada golpe a -6 dB del pico del
  sample, antes de Entrada/Salida (True Peak máximo +6,35 → +4,18 dB). "Del sample" lee una copia sin desfase
  (`SampleData::attackHigh`, filtro adelante y atrás): restaba hasta -1,3 dB en 5 de 33 samples, ahora suma en fase.
  Recomendación por el nombre del archivo (`Mechanism::nameHint`). Botón **Oír todo / nota / golpe**
  (`SynthEngine::listen`, no se guarda). Probado y descartado: resonadores con los parciales del sample (0 a +0,5 dB,
  sin mejora) y un clasificador sinte/real por el sonido (datos sesgados).
- **Límite encontrado:** ninguna grabación de otra nota, ni la vecina del mismo piano bien alineada, acerca el ataque
  al real; el ataque verdadero es coherente con la nota. Línea abierta: excitar los parciales del propio sample.

## Hoja de ruta

- **Ahora:** pruebas de la alpha en Ableton y decidir con el oído si la IA aporta algo sobre el motor D.
- **Después:** LFOs y envolventes dibujables, arrastrar para modular, slicing, presets con el sample incrustado.
- **Más adelante:** versión macOS (VST3 + AU).

## 0.9.5 — Guías por pestaña (2026-11-13)

- Un "?" junto al nombre de cada pestaña (GuideDot, componente extra del TabBarButton) abre la guía de esa pestaña; el "?" de la cabecera sigue abriendo la guía rápida de 12 pasos.
- Motor común: GuideOverlay (Source/Guide.*). Niveles 1 Fundamentos / 2 Práctica / 3 Avanzado con botones para saltar, puntos de progreso, "Pruébalo" y "Para empezar", pulso suave del contorno (temporizador de 30 Hz sólo mientras está abierta). Dentro del contorno los clics llegan al control (hitTest), así se aprende tocando. Nunca cambia un parámetro.
- Contenido: Source/TabGuides.cpp, una función por pestaña. Los pasos encuentran su control por lo que ya identifica a cada uno: "p:<parámetro>" (Knob, Toggle, Choice), "s:<título>" (Section), el texto de un botón, o áreas con nombre que da el editor (sampleCard, engineCard, waveform, editView, export, ab). Para ampliar: añadir pasos con w.add(...).
- Validado en ReanimaSnapshot: 56 pasos (SAMPLE 15, EDICIÓN 10, FILTRO · MOD 11, MATRIZ 10, FX 10), cada uno con su control visible, la burbuja dentro de la ventana y sin taparlo, clics al control, los tres niveles, flechas, Anterior, Terminar, el ? y Esc cierran, y ningún parámetro cambia. Capturas 10_guia_<pestaña>_nivel_<n>.png.

## 0.9.5 — Refinamiento (2026-11-14)

- **CPU de Warp (motor A y los demás que usan WarpReader):** cada grano lee de una vez todos los canales que
  comparten posición (posición y fracción calculadas una vez; cuatro canales en un registro SSE con la misma
  aritmética que `hermite()`), `floor()` sin llamada a la biblioteca, sin seguir el nivel de grupos sin canales, y el
  motor A deja de leer el original cuando la nota ya lo ha pasado (sin bucle no vuelve). Los lechos también se
  guardan intercalados (`SampleData::bedQuad`). Resultado idéntico bit a bit en los seis motores
  (`tests/CpuBench.cpp`, `CPUBENCH_DUMP`); CPU de la improvisación con Voces 4, Textura y Release largo: 135 % → 72 %.
- **Guías:** cada paso ilumina el panel entero (sección, fila de la matriz, ranura de efecto, barra de edición)
  encontrado en la interfaz real, y dentro marca los controles de los que habla. Colores por nivel: Fundamentos
  verde, Práctica azul, Avanzado naranja. La comprobación (Snapshot) exige que ningún control visible quede cortado.
- **FX:** cada cadena conserva su color en todo lo suyo, también en el efecto elegido: MAIN azul, BUS 1 verde,
  BUS 2 naranja (`chainColour`). El violeta ya no se usa en la página.
- **Sample de demostración:** `Resources/Demo/demo_pluck.wav` dentro del plugin (datos binarios, igual en Windows y
  macOS). Solo en una instancia nueva y vacía, 0,4 s después de crearla; un proyecto, un preset o un sample del
  usuario que llegan antes lo impiden, y uno que llega mientras se analiza la demo la descarta.
- **Roce del arco:** el ruido continuo de Arco y Arco real sale de notas reales de arco pasada su mordida (0,2 s),
  cada grabación normalizada por el nivel de ese tramo; el nivel medio es el de antes. `roce.py` y `mechmix.py`.
- **Presets:** categorías, descripción y uso; 20 en total (8 nuevos); niveles igualados a unos -24,5 LUFS con
  `SampleGain`, sin pasar de -1,5 dBTP (`research/presets/auditar_presets.py`, `tests/PresetRender.cpp`).
