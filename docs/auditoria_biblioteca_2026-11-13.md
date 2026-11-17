# Auditoría de la biblioteca maestra de samples (13-11-2026)

Biblioteca: `samples_prueba/all samples`. Comparada con `samples_prueba/one shots` (ONE SHOTS) y con las dos tandas
analizadas antes (`research/muestras_nuevas_2026-11-08*.csv`). Ningún sample se ha modificado, movido ni renombrado.

Datos reutilizables en `research/biblioteca_2026-11-13/` (guía en su `LEEME.md`): inventario CSV/JSON de los 9114
archivos (91 columnas), relaciones verificadas, 12 conjuntos de prueba, mediciones y los scripts que lo generan.

> **Medido** = calculado sobre el audio. **Estimado** = deducido del nombre del archivo o de una regla; el inventario
> marca la fuente de cada clasificación (`tipo_fuente`, `familia_fuente`).

---

## 0. Lo más importante

1. **Tonos (Warp, versión 0.9.3, aún no instalada) falla con mucho material real.** Medido sobre 176 one shots
   tonales de 25 familias: a +12 st, en 97 la mayor parte de la energía no cae en los armónicos de la nota pedida
   (a +7 st, en 51). Textura falla en 15 y 5. Causas encontradas: acordes (PSOLA no transporta acordes), nota
   detectada una octava abajo (Tonos toca una octava por debajo de lo pedido), sonidos sin armónicos pares (onda
   cuadrada: se cancelan) y sintes con voces desafinadas. **Recomiendo no instalar la 0.9.3 tal cual** y corregirlo
   antes (propuesta 1).
2. **La biblioteca es sobre todo de loops:** 5010 de 7089 (71 %), 4446 con el tempo en el nombre. Es el material ideal
   para la fase 2 (Loop/Tempo) y aporta poco a los one shots: solo 460 one shots nuevos.
3. **La recomendación de Mecanismo acierta poco:** frente al instrumento del nombre, el 24 % (guitarras 0 %, metales
   8 %, maderas 22 %, cuerdas frotadas 67 %). Ni las medidas actuales ni cinco medidas físicas nuevas lo mejoran
   con este material (máximo 43 %, validación cruzada). Con este material sí funcionaría usar el nombre del archivo
   como pista. Reconocer el gesto por el sonido necesita grabaciones de referencia limpias (sección 9).
4. **Los motores son robustos con el material nuevo:** 460 one shots nuevos en los seis motores, sin valores no
   válidos. A y C sin ninguna incidencia; B y D superan el pico del sample en más de 3 dB en 11 (2,4 %, igual que
   en la 0.9.2); E hace 47 clics al soltar en golpes secos.
5. **Calidad de los archivos: buena.** 1 ilegible; 5264 de 7089 sin ningún aviso; los avisos más comunes
   (silencios, huecos) son normales en loops.

---

## 1. Total examinado

| | Biblioteca (`all samples`) | ONE SHOTS |
|---|---|---|
| Archivos | 7090 (7084 `.wav` + 6 `.Wav`) | 2024 |
| Carpetas | 1 (sin subcarpetas) | 1 |
| Tamaño | 20,1 GB | 2,7 GB |
| Legibles | 7089 | 2024 |
| Audio total | ≈ 20 horas | — |

Cada archivo se abrió y se decodificó entero; el formato se comprobó leyendo su cabecera, no su extensión. Huella
SHA-256 de los bytes y del audio decodificado (PCM).

## 2. Características generales (medidas)

- **Formato:** 44,1 kHz 6484 · 48 kHz 442 · 96 kHz 143 · 192 kHz 20. PCM 24 bit 6677 · 16 bit 384 · coma
  flotante 28. Estéreo 7015 · mono 71 · 6 canales 3. 207 estéreos tienen los dos canales idénticos (mono real).
- **Duración:** mediana 7,7 s; el 10 % dura menos de 0,48 s y el 10 % más de 19,6 s; la más larga, 328 s.
- **Tipo (estimado):** loops 5010 (4455 por el nombre, 555 por sus eventos medidos), one shots 1984 (644 por el
  nombre, 1340 medidos), texturas/largos 95.
