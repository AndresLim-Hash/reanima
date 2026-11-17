# Refinamiento global — Reanima 0.9.5 alpha (14-11-2026)

Copia de seguridad del estado anterior: `backups/antes_refinamiento_2026-11-13` (código y plugin instalado).
Plugin instalado ahora: sha256 `15650995…`.

## 1. CPU del motor A con polifonía — HECHO

**Problema reproducido** (`tests/CpuBench.cpp`, tu sample, Voces 4, notas aleatorias): sin Warp el motor A va
holgado (≤ 30 % incluso con samples de 25 s). Con **Warp en Tonos o Textura**, Sostenido y Release largo, la
improvisación llegaba al 135 % de media y 2.948 bloques fuera de tiempo: los crujidos.

**Causa medida:** el lector de granos de Warp es el 80 % del tiempo de cada voz. Cada copia de Voces leía el sample
hasta 34 veces por muestra (6 canales × 4 granos + referencias), calculando la posición canal a canal, y seguía
leyendo el original aunque la nota ya lo hubiera pasado. Con Release largo las voces se acumulan (hasta 33).

**Optimizaciones, todas sin cambiar una sola muestra del sonido** (prueba nula: idéntico bit a bit en los seis
motores A-F, en Warp y sin él):
- la posición se calcula una vez por grupo de canales y los cuatro canales de los lechos se leen a la vez (SSE);
- redondeo sin llamada a la biblioteca;
- no se sigue el nivel de grupos de canales vacíos;
- el motor A deja de leer el original cuando la nota lo ha pasado.

| Escenario (Warp Textura, Voces 4, Release 6 s) | Antes | Después |
|---|---|---|
| Improvisación, notas cortas | 135 % · 2.948 bloques tarde | 72 % · 2 |
| Notas rápidas repetidas | 142 % · 2.307 | 80 % · 7 |
| Acordes cada 0,5 s | 135 % · 2.152 | 74 % · 23 |
| 4 notas mantenidas | 18,7 % | 10,0 % |

La CPU vuelve a reposo al acabar cada nota (sin voces colgadas); 0 reservas de memoria en el hilo de audio.

**Corte al cambiar de motor — CORREGIDO:** al pasar de A a B, C o D se completa el análisis en segundo plano y, al
instalarse, se paraban todas las notas (silencio de ~0,1 s). Ahora, cuando solo se amplía el análisis del mismo
audio, las notas que suenan terminan con el análisis con el que empezaron y las nuevas usan el nuevo; el anterior se
libera después en el hilo de mensajes, nunca en el de audio (`SynthEngine::upgradeSample`). Prueba (Snapshot, A→B,
A→C y A→D con una nota sostenida): con la corrección, el nivel tras llegar el análisis baja como mucho 1 dB; sin ella,
-100 dB. Plugin instalado: sha256 `15650995…` (la versión anterior de esta fase, en
`backups/Reanima_0.9.5_refinamiento_sin_cambio_motor`).

## 2. Guías — HECHO
- Cada paso ilumina el **panel completo** (sección, fila de la matriz, ranura de efecto, barra de edición),
  localizado en la interfaz real, y **marca dentro los controles** de los que habla. Antes, algunos pasos unían dos
  mandos separados y el rectángulo cortaba otros rotativos (p. ej. "Modo ADSR y Golpe").
- Validado automáticamente: en los 56 pasos de las pestañas y los 12 de la guía rápida ningún mando, menú,
  interruptor o botón visible queda cortado; los resaltados son idénticos con la ventana al 75 %.
- Colores por nivel: **Fundamentos verde, Práctica azul, Avanzado naranja** (botones de nivel, título, contorno,
  flecha, puntos de progreso, "Pruébalo", borde de la burbuja y botón Siguiente).

