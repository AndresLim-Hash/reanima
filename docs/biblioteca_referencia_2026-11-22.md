# Biblioteca de referencia de Reanima: «all samples» (22-11-2026)

Biblioteca: `samples_prueba/all samples`, actualizada por el usuario después de la auditoría del 13-11 (`docs/auditoria_biblioteca_2026-11-13.md`).
Ningún sample se ha modificado, movido ni renombrado: todo los nombra por su ruta original.

Los datos y scripts están en `research/biblioteca_2026-11-22/` (guía en su `LEEME.md`).

> **Fuentes de cada dato**
> - **[medido]**: calculado sobre el audio.
> - **[Reanima]**: lo que detecta el análisis del propio plugin (versión 0.9.5 de hoy).
> - **[nombre]**: estimado del nombre del archivo.
> - **[regla]**: deducido de lo anterior con una regla escrita en el script.
>
> **Valoraciones de escucha: ninguna en este informe.** Las que hagan falta se piden aparte, con listas A/B.
>
> **BPM y tonalidades:** solo se dan los del nombre, o los que Reanima mide junto con lo que suele acertar. No se inventa ninguno.

---

## 0. Lo más importante

1. **Material nuevo:** 279 archivos, 2,1 GB.
   - **No son audio** 78 de ellos: archivos `._nombre.wav` de 163 a 353 bytes que macOS crea al copiar. Se pueden borrar sin perder nada; no los he borrado.
   - **Uno es silencio digital.**
   - **Audio útil:** 200 archivos, 132 minutos.
   - **Casi todo son loops largos** (196): mediana de 29,5 s; 28 pasan de 70 s y 4 de 120 s.
   - **Muchos son pistas de canciones reales:** 16 temas con 3 pistas o más (batería, bajo, voz, teclas, pads...) del mismo tema, tempo y tonalidad.
   - **Lo que aporta, que antes no teníamos:**
     - música real de productores, en vez de loops de librería: pistas con silencios largos;
     - partes que entran a mitad de canción;
     - 38 archivos en coma flotante;
     - stems de un mismo tema que suenan bien juntos.
2. **Error del tempo por el sonido.** Con este material aparece un fallo que la biblioteca anterior no dejaba ver, porque sus nombres ya traían el tempo.
   - Sin leer el nombre, el tempo que Reanima mide coincide con el del nombre en 17 de 102 pistas. En 46 sale el doble o la mitad, y en 39 otro distinto.
   - **La mayoría de esos «distintos» son exactamente 3/4 del tempo real:** 116 → 87, 98 → 73,5, 90 → 67,5, 80 → 60, 106 → 79,5.
   - Reanima elige un número de tiempos 3/4 del verdadero: 48 en vez de 64, 12 en vez de 16.
   - Es un sesgo concreto, reproducible y fácil de atacar (sección 6).
3. **Tonalidad por el sonido de pistas largas:** acierta la tonalidad o su relativa en 32 de 89 (36 %); muchas fallan por una quinta. Confirma la medición de la fase 7: por el sonido, solo sugerencia.
4. **Biblioteca de referencia de calidad sonora:** 120 muestras en 12 categorías, de lo sencillo a lo difícil; 20 son del material nuevo.
   - Viene con su línea base: los 6 motores × 5 casos (raíz, +7, −12, suave y acorde), 3600 medidas.
   - Lleva un script que compara cualquier versión futura con esa línea base y dice qué mejora y qué empeora (sección 4).
5. **Lo que la línea base ya enseña (medido, sin escuchar todavía):**
   - A, B y D reproducen el sample exactamente en la raíz (error espectral mediano 0,00–0,01 dB).
   - **Motor C:** el error mediano es de 2,04 dB. Va de 0,3 dB en las colas largas a 3,9 dB en los sonidos con evolución de tono.
   - **Motores B y D:** caen por debajo de −40 dB, donde el sample suena, en 17 muestras. Son sobre todo risers y sonidos que empiezan bajos y crecen: 5 risers, una atmósfera vocal, la tetera y loops de hats.
     - El patrón coincide con el fallo de ataque lento que arreglé hoy en el motor C (sección 6, punto 2).
   - **Motor F:** abre la imagen estéreo hasta llegar a contrafase en algún caso; al sumar a mono pierde hasta 4,6 dB.
   - Ninguna medida da valores no válidos.