- **Familias (estimado por el nombre; 395 sin nombre informativo, clasificados por la medida como tonales o no):**
  batería en loop 1336 · voz 595 · bombos 566 · bajos 467 · cajas 346 · FX 332 · hi-hats 332 · sintetizador 232 ·
  guitarra 210 · melodías/loops musicales 200 · piano 191 · palmas 182 · piano eléctrico/órgano 134 · ambientes
  133 · 808 132 · metales 129 · acordes/stabs 128 · toms 115 · percusión pequeña 112 · maderas 108 · pads 100 ·
  percusión de mano 82 · leads 78 · arpas 73 · cuerda frotada 68 · arpegios 58 · plucks 55 · platillos 50 · mazos 44 ·
  log drums 39 · campanas 39 · rims 24 · cuerda pulsada étnica 4.
- **Metadatos internos:** 284 archivos con bloque `acid` (tempo; coincide con el nombre en 171 de 199; 25 traen
  120 por defecto). 426 con bloque `smpl`, pero 396 dicen nota 60 por defecto: **no sirven como referencia de nota**.
- **Por familia (one shots, medianas medidas por Reanima):** bombos ataque 3 ms, sustain 0,45; cajas 2 ms, sin
  sustain, transitorio +15 dB; bajos 20 ms, sustain 0,82, tonal 0,99; 808 8 ms, sustain 1,0; metales 44 ms; maderas
  205 ms, transitorio -22 dB; cuerda frotada 111 ms; piano 19 ms, decay 1 s; platillos 132 ms, decay 2,3 s.
  Tabla completa en `mediciones/estadisticas.txt`.

## 3. Archivos defectuosos o problemáticos (medido)

| Problema | Total | One shots | Loops | Nota |
|---|---|---|---|---|
| **Ilegible** | 1 | — | — | `Kick_04.wav`: es un FLAC con extensión `.wav`, truncado (la cabecera dice 1,03 s y solo hay 26 KB). En ONE SHOTS hay otro `Kick_04.wav` válido (0,15 s, otra edición). |
| Clipping (≥3 muestras seguidas a 0 dBFS) | 161 | 85 | 76 | Sobre todo bombos, 808 y bajos |
| Techo plano (recortado y luego bajado) | 139 | 37 | 102 | Baterías en loop |
| Corte brusco de banda (fuente comprimida o remuestreada) | 103 | 42 | 61 | Cortes a 15–17 kHz en 48; a 6–10 kHz en 6 |
| Nivel muy bajo (pico < -20 dBFS) | 58 | 6 | 52 | Ambientes y atmósferas |
| DC apreciable (> -20 dB del RMS) | 20 | | | Las formas de onda asimétricas de bombos y bajos dan valores menores, que no son defectos |
| Estéreo en contrafase (correlación < -0,2) | 177 | 35 | 142 | Efectos muy abiertos; solo 2 por debajo de -0,8 |
| Desequilibrio L/R > 3 dB | 333 | 69 | 264 | Normal en efectos y paneos |
| One shot que termina sin fundido | 56 | 56 | — | Puede hacer clic al final |
| Silencio digital dentro del sonido | 255 | 4 | 251 | En loops y stems son los silencios entre golpes, no cortes |
| Silencio inicial > 100 ms | 528 | 40 | 488 | En loops suele ser un compás que empieza en silencio |

No hay valores no válidos (NaN), archivos vacíos, archivos de silencio total ni datos truncados, aparte de
`Kick_04.wav`. El detalle por archivo está en las columnas `problemas` y `avisos` del inventario.

## 4. Duplicados exactos y variantes

- **Idénticos byte a byte:** los 2023 archivos de ONE SHOTS que tienen el mismo nombre en la biblioteca. Dentro de la
  biblioteca no hay duplicados exactos.
- **Mismo audio, distintos bytes:** `Digitalo_Kick18.wav` = `Kick_19.wav` (solo cambian los metadatos).
- **Variantes verificadas por el audio:** 39 relaciones entre 67 archivos. El método tiene dos pasos: una huella por
  bandas de 1/4 de octava propone 910 parejas, y luego se verifica cada una por la forma de onda (correlación
  cruzada) y por el espectro fino.
  - 3 son la misma grabación, por ejemplo `KMRBI_AS3_…_dry` / `…_wet`;
  - 6 son la misma grabación retocada (nivel, EQ o longitud);
  - 6 son la misma grabación con otro procesado (efecto o reverb);
  - 24 son stems o mezclas parciales de la misma grabación (`…_full` / `…_kick` / `…_tops`).
- **Descartados:** 486 parejas que se parecían en la huella eran sonidos parecidos de otra grabación, sobre todo
  bombos y 808 sintéticos. No son duplicados y no se tratan como tales.