## 3. Articulaciones — HECHO (arco); el resto auditado
- **Arco y Arco real:** el roce continuo se hacía con trozos de mordidas (la irregularidad medida era el doble que
  la de un arco real en Arco real). Ahora sale de notas reales de arco pasada su mordida, cada grabación normalizada.
  Mismo nivel medio que antes. Medido frente a violín y violonchelo reales (`roce.py`) y sobre 59 samples de la
  biblioteca (`mechmix.py`):

| | Arco antes → ahora | Arco real antes → ahora |
|---|---|---|
| Color frente al arco real | 22,0 → 12,9 dB | 16,1 → 13,9 dB |
| Irregularidad (real 2,5 dB) | 3,3 → 2,2 dB | 5,0 → 2,3 dB |
| Nivel medio (biblioteca) | -21,0 → -21,0 dB | -37,0 → -37,0 dB |
| Pérdida en mono | -1,5 → -0,9 dB | -0,7 → -0,8 dB |

- **Soplo / Soplo real:** probado el mismo método con aire real: el color empeoraba (21 → 24 dB). **No cambiado.**
- Golpes de Piano, Púa, Tecla, Pulsado, Púa guitarra, Del sample: sin cambios (auditados en la fase anterior:
  `docs/mecanismo_informe_2026-11-13.md`). Nivel de referencia: a 0,6, el aire de Soplo queda unos 16 dB sobre el
  aire real; se mantiene porque antes pediste que se oyera más. **Propuesto:** bajarlo si ahora te parece descompensado.

## 4. Colores de EFFECTS — HECHO
MAIN azul, BUS 1 verde, BUS 2 naranja en todo lo suyo, también en los mandos, menús, interruptores, barra Mix y
selección del efecto elegido. El violeta ya no se usa en esa página.

## 5. Sample de demostración — HECHO
Tu `ZEN_STRAT_synth_pluck_A.wav` va dentro del plugin (sin rutas: igual en Windows y macOS) como "Demo · synth
pluck". Comprobado: se carga en una instancia nueva y vacía y suena; un proyecto con su sample se abre con el suyo; un
proyecto sin sample sigue sin sample; un proyecto que llega mientras se analiza la demo la descarta; abrir y cerrar
las guías no carga ni cambia nada. **Aviso de licencia:** es de un pack comercial; para uso propio no hay problema,
pero si compartes el plugin, ese sample va dentro y la licencia del pack podría no permitirlo.

## 6. Presets — HECHO
Auditoría con `tests/PresetRender.cpp` + `research/presets/auditar_presets.py` (cada preset en 8 samples de familias
distintas: sonoridad EBU R128, pico real, estéreo, mono, DC, cola, CPU y parecido entre presets).
- **Problema principal:** 9 dB de diferencia de volumen entre presets (Shimmer -20,2 LUFS, Bajo sinte -29,4).
  Ahora 17 de 20 entre -24,4 y -24,9 LUFS; Supersaw, Drone eterno y Texturas glitch algo por debajo para no pasar de
  -1,5 dBTP. Ninguno pasa de -1,49 dBTP.
- Sin valores no finitos ni DC; las pérdidas en mono de ~3 dB son las de pads anchos descorrelados, no cancelaciones.
- **Piano de verdad** usa ahora Piano real (tu criterio para teclados reales); **Cuerdas con arco** gana el roce nuevo.
- **8 nuevos:** Arpa pulsada, Guitarra de púa, Teclado eléctrico, Lead expresivo, Campana de cristal, Pad que respira,
  Impacto cinemático, Texturas glitch. Guitarra de púa se rehízo porque medía casi igual que Piano de verdad.
- Navegador por categorías (Natural, Híbrido, Sintético, Percusivo, Atmósfera, Cinemático, Experimental); la ayuda
  de cada preset dice su categoría, qué es y para qué sirve.

## 7. Pruebas ejecutadas
| Prueba | Resultado |
|---|---|
| EngineTests | todo correcto, 0 fallos |
| Interfaz (Snapshot): guías, colores, demo, presets, deshacer, cambio de motor | 174 correctas, 0 fallos |
| Auditoría técnica (Audit, modo rápido) | 315 correctas, 0 fallos |
| Tiempo real (RtCheck, demo) | 0 reservas en 217 escenarios |
| Prueba nula motores A-F antes/después | idénticos bit a bit |
| Host (plugin instalado) | ventana, 8 instancias: correcto |