---

## 1. Total examinado

| | Antes (13-11) | Ahora (22-11) | Nuevo |
|---|---|---|---|
| Archivos en `all samples` | 7090 | 7369 | 279 (78 son `._` de macOS) |
| Tamaño | 20,1 GB | 22,2 GB | 2,1 GB |
| Audio legible | 7089 | 7289 | 200 (más 1 silencio digital) |
| Horas de audio | ≈ 20 | 22,3 | 2,2 |

- **Archivos que faltan:** ninguno de los de antes; nada se ha borrado ni movido.
- **Inventario:** se ha vuelto a medir todo, 9393 archivos contando ONE SHOTS, con el mismo método del 13-11. El análisis de Reanima se ha hecho con la versión actual en los 7289 legibles.

## 2. El material nuevo

### Formato [medido]
- **Frecuencia de muestreo:** 44,1 kHz 147, 48 kHz 54.
- **Formato de muestra:** PCM 24 bit 100, PCM 16 bit 63, coma flotante 38.
- **Canales:** todos estéreo (3 con los dos canales idénticos).

### Tipo y carácter
- **Tipo [nombre/medido]:** 196 loops, 2 texturas largas y 2 one-shots.
- **Carácter [regla sobre Reanima]:**
  - 100 tonales: 73 monofónicos o casi y 26 con acordes (3 o más notas a la vez), según la polifonía [medido];
  - 74 percusivos (más de la mitad de su energía en golpes);
  - 27 texturas o ruido.
- **Familias [nombre]:**

  | Grupo | Archivos | Contenido |
  |---|---|---|
  | Percusión | 61 | kick, snare, hats, bongos, shakers, percusiones, baterías completas |
  | Sin nombre informativo | 59 | se clasifican por la medida |
  | Sintetizador | 19 | leads, pads, arps |
  | Bajo | 15 | |
  | Voz | 12 | voces principales, voz afinada, coros, voz como percusión |
  | Cuerda pulsada | 12 | guitarras acústica y eléctrica, pedal steel, arpa |
  | Teclas | 6 | |
  | Ambiente | 4 | |
  | Cuerda frotada | 4 | |
  | FX | 3 | |

- **Con tempo en el nombre:** 158. **Con tonalidad en el nombre:** 105.

### Canciones con varias pistas [nombre; en `canciones_nuevas.csv`]

| Canción | Pistas | Contenido |
|---|---|---|
| deluxe 122 C menor | 16 | |
| woza D# 116 | 15 | percusión afro: bongos, shakers, cajas, kick, palmas |
| madness D#m 130 | 11 | 808, bajo, clap, tops, voces |
| stems reflejo D# mayor 120 | 8 | teclas, pad, cuerdas |
| brava Ebm 100 | 7 | |
| fallen | 7 | bajo, guitarra, textura |
| chrome Em 101 | 6 | |
| no digas nada Gbm 130 | 6 | |
| be true 120 | 5 | |
| comearound | 5 | |

### Problemas [medido]
- **Silencio inicial:** 49 pistas empiezan con más de 1 s de silencio, y 30 con más de 10 s. Son partes que entran a mitad del tema.
- **Nivel muy bajo:** 12. **Desequilibrio L/R:** 11. **Fase invertida entre canales:** 8. **DC:** 6.

### Duplicados y variantes [medido]
- 6 duplicados exactos de archivos que ya estaban.
- 21 variantes verificadas.
- 17 archivos `__wavsmod_…` son otra versión de un loop que también está sin ese sufijo. En algunos el tempo medido cambia: el original da 73,5 y el `wavsmod` 134,2.

### Límites de Reanima que este material toca
- **4 archivos de más de 2 minutos:** `loadFile` lee como mucho 120 s y el resto no llega.
- **28 de más de 70 s:** LOOP no guarda su copia sin fundidos (`loopAudio` está vacío por encima de 70 s).
- **Pistas con decenas de segundos de silencio al principio:** ponen a prueba el inicio detectado y la longitud del loop.