- **Familias por nombre:** 168 grupos (433 archivos) con el mismo nombre salvo palabras de versión (dry/wet,
  full/top, alt…). Sirven para encontrar series de un mismo pack; el audio las confirma solo cuando hay relación real.

## 5. Diferencias con ONE SHOTS

- **ONE SHOTS está contenida en la biblioteca:** 2023 archivos idénticos y `Kick_04.wav`, que difiere (el de la
  biblioteca está corrupto).
- **Los análisis anteriores siguen siendo válidos:** 1894 de 1898 coinciden en duración y pico. Los 4 restantes son
  2 archivos de más de 120 s, que entonces se midieron recortados porque Reanima corta a 120 s al cargar.
- **Solo en la biblioteca:** 5066 archivos nuevos: 4550 loops, 460 one shots y 56 texturas.

## 6. Material nuevo con valor

- **Loops con tempo y tonalidad en el nombre:** 4446 con BPM y 2813 con tonalidad. Son la referencia que necesita la
  fase 2. Un estimador de tempo sencillo acierta el 31 % y en otro 49 % se equivoca en la octava o en el compás:
  el problema es real y medible.
- **Voces:** 489 nuevas (frases, chops, ad-libs, coros), casi todas loops. Es el mejor material para juzgar Tonos,
  porque en la voz los formantes son lo primero que se oye.
- **Stems de un mismo loop** (full/kick/tops) y **parejas dry/wet:** referencia de dónde está cada golpe (para
  trocear) y de qué añade la reverb.
- **Guitarras** (188 nuevas), **pianos y teclas en loop**, **bajos en loop** (311) y **baterías en loop** (1228).
- **Casos de formato difíciles:** 6 canales, 192 kHz, coma flotante, mono y archivos de más de 120 s.

## 7. Material por motor y función (conjuntos en `conjuntos/`)

Cada conjunto es un CSV (ruta original, motivo, medidas clave) más un `.txt` de rutas que ReanimaBatch acepta como
lista. Mezclan ejemplos representativos y casos difíciles, repartidos por el rango de la medida, no solo los que
salen bien.

| Conjunto | Archivos | Para qué |
|---|---|---|
| `warp_tonos_monofonicos` | 80 | Tonos: 12 familias monofónicas, de grave a agudo |
| `warp_trampas` | 32 | Acordes, unísonos desafinados, muy cortos, glissandos y los dos casos analizados |
| `nota_graves` | 93 | Nota de bajos, 808 y log drums por debajo de E3, con todos los fallos actuales |
| `mecanismo_referencias` | 59 | 8 por familia (piano, guitarra, arpa, pluck, cuerda frotada, maderas, metales, teclas), repartidos por ataque |
| `loop_tempo` | 150 | 7 franjas de tempo, más swing, medio tiempo y tresillos, más tempos confirmados por `acid` |
| `troceado_stems` | 65 | Stems verificados y baterías de densidad baja a alta |
| `transitorios` | 30 | Percusión del ataque más seco al más blando |
| `envolventes_adsr` | 60 | Percusivo, pulsado, sostenido, crescendo y con cola de reverb |
| `registro_espectro` | 39 | De la nota más grave a la más aguda, del más oscuro al más brillante |
| `texturas_E_F` | 25 | Ambientes, pads y largos para los motores E y F |
| `variantes_dry_wet` | 73 | Misma grabación con y sin procesado |
| `dificiles_y_defectuosos` | 69 | Clipping, corte de banda, nivel bajo, contrafase, 6 canales, 192 kHz, float, mono, > 120 s y el ilegible |

## 8. Oportunidades concretas (lo que se ha medido con este material)

- **Warp:** las cifras de la sección 0. Signalsmith Stretch, ya incluido, transporta bien casi siempre: a +12 falla
  en 20 de 176 y a +7 en 12. En cambio, apenas conserva los formantes: 12,4 dB de distancia frente a 7,2 de Tonos
  cuando Tonos funciona y 15,0 de Re-Pitch.