**No ejecutadas en esta fase:** Audit completo (modo largo), validadores de Steinberg y pluginval, ASan, sesión
larga de resistencia, y tu escucha en Ableton.

## 8. Ampliación de presets (41 en total) — HECHO
21 presets nuevos, medidos igual que los anteriores (8 samples, sonoridad EBU R128, pico real, mono, parecido) e
igualados a unos -24,5 LUFS sin pasar de -1,5 dBTP (algunos percusivos y el sub quedan algo por debajo por su pico).
- Natural: Pizzicato de cámara, Flauta con aliento, Piano de concierto.
- Híbrido: Órgano de iglesia, Marimba de madera, Clavinet funky.
- Sintético: Bajo sub, Acid, Pad analógico, Stab de house.
- Percusivo: Tom electrónico, Golpe metálico, Caja de ritmos lo-fi.
- Atmósfera: Viento helado, Coro etéreo, Instante congelado.
- Cinemático: Riser de tensión, Braam, Pulso de suspense.
- Experimental: Al revés, Robot formante.
Corregidos en la medición antes de entregarlos: Pulso de suspense y Clavinet funky (picos de +6 dBTP), Coro etéreo y
Robot formante (el filtro de vocales los dejaba casi mudos con algunos samples), Pizzicato y Flauta (demasiado
parecidos a Piano de verdad y Cuerdas con arco). Plugin instalado: sha256 `5afa3068…` (el de 20 presets en
`backups/Reanima_0.9.5_20_presets`).

## 9. Banco de presets ampliado (56) — HECHO
- Los presets pueden fijar los parámetros de cada efecto en cualquier cadena (`Factory::FxSetting`: MAIN, BUS 1, BUS 2),
  y los envíos y niveles de los buses. Copia del banco anterior: `backups/presets_antes_banco_2026-11-14`.
- Eliminado: Piano de concierto (casi idéntico a Piano de verdad). Efectos afinados en 9 existentes (tipo de reverb,
  decay, pre-delay, delays sincronizados y filtrados).
- Categorías nuevas: **Rítmico** (Eco de cinta, Ping-pong de semicorcheas, Puerta trance, Filtro que late, Wobble,
  Hi-hats granulares, Golpes en multi-tap, Latido de tráiler, Arpegio de cristal, Trémolo surf) y **Agresivo** (Fuzz
  con flanger, Sierra oxidada, Rectificador, Saturación paralela). También Cinta que se arrastra (Atmósfera) y Espiral
  de Bode (Experimental).
- Validado: los 56 suenan, conservan el sample y recuperan todos sus parámetros al guardar y abrir; ninguno pasa de
  -1,5 dBTP; mono sin cancelaciones. Niveles: la mayoría entre -24,5 y -25,5 LUFS; percusivos dispersos más bajos
  (Hi-hats granulares unos -32 LUFS) por su naturaleza. Plugin instalado: ver `sha256` en la entrega.

## 10. Ampliación a 206 presets — HECHO
150 nuevos en `research/presets/banco150.py` (genera `Source/FactoryBank.inc`); niveles en `niveles150.csv`
(`nivelar150.py`, tres pasadas sobre 4 samples). Categorías nuevas: Transformado, Industrial, Oscuro, Evolutivo.
Comprobado: los 206 suenan (nota de 6 s), conservan el sample y recuperan todos sus parámetros al guardar; ninguno pasa de
-1,5 dBTP; pérdida en mono como mucho 3,7 dB (sin cancelaciones); CPU máxima 16,7 % de un núcleo. Escucha individual
pendiente. Banco anterior: `backups/presets_antes_150_2026-11-14`; plugin anterior: `backups/Reanima_0.9.5_56_presets`.