## 3. Utilidad para cada motor y función

Lo que cada archivo aporta está, por archivo, en la columna «utilidad» de `indice_biblioteca.csv` [regla]. Resumen:

| Motor o función | Material más útil | Qué permite detectar |
|---|---|---|
| **A** (reconstrucción) | Sencillos, colas largas, graves | Fidelidad exacta; clics al soltar en loops cortados a media nota |
| **B, D** (re-envolvente, híbrido) | Sostenidos, loops de batería tocados como nota | Silencios del motor B sobre loops de batería (sección 0); subidas por encima del pico |
| **C** (resíntesis) | Evolución tonal, ruido, loops densos, acordes | Error espectral 3–4 dB en esos tipos (frente a 0,3–0,4 en notas limpias y colas) |
| **E, F** (granular, dron) | Texturas, pads, atmósferas, las 27 texturas nuevas | Pérdida al sumar a mono, hasta contrafase en F; caídas hasta −7 dB |
| **LOOP Warp (Beats, Complex, Pro)** | Las 196 pistas nuevas; stems con silencio inicial; coma flotante | Tempo por el sonido: 3/4 y doble/mitad; inicio con silencio |
| **Transitorios, SLICE, golpes** | woza (15 pistas de percusión afro), madness tops/percloop, brava drums, loops densos | Golpes sobre colas (arreglo de hoy); densidad alta |
| **Seguimiento de tono, Warp Tonos** | Bajos, leads y voces monofónicas nuevas (73 tonales monofónicos) | Notas que se deslizan (pedal steel, voz afinada) |
| **Acordes y polifonía** | 26 tonales nuevos con acordes, dancethen chords, keys/pad de reflejo, acordes y stabs de antes | Resíntesis y Tonos con acordes; reconocimiento de acordes por el sonido |
| **Arpegiador, secuenciador y tonalidad** | Stems de una misma canción (misma tonalidad y tempo) | Tonalidad por la entrada lateral con una pista real; el arpegiador sobre el tempo de un tema |
| **Envolventes, filtros, modulación** | Colas largas, sostenidos, transitorios | Caídas con suave, +7 y −12 (sección 4) |

## 4. Biblioteca de referencia de calidad sonora

`research/biblioteca_2026-11-22/conjuntos/referencia_calidad.csv` (y su `.txt` de rutas): 120 muestras, cada una con su motivo. Están elegidas por sus medidas, repartidas por familias y por el rango de la medida.

| Categoría | Muestras | Criterio [medido / Reanima] |
|---|---|---|
| 1 Sencillos (control) | 10 | Una nota limpia (armonicidad > 0,8), sin problemas, de grave a agudo |
| 2 Transitorios rápidos | 12 | Ataque ≤ 3 ms (Reanima), cresta > 12 dB |
| 3 Colas largas | 10 | Caen 20 dB en ≥ 1,5 s |
| 4 Acordes complejos | 10 | Acordes y stabs, y lo nuevo con 4 o más notas a la vez |
| 5 Graves | 10 | Nota por debajo de E1 |
| 6 Ruido y sonidos sin nota | 10 | Planitud ≥ 0,25; la tetera |
| 7 Loops con muchos golpes | 10 | ≥ 6 golpes/s |
| 8 Sostenidos | 10 | Sustain ≥ 0,6, de 2 a 15 s |
| 9 Evolución tonal | 10 | riser, sweep, glide, bend, slide... |
| 10 Estéreo y fase | 8 | Correlación L/R < 0,2 (compatibilidad mono) |
| 11 Pistas de canciones (nuevo) | 10 | Batería, bajo, voz y teclas de las 4 canciones con más pistas |
| 12 Difíciles y defectuosos | 10 | Clipping, techo plano, nivel bajo, DC, contrafase, corte de banda, 96/192 kHz, coma flotante, > 120 s |