- **Nota:** acierta el 74 % de 582 one shots con nota en el nombre:
  - bajos 90 %, 808 89 %, metales 82 %, maderas 78 %;
  - los bombos (31 %) llevan en el nombre la tonalidad del tema, no su nota.

  En bajos y 808 los fallos están en notas muy graves (E0–F#0) y caen a ±0,4–0,5 st del borde entre dos notas, lo
  que apunta a la caída de tono del arranque. Además, un bajo eléctrico salió una octava abajo (E0 en vez de E1).
- **Análisis:** a 192 kHz tarda unas 9 veces más que a 44,1 kHz para la misma duración (0,3 s de sonido: 3,3 s
  frente a 0,35 s). Los archivos de más de 120 s se recortan al cargar.
- **Motor E:** 47 de 460 golpes secos hacen clic al soltar.
- **Mecanismo:** sección 0, punto 3.

## 9. Material adicional

**Ya disponible:** no hace falta pedir 808, bombos, cajas, loops de batería, bajos, voces en loop ni FX; hay de sobra.

**Poco representado (one shots tonales de instrumento acústico):**
- piano 15, piano eléctrico/órgano 12, guitarra 15, arpa 10, cuerda frotada 27 y mazos 14;
- ninguna serie de la misma nota a varias intensidades;
- ninguna serie de un mismo instrumento a muchas alturas;
- pocas vocales cantadas sostenidas y secas.

**Imprescindible:**
- **Un instrumento grabado nota a nota a lo largo de su registro, seco.** Es la única referencia exacta para Tonos: la
  nota real una octava arriba frente a la transportada. Sirve también para la nota en graves.
- **Notas sueltas limpias de piano, guitarra, cuerda frotada, viento y teclas, con su gesto conocido.** Sin ellas no
  se puede enseñar a reconocer el gesto.

Hay fuentes gratuitas que lo cubren: University of Iowa Electronic Music Studios, VSCO 2 Community Edition (CC0) y
NSynth (CC BY 4.0). No he descargado nada: si quieres, te digo tamaño, licencia y origen de cada una antes.

**Recomendable:**
- un piano con varias intensidades por nota (Salamander Grand Piano, CC BY 3.0);
- loops con los golpes anotados, para validar el troceado (el tempo ya está en los nombres de esta biblioteca).

**Lo que puedes grabar tú** (seco, sin reverb, 48 kHz / 24 bit, con 1 s de silencio delante):
- tu voz cantando a, e, i, o, u, sostenidas, en tres alturas;
- si tienes guitarra o teclado: una nota pulsada, apagada y con armónico, a tres alturas y tres intensidades;
- los ruidos reales de tu teclado (tecla, pedal).

**Opcional:** referencias profesionales para comparar a oído: Ableton Simpler (Tones/Texture), Kontakt o Falcon con
el mismo sample.

## 10. Propuestas de mejora y prioridades

### P1 · Tonos fiable (urgente: antes de instalar la 0.9.3)

- **Material:** `mediciones/warpfail.json` (176 one shots, 25 familias), `sscheck.json`, `warp_trampas`, y cuatro
  casos explicados: `JC_FH2_brass_soft_warm_Fmin` (acorde), `PJ_SII_Keys_oneshot_Dark_Pluck_Note_1_C` (unísono
  desafinado), `JJP_RRNB2_electric_bass_one_shot_classic_Em` (octava) y `STCR2_SG4_Synth_Bass_Dermis_D` (cuadrada).
- **Limitación:** Tonos no transporta bien en más de la mitad del material a +12 y en un 29 % a +7.
- **Módulo:** `WarpReader` (SynthEngine.cpp) y `SampleAnalysis` (marcas de periodo).
- **Solución, en dos pasos:**
  - **(a) Ya.** Al cargar, en el hilo de análisis, Reanima prueba Tonos sobre 0,5 s del sample a +7 y +12 con la
    misma prueba de rejilla de esta auditoría. Si falla, ese sample usa Textura con Tonos elegido y el menú dice
    «Tonos → Textura (acorde / ciclos irregulares)». Además, el periodo se comprueba contra la mitad y el tercio: si
    el ciclo se repite antes, se corrige la octava.
  - **(b) Después.** Un desplazamiento espectral propio con corrección por la envolvente, o Signalsmith con una
    corrección de envolvente más fuerte. Como el sample se conoce entero, se puede leer por adelantado: sin latencia.
- **Beneficio:** (a) ninguna nota equivocada con Tonos; (b) formantes en su sitio también donde hoy falla PSOLA.
- **Rendimiento:** (a) unos 0,1–0,3 s más de análisis por sample y nada durante la reproducción; (b) por medir
  (Signalsmith: ≈1,6 % de un núcleo por voz).
- **Comprobación:** `warpfail.py` sobre los 176 y los 32 de `warp_trampas`. Objetivo: nota correcta en ≥ 95 % a
  ±7 y ±12, formantes ≤ 8 dB donde se use Tonos, y escucha A/B.
- **Estado (0.9.4, 13-11-2026): (a) hecho.** Con `warpfail.py`, nota correcta a +12 del 53,4 % al 95,5 % y a +7
  del 80,1 % al 98,3 % (el máximo posible eligiendo entre Tonos y Textura era 97,7 % y 99,4 %). Pasan a Textura
  110 de 176. Formantes donde Tonos se mantiene: 8,1 dB a +12 y 8,8 dB a -12 (objetivo de 8 dB: casi). La
  comprobación de octava no se activa en ningún sample de la biblioteca. Detalle en `DESIGN.md`, sección 0.9.4.
  Falta la escucha A/B.

### P2 · Fase 2, Loop/Tempo, con esta biblioteca como banco de pruebas

- **Material:** `loop_tempo` (150), los 4446 loops con BPM, los 284 con bloque `acid`, los stems de `troceado_stems`
  y 2813 loops con tonalidad en el nombre.
- **Limitación:** no hay modo loop. Un estimador simple acierta el tempo exacto solo el 31 % de las veces.
- **Solución:** detección de tempo con resolución de octava y compás (flujo espectral + peine de tempos +
  preferencia por la duración en compases enteros), troceado por golpes y sincronía con el DAW.
- **Comprobación:** ≥ 90 % de tempos exactos (±1 %, octava correcta) en `loop_tempo` y en los 4446, y golpes
  dentro de ±10 ms en los stems.

### P3 · Recomendación de Mecanismo: el nombre como pista

- **Material:** `mecanismo_referencias` (59) y los 194 one shots tonales con instrumento en el nombre.
- **Limitación:** 24 % de acierto. El sonido solo, con este material, no lo arregla (máximo 43 %).
- **Módulo:** `Mechanism::fit` y el panel TIMBRE.
- **Solución:** si el nombre dice el instrumento (guitar, harp, violin, flute, brass, piano, rhodes…), el gesto
  recomendado sale de ahí y la línea dice «por el nombre». Si no, se usa la medida, como hoy. Mismo diccionario
  que `libnames.py`.
- **Beneficio:** recomendaciones creíbles en los samples comerciales, que casi siempre llevan nombre.
- **Rendimiento:** nulo. **Comprobación:** acierto ≥ 80 % en los 194 y nada cambia en samples sin nombre informativo.

### P4 · Nota en graves

- **Material:** `nota_graves` (93, con todos los fallos).
- **Limitación:** bajos y 808 aciertan el 89–90 %. Los errores son de semitono (la caída de tono inicial) y de
  octava.
- **Solución:** medir el tono en el tramo estable tras la caída inicial y comprobar la octava por periodicidad.
  Esta comprobación es la misma que necesita P1(a).
- **Comprobación:** ≥ 95 % en `nota_graves`, sin empeorar el resto (582 one shots con nota).

### P5 · Análisis a 48 kHz como máximo

- **Material:** los 163 archivos de 96 y 192 kHz.
- **Limitación:** a 192 kHz el análisis es unas 9 veces más lento.
- **Solución:** analizar una copia a 48 kHz y reproducir siempre el original.
- **Comprobación:** mismo resultado (nota ±0,05 st, ADSR) y tiempo de análisis comparable al de 44,1 kHz.

### P6 · Motor E: clics al soltar golpes secos

- **Material:** las 47 notas con clic (`mediciones/motores_one_shots_nuevos.csv`).
- **Solución:** fundido corto de los granos activos al soltar.
- **Comprobación:** 0 clics en esa medición.

### Orden recomendado

| Orden | Qué | Riesgo | Cambio |
|---|---|---|---|
| 1 | P1(a) y la comprobación de octava de P4 → 0.9.4, y entonces instalar | bajo | no estructural |
| 2 | P3 | bajo | no estructural |
| 3 | P5 y P6 | bajo | no estructural |
| 4 | P2 (fase 2) | medio | estructural: presentaré el diseño antes |
| 5 | P1(b) | medio | estructural: necesita el material imprescindible de la sección 9 |

**Sobre tus archivos:** te recomiendo sustituir `Kick_04.wav` de la biblioteca por el de ONE SHOTS. Los 161 con
clipping y los 103 con corte de banda están listados en el inventario por si quieres apartarlos. No he tocado
ninguno.