### Banco «calidad» (`ReanimaBatch calidad <lista> <csv> [motores]`)
Cada muestra se toca con la envolvente detectada y la nota mantenida durante su duración (4 s como mucho), en los motores A–F y en 5 casos: raíz, +7, −12, velocidad 0,3 y acorde de 3 notas. Medidas por caso:
- **Fidelidad**, solo en la raíz y comparable en A–D: error medio de 24 bandas cada 20 ms frente al sample, con el nivel igualado. E y F no buscan reproducirlo.
- **Peor caída y peor subida del nivel:** en la raíz, frente al sample; en el resto, frente a la mediana de los 100 ms de alrededor.
- **Pérdida al sumar a mono** frente a la del propio sample, y correlación L/R.
- **Ataque** del 10 al 90 %, frente al del sample.
- **Clic al empezar y al soltar:** la pendiente frente a la de alrededor. Sirve para comparar versiones, no como veredicto: un golpe seco ya tiene una pendiente alta.
- **Cola** que queda 0,3 s después del release.

### Línea base (Reanima 0.9.5, 22-11; `linea_base/calidad.csv`, resumen en `linea_base/resumen_calidad.txt`)

| Motor | Fidelidad en la raíz, mediana / peor | Peor caída, mediana | Suma a mono frente al sample, peor |
|---|---|---|---|
| A | 0,00 / 0,0 dB | −2,3 dB | −2,6 dB |
| B | 0,01 / 48,9 dB | −3,1 dB | −3,0 dB |
| C | 2,04 / 11,1 dB | −3,7 dB | −2,1 dB |
| D | 0,01 / 30,4 dB | −3,1 dB | −2,9 dB |
| E | (no aplica) | −6,9 dB | −3,1 dB |
| F | (no aplica) | −4,5 dB | −4,6 dB |

- **Motor C por categoría** (error espectral mediano en la raíz):

  | Categoría | dB |
  |---|---|
  | Colas largas | 0,32 |
  | Graves | 0,42 |
  | Sencillos | 0,44 |
  | Sostenidos | 1,30 |
  | Pistas de canciones | 1,32 |
  | Transitorios | 1,81 |
  | Estéreo y fase | 2,60 |
  | Acordes | 2,84 |
  | Ruido | 3,12 |
  | Difíciles | 3,16 |
  | Loops densos | 3,66 |
  | Evolución tonal | 3,87 |

- **Caídas en A–D por caso** (mediana / peor), frente a la mediana de alrededor; incluyen el vibrato y el decaimiento propios del sonido:

  | Caso | Mediana | Peor |
  |---|---|---|
  | +7 | −3,5 dB | −27,5 dB |
  | −12 | −3,5 dB | −17,4 dB |
  | Suave | −3,0 dB | −19,7 dB |
  | Acorde | −4,7 dB | −27,0 dB |

- **Llamativo (sin escuchar todavía):**
  - B en la raíz cae por debajo de −40 dB, donde el sample suena, en 17 muestras (D en casi las mismas).
    - Afectadas: 7 risers y efectos de subida, una atmósfera vocal larga, la tetera, 4 loops de hats/tops, «Fallen – Texture» y un stab de metales.
    - En E y F, las caídas frente al sample no cuentan: no siguen su envolvente por diseño.
  - Un loop de bajo cortado a −12 st hace un clic al soltar de +55 dB en A, B y D.
  - F suma a mono con hasta −4,6 dB (correlación −0,32) en un pluck a −12 st.

### Cómo usarla en cada cambio futuro
```
ia/venv/Scripts/python.exe -I research/biblioteca_2026-11-22/scripts/regresion.py pasar build/ReanimaBatch_artefacts/Release/ReanimaBatch.exe nuevo.csv
ia/venv/Scripts/python.exe -I research/biblioteca_2026-11-22/scripts/regresion.py comparar research/biblioteca_2026-11-22/linea_base/calidad.csv nuevo.csv
```
- **Qué cuenta como empeorar** (umbrales por encima del ruido de la medida):

  | Medida | Umbral |
  |---|---|
  | Fidelidad | +0,5 dB |
  | Caída o subida | 3 dB |
  | Suma a mono | 1 dB |
  | Clics y cola | 6 dB |
  | Ataque | el doble de lento |
  | Valores | cualquier no finito |

- **Tarda** unos 3 minutos con 8 procesos.
- **Bancos anteriores, con su línea base sobre la misma lista:**
  - `ReanimaBatch loops`: tempo y cortes;
  - `tonalidad`: tonalidad nombre/sonido;
  - `resintesis`: motor C frente al sample.

## 5. Índices y datos (todo en `research/biblioteca_2026-11-22/`)
- **`indice_biblioteca.csv`:** los 7369 archivos (31 columnas, fuente de cada una entre corchetes).
  - Contenido: categoría, tipo, duración, formato, tonal y nota de Reanima, ataque, cola, golpes por segundo, BPM y tonalidad del nombre, problemas, duplicados, variantes, canción, si está en la biblioteca de referencia y para qué motores y funciones sirve.
- **`indice_nuevos.csv`:** los 200 nuevos, con lo anterior y además:
  - polifonía medida;
  - carácter;
  - BPM y tonalidad por el sonido, y si coinciden con el nombre;
  - tiempos y cortes de Beats.
- **`inventario.csv/json`:** el inventario completo de 93 columnas (las 91 del 13-11, más «nuevo» y la versión de Reanima), de las dos carpetas.
- **Conjuntos:**
  - `conjuntos/`: los 12 conjuntos del 13-11 sin cambiar, para que las comparaciones antiguas sigan valiendo, más `referencia_calidad`.
- **Canciones y bancos:**
  - `canciones_nuevas.csv`: las pistas agrupadas por canción;
  - `nuevos_loops.csv` y `nuevos_tonalidad.csv`: los bancos sobre lo nuevo.
- **`linea_base/`:** calidad, loops, tonalidad y resíntesis de la biblioteca de referencia.

## 6. Oportunidades que abre este material
1. **Tempo por el sonido: el sesgo de 3/4** (medido en 39 pistas). La elección del número de tiempos prefiere 12, 24 o 48 cuando lo cierto es 16, 32 o 64. Proponer que esos recuentos solo ganen con pruebas de compás ternario.
   - **Banco:** `nuevos_loops.csv` (102 pistas con BPM en el nombre).
   - **Importancia:** afecta a los archivos sin tempo en el nombre y a los exportados del DAW (los «`deluxe … insert 6`»).
2. **Motores B y D con ataques lentos** (17 muestras, sobre todo risers). Hipótesis: el mismo fallo que hoy se arregló en el motor C.
   - B y D dividen la envolvente pedida por el modelo del sample, y ese modelo tiene un suelo de −60 dB que la envolvente no tiene; en una subida larga y curvada la nota queda casi muda.
   - Sin comprobar todavía.
   - **Banco:** las 17 de `linea_base/calidad.csv` (caso raíz, `worstDipDb` < −40).
3. **Resíntesis (C) en evolución tonal, ruido y loops densos:** 3–4 dB de error, frente a 0,3–0,4 dB en notas limpias. Es lo siguiente a atacar si se sigue con el motor C, con la referencia como banco.
4. **Motor F y la suma a mono:** decorrelación hasta contrafase. Valorar un control de anchura, o limitarla en graves.
5. **Pistas con mucho silencio inicial y archivos largos:** el inicio detectado, la longitud del loop y el límite de 120 s (y de 70 s para la copia sin fundidos de LOOP).
6. **Tonalidad y acordes por la entrada lateral con stems reales:** las pistas de un mismo tema dan una prueba que no teníamos: el bajo y las teclas de «reflejo» o «madness» con la tonalidad que dice su nombre.
7. **Limpieza opcional:** los 78 `._*.wav` no son audio. Son inocuos para Reanima (no los abre), pero ensucian las listas. Borrarlos o no lo decides tú; no los he tocado.

## 7. Escucha pendiente
- Nada de este informe se ha valorado de oído.
- **Propuesta:** antes de cambiar nada, escuchar el motor B en los risers donde mide silencio y el F a −12 st en mono.
- Para eso se pueden sacar WAV A/B de la referencia con `ReanimaBatch resintesis <lista> <csv> <carpeta wav>`, o con un modo que escriba los casos del banco «calidad».

---

## 8. Primer uso: motores B y D en risers (22-11, mismo día)
- **Causa, comprobada:**
  - B y D dividen la envolvente pedida por el modelo del sample, y ese modelo se queda en −60 dB.
  - En una subida larga y curvada (un riser de 4,2 s con curva 3) la envolvente pedida baja de ese suelo y la división se hunde: la nota sonaba muda sus primeros 0,3 s (−120 dB donde el sample está a −56).
- **Arreglo** (`modelBelowFloor` en `SynthEngine.cpp`):
  - mientras la tecla está pulsada y sin Aplanar, por debajo del suelo se usa el valor real del modelo;
  - por encima del suelo, o tras soltar, nada cambia; la transposición conserva su envolvente en tiempo real.
- **Motor C:** se probó lo mismo y empeoraba sus notas transportadas (35 colas más largas a +7). Se queda exactamente como se aprobó de oído por la mañana.
- **Medido con la biblioteca de referencia** (`regresion.py comparar linea_base/calidad.csv linea_base/calidad_bd6.csv`):
  - casos de B/D en la raíz con caídas de más de 20 dB: de 36 a 2;
  - mejoran 42 caídas, 15 de fidelidad y 65 arranques; el motor C no cambia en nada.
  - **Lo que el comparador marca como «peor»:**
    - **241 arranques:** 199 quedan iguales al motor A (el sample tal cual) y el resto, entre el valor de antes y el de A. B y D suavizaban sin querer el arranque del propio sample.
    - **39 ataques más lentos:** 30 iguales a A.
    - **7 caídas:** 5 iguales a A.
  - **Quedan:** «Fallen – Texture» y la tetera en B, con una caída corta al principio mientras entran sus granos.
- **Escucha A/B:** `release/ab_risers_2026-11-22`, 10 tríos (original, motor B antes, motor B ahora).
- **Herramienta nueva:** `scripts/frente_a_A.py <base> <nuevo>` separa lo que empeora de verdad de lo que ahora suena como el sample.
- **Escucha del usuario (22-11):** tras el A/B, «Sí, se queda». El arreglo de B/D queda aprobado.

## 9. Segundo uso: el tempo por el sonido a 3/4 (22-11)
- **Causa, comprobada** (`detectLoop` en `SampleAnalysis.cpp`):
  - Para reconocer una longitud «de DAW» (un número entero de tiempos a un tempo entero o de medio), se recorrían los recuentos de menor a mayor y ganaba el primero que encajaba, sin mirar los golpes.
  - Una pista de 33,1 s a 116 BPM son 64 tiempos, pero también 48 a 87,0. El 48 se miraba antes y ganaba: de ahí el 3/4 (o 2/3, 120 → 80), y también muchos tempos a la mitad.
- **Arreglo:** se apuntan todos los recuentos exactos, y se elige el que mejor encaja con la rejilla de golpes, con la preferencia de siempre por 4, 8, 16, 32 y por un tempo cercano a 120. Sin golpes que lo digan, se elige un recuento de 4, 8, 16, 32 o 64.
- **Medido** (`research/tempo_2026-11-22`, `comparar_tempo.py`), el tempo leído solo del sonido frente al del nombre:

  | | 158 pistas nuevas con tempo en el nombre | 147 loops de `loop_tempo` (13-11) |
  |---|---|---|
  | Acierta | 17 → 73 | 20 → 107 |
  | Doble o mitad | 46 → 14 | 73 → 13 |
  | 3/4 | 31 → 6 | 39 → 8 |
  | 2/3 | 2 → 4 | 0 → 10 |

  - Empeoran algunos loops lentos (80 → 160, 65 → 130) y un loop en tresillos (124 → 93).
- **Con el nombre** (lo que Reanima usa cuando lo hay): igual en 150 de 150 loops de la biblioteca y en 197 de 201 pistas nuevas.
  - Las 4 que cambian no tienen un tempo que el lector de nombres reconozca:
    - `hm_drm118_08_kick` pasa de 88,5 a 118, lo que dice su nombre;
    - tres pistas de «Fallen» pasan de 70,5 a 94, sin dato con que comprobarlo.
- **Pruebas:** motor (la medida de CPU de Mecanismo falló una vez por ruido y pasa sola dos veces seguidas), tiempo real, auditoría 515/515, LOOP 103/103. La biblioteca de referencia no cambia en nada.
