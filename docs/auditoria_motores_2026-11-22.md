# Auditoría de calidad sonora de los motores A–F (22-11-2026)

**Encargo:** fidelidad al sample sin perder lo que cada motor hace bien. Este informe es la primera parte: **referencia, diagnóstico y propuestas**. No se ha cambiado ningún sonido todavía.

Datos y scripts en `research/motores_2026-11-22/`; bancos en `tests/Batch.cpp`.

> **Cómo leer los datos**
> - **[medido]**: calculado sobre el audio.
> - **[hipótesis]**: causa probable, sin demostrar todavía.
> - **Escucha:** nada de este informe se ha valorado de oído. Cada cambio se presentará con su A/B de volumen igualado antes de darlo por bueno.

## 0. Referencia y métodos
- **Tres bancos**, con la versión instalada hoy (la referencia sonora de partida):
  1. **`ReanimaBatch calidad` sobre la biblioteca de referencia** (120 muestras reales, 12 categorías; `research/biblioteca_2026-11-22/linea_base/`).
     - Cada motor contra el sample, en la raíz, +7, −12, velocidad suave y acorde.
     - Mide fidelidad espectral, caídas, mono, ataque, clics y cola.
  2. **`ReanimaBatch motores_tonal` sobre 78 samples tonales en 12 familias** (`tonal_antes.csv`): plucks, pianos acústicos, eléctricos y órganos, cuerdas pulsadas y frotadas, vientos, voces, bajos, pads, sintes, acordes, mazos y campanas.
     - Cada motor contra el motor A en el mismo caso (raíz, +5, +12, −7, −12).
     - Al transportar, B y D mantienen la envolvente en tiempo real y A no (es un sampler clásico). Por eso fuera de la raíz parte de la diferencia es de diseño, no de defecto, y este banco se usa sobre todo para ver familias.
  3. **`ReanimaBatch motores_sinteticos`** (`sinteticos_antes_*.csv`): sonidos cuyo resultado ideal se conoce en cualquier transposición, para separar lo que añade un motor de lo que hace a propósito.
     - Los sonidos: un tono de 220 Hz, un bajo de 55 Hz, un pulsado que decae, el tono con aliento (ruido a −30 dB que debe conservarse) y un pad ancho (estéreo con voces desafinadas).
     - Lo que mide: error de armónicos, ruido entre armónicos, temblor de nivel, afinación y suma a mono.
     - Se repite a 44,1, 48 y 96 kHz, y con bloques de 32, 512 y 2048 muestras.
- **Lo que ya cubrían auditorías anteriores y sigue pasando** (`ReanimaAudit`, 515 comprobaciones):
  - lectura con sinc (espurias < −95 dB, plegado < −98 dB al transportar);
  - «sin tocar = archivo original» con 131 dB de nulo;
  - sin asignaciones en el hilo de audio;
  - latencia cero;
  - filtros iguales a cualquier frecuencia de muestreo.

### Resultados comunes a todos los motores [medido]
- **Tamaño de bloque:** resultado idéntico, bit a bit en las medidas, con bloques de 32, 512 y 2048. No hay dependencia del buffer.
- **Frecuencia de muestreo:** A, B, C, D y E son estables entre 44,1 y 48 kHz (±0,5 dB). F varía su nivel hasta 4 dB porque sus fases son aleatorias (ver F).
- **CPU por voz** a 48 kHz (milisegundos de cálculo por segundo de sonido; 10 ms/s = 1 % de un núcleo):

  | Motor | ms/s |
  |---|---|
  | A | ≈ 4 |
  | B | ≈ 10–15 |
  | C | ≈ 10 |
  | D | ≈ 10–15 |
  | E | ≈ 6 |
  | F | ≈ 22 |

  - A 96 kHz, entre 1,5 y 2 veces más.
  - Ninguna propuesta de abajo cambia esto salvo donde se dice.

---

## A · Reconstrucción
**Arquitectura:** toca el archivo con una sinc de Kaiser. Cuando la envolvente pide más de lo que el sample tiene (un sustain más largo, o una nota transportada hacia arriba que se acaba antes), lo completa con lechos reconstruidos: bucles sin costura de una región del sample, con fundidos e igualados de color (`Reconstruct.cpp`).

**Fortalezas que hay que conservar** [medido]:
- en la raíz y con los mandos detectados es el archivo exacto (fidelidad 0,00 dB, nulo de 131 dB);
- transitorios, estéreo y mono idénticos al sample;
- transportado, mientras dura el sample, armónicos exactos (error 0,0 dB) y sin ruido añadido.

**Problemas** [medido, sonidos sintéticos sostenidos 3 s]:
- **Cuando el sample se acaba y entra el lecho** (+5, +12):
  - **Ruido entre armónicos:** −34 dB (la fuente: −80) en el tono de 220 Hz, y −28 dB (la fuente: −48) en el bajo de 55 Hz.
  - **Temblor de nivel:** 2,7 dB en el tono y **6,5–8,6 dB en el bajo** (se oiría como un trémolo o un batido).
  - Hacia abajo no pasa: el sample dura más que la nota.

**Causa** [hipótesis]:
- El lecho de un grave no tiene una longitud múltiplo del periodo y sus fundidos cruzan ondas desfasadas. El propio código ya documenta cancelaciones de ese tipo y las compensa a posteriori con `fillDips`.
- Con 55 Hz el periodo es de 18 ms y los fundidos no bastan.

**Propuesta A1:** alinear la longitud y los cruces de los lechos al periodo de la nota cuando el sample tiene nota.
- Afecta solo a la parte reconstruida, no al sample.
- No cambia el carácter, y el coste es de análisis al cargar, no en tiempo real.
- **Recomendada.**

## B · Reenvolvente · D · Híbrido (comparten el lector)
**Arquitectura:**
- Lee el sample con granos de Hann limitados en banda (sinc), estirados para seguir la envolvente pedida.
- Al llegar al sustain cede a una fuente sostenida: Loop, Granular o Freeze.
- D hace lo mismo y conserva el transitorio original.

**Fortalezas** [medido]:
- en la raíz son tan fieles como A (0,00 dB tras el arreglo de los risers de hoy);
- mono y estéreo intactos;
- la envolvente en tiempo real al transportar (diseño);
- sostienen mejor que A más allá del sample: ruido −39 a −47 dB y temblor 0,4–1,2 dB, frente al lecho de A.

**Problemas** [medido]:
1. **Pulsado transportado hacia arriba:** ruido entre armónicos de −52 a −56 dB (A: −89 a −96).
   - Es bajo, pero se suma a lo que dicen las familias reales: con 808 y pianos a +12, hasta 16–26 dB menos de relación armónicos/ruido que A en algunos casos.
2. **Pad ancho (estéreo de voces desafinadas) a +12:** la suma a mono pierde 4,7 dB (la fuente, 2,7).
3. **Timbre a ±12 en familias reales:** 4–6 dB de diferencia con A en cuerdas frotadas, pianos eléctricos y voces. Parte es de diseño (la envolvente en tiempo real), y no está separado todavía de defecto.

**Causa** [hipótesis]:
- Los granos se sincronizan con el periodo solo mientras se estira el ataque o el decay (`stretching`).
- Al transportar con la envolvente en tiempo real, la ganancia por muestra (envolvente ÷ modelo) cambia dentro de cada grano, y los granos solapados se modulan: bandas laterales, el «granulado».

**Propuesta BD1:** medir la parte que es defecto con el pulsado sintético en tiempo real.
- Si se confirma: suavizar la ganancia a la escala del grano y sincronizar los granos al periodo también al transportar sonidos con nota.
- No cambia el carácter. **Recomendada**, con escucha.

## C · Resíntesis
**Arquitectura:**
- Hasta 128 parciales seguidos desde un fotograma de referencia, y ruido por 24 bandas del residuo.
- El transitorio original, mezclado al principio.
- Hoy se le han hecho dos arreglos: ataque lento y tope de energía; el ruido se mide también entre los parciales.

**Fortalezas** [medido]:
- **Familias reales:** es el más estable de B–F frente a A en todas, con timbre 0,3–2,0 dB y envolvente 0,2–1,3 dB.
- **Transposición:** limpio (error de armónicos ≤ 0,4 dB).
- **Mono:** nunca pierde nada al sumar a mono.
- **Afinación:** temblor bajo, salvo en voces.

**Problemas** [medido]:
1. **Más allá del final del sample, al transportar hacia arriba:** ruido entre armónicos de **−18 a −27 dB** (la fuente: −80).
   - Es el peor caso de C. Se oye como un soplo que crece en la cola de notas agudas sostenidas.
2. **Estéreo ancho:**
   - Colapsa la anchura: en el pad ancho, la suma a mono pierde 0 dB cuando la fuente pierde 2,7 dB. Es decir, C lo ha hecho casi mono.
   - Además añade ruido: −48 dB (la fuente: −77), porque las dos voces desafinadas (220 y 220,8 Hz) caen en el mismo parcial y lo hacen latir.
3. **Aliento:** en la raíz quita 3,4 dB del ruido que forma parte del sonido (el tono con aliento: −47,9 frente a −44,5).
4. **Voces:** temblor de afinación +10,7 cents frente a A, y timbre 2 dB (la familia más difícil para C).

**Causas** [hipótesis]:
1. **Más allá del final:** el ruido se extrapola desde el último fotograma con la caída del sample, mientras los parciales siguen con la suya. Al subir la nota se acaba antes y queda casi solo el ruido.
2. **Estéreo:** cada parcial tiene un solo panorama fijo.
3. **Aliento:** efecto del arreglo de hoy (el ruido se mide también entre los parciales). Es el precio de no añadir ruido en otros 39 sonidos.
4. **Voces:** parciales con vibrato rápido seguidos fotograma a fotograma.

**Propuestas:**
- **C1:** que el ruido se extinga al mismo ritmo que los parciales al prolongar la nota. Fidelidad, sin cambio de carácter. **Recomendada.**
- **C2:** resíntesis por canal (L/R) cuando el sample es estéreo de verdad (correlación baja). Recupera la anchura y quita el latido.
  - **Dobla la CPU de C**, de ≈ 10 a ≈ 20 ms/s por voz, y cambia cómo suena el estéreo de C.
  - **Hay que decidirlo.**
- **C3:** recuperar el aliento solo en bandas sin parciales y por encima de 2 kHz, sin deshacer el arreglo de hoy. Pequeña; se mide en los 39 sonidos de entonces.

## E · Granular
**Arquitectura:** nube de granos (densidad, tamaño, posición, ancho, pitch aleatorio) sobre una región del sample; es un motor de textura.

**Fortalezas:** su carácter de nube, el control creativo y un CPU bajo (≈ 6 ms/s).

**Problemas** [medido]:
- **Bajo de 55 Hz:** ruido entre armónicos de **−9 dB**: el sonido es casi todo ruido de granos.
- **Tono de 220 Hz:** −43 dB.
- **Familias reales:** bajos 19 dB peor que A, y temblor de afinación de 68–143 cents en bajos y acordes.

**Causa** [comprobada por diseño]:
- Los granos (80 ms) se colocan sin relación con el periodo, y los solapados se suman con fases arbitrarias.
- En graves el grano contiene pocos periodos y la cancelación es máxima.

**Propuesta E1:** colocar los granos a un número entero de periodos cuando el sample tiene nota, como ya hace B al estirar. Cambia el carácter de E: menos nube y más nota.
- Puede hacerse como opción («Coherente», apagada por defecto), o solo por debajo de una frecuencia (los graves).
- **Hay que decidirlo.**

## F · Drone Maker
**Arquitectura:** PaulStretch: FFT muy largas con fases aleatorias, lectura hasta 1000 veces más lenta, armónicos, desplazamientos, curva dibujada. Los graves (< 120 Hz) se mantienen en el centro.

**Fortalezas:** su idea (un dron sin ataque ni ritmo), graves compatibles con mono y sus controles creativos.

**Problemas** [medido]:
- **Suma a mono:**
  - con el Estéreo por defecto (100 %), pierde 2,6–3,6 dB en tonos y 3,1–5,3 dB en el pulsado;
  - en la biblioteca, hasta −4,6 dB, con correlación negativa (contrafase) en algunos casos.
- **Nivel:** varía 1–4 dB entre frecuencias de muestreo y entre renders, por las fases aleatorias.
- **Temblor de nivel:** de 4 dB en el pulsado.

**Causa** [comprobada por diseño]: fases aleatorias independientes por canal por encima de 120 Hz.

**Propuestas:**
- **F1:** igualar el nivel de salida al del sample: medir la potencia del espectro y compensar. Sin cambio de carácter. **Recomendada.**
- **F2:** evitar la contrafase: que la correlación entre canales no baje de un mínimo (0 o +0,3) en medios, conservando la anchura. O bajar el Estéreo por defecto al 70 %.
  - Cambia la anchura de F.
  - **Hay que decidirlo.**

---

## Lo que no se toca
- **Lectura sinc y filtros:** limpios y medidos.
- **Independencia del buffer.**
- **Comportamiento de los mandos y compatibilidad de presets y proyectos:** ninguna propuesta añade ni cambia parámetros, salvo las opciones que decidas (E1 y, quizá, C2).
- **La envolvente en tiempo real de B/D al transportar:** es diseño.

## Plan de validación de cada cambio
1. **Medidas, antes y después:**
   - biblioteca de referencia (`regresion.py` + `frente_a_A.py`);
   - 78 tonales (`motores_tonal`);
   - sintéticos a 44,1, 48 y 96 kHz;
   - comprobar que percusión, loops, texturas y FX (categorías 2, 6 y 7 de la referencia) no empeoran.
2. **CPU** con `ReanimaCpuBench` y `RtCheck`.
3. **Escucha:** A/B de volumen igualado para tu escucha (original / antes / después), por familia.
4. **Se queda solo lo que confirmes de oído.**

## Decisiones del usuario (22-11)
- **C2:** resíntesis por canal en samples con estéreo de verdad. Sí.
- **E1:** como opción «Coherente», apagada por defecto.
- **F2:** evitar la contrafase (correlación mínima 0 en medios).
- **Recomendadas (fidelidad, sin cambio de carácter):** A1, BD1, C1, C3 y F1, en ese orden de trabajo.

---

## Resultados de la implementación (22-11, instalado; pendiente de escucha)

| Propuesta | Estado | Medido |
|---|---|---|
| **A1** | No era defecto | A con la envolvente detectada toca el archivo y se para al acabarse, como un sampler. El «temblor» medido era ese final dentro de la ventana de medida. Nada que cambiar. |
| **BD1** | No se cambia | Lo que queda en el pulsado transportado es −45 dB, al final del decay y bajo la fuga de la ventana. Inaudible a efectos prácticos. |
| **C1** | Hecho | Ruido de C al prolongar una nota transportada en archivos cortados. |
| **C2** | Hecho | Resíntesis por canal en samples anchos. |
| **C3** | No se cambia | Investigado: C reproduce el ruido solo a su nivel exacto (−46,9 frente a −47,2 dB). El aliento que pierde con tono (3,4 dB) lo absorben los parciales en sus propias bandas, y corregirlo devolvería el ruido de más a los 39 sonidos arreglados por la mañana. |
| **E1** | Hecho | Opción «Coherente» en E, apagada por defecto. |
| **F1** | No era defecto | El nivel por canal es correcto; los −3 dB eran la suma a mono de canales descorrelados. |
| **F2** | Hecho | Sin contrafase en F. |

### C1: ruido de C al prolongar la nota
- **Causa, comprobada:** C prolongaba desde el último fotograma de análisis, cuya ventana cuelga fuera del archivo; su residuo, que se toma por ruido, sale enorme.
- **Arreglo:** en archivos cortados mientras suenan (último fotograma a menos de 30 dB del pico), el ruido sale del último fotograma entero y cae con los parciales. Lo demás no cambia.
- **Medido:** tono sostenido a +12, ruido entre armónicos de −18 a −44 dB. Biblioteca de referencia sin cambios.
- **Pendiente:** queda el bajo. Una versión más agresiva lo arreglaba (−60 dB), pero alargaba la cola de 3 muestras reales y se descartó.

### C2: resíntesis por canal en samples anchos
- **Cuándo:** correlación entre canales menor que 0,3. En la biblioteca de tonales, 31 de 78 samples.
- **Cómo:**
  - el mismo parcial en los dos lados comparte frecuencia, para que no derive y se cancele;
  - y no se separan más de un cuarto de ciclo, para que no queden en contrafase.
  - «Parciales» cuenta por lado. La CPU sube ≈ 10 % en esos samples.
- **Medido, canal por canal, frente al original:**

  | | Antes | Ahora | Original |
  |---|---|---|---|
  | Nivel de cada lado | −2 a −3 dB | ±0,3 dB | — |
  | Correlación entre canales | 0,91–0,97 (casi mono) | 0,02–0,13 | −0,20 a 0,21 |

  - Timbre por canal, en la biblioteca: −0,67 dB de error con correlación < 0,3. En el pad sintético ancho, 0,05–0,19 dB por canal, y el ruido de latido de −48 a −75 dB.
- **El precio:** en las medidas de la suma a mono, el timbre empeora ≈ 1 dB y el ataque medido sobre la suma tarda más. Los 101 casos «peores» de mono quedan iguales al sample.
- **Escucha:** es el cambio que más necesita tu oído.

### E1: opción «Coherente» en E
- **Cómo:** granos a un número entero de periodos y a intervalos regulares. Nuevo parámetro `grainCoherent`, al final de la lista: los presets y proyectos no cambian.
- **Medido** (sintéticos):

  | | Ruido entre armónicos | Desafinación |
  |---|---|---|
  | Bajo de 55 Hz | de −9 a −37 dB | de 500–1000 cents a < 1 |
  | Tono | de −43 a −68 dB | |
  | Pulsado | | armónicos de 2,6 a 0,04 dB de error |

- **Apagada:** E es idéntico.

### F2: sin contrafase
- **Cómo:** la fase entre canales queda dentro de ±¼ de ciclo.
- **Medido, en la biblioteca:**
  - suma a mono de F: mediana de −2,41 a −0,86 dB, y la peor de −4,69 a −2,94 dB;
  - correlación mínima de −0,32 a 0,00.
  - Los armónicos, mejor también. 15 ataques y 4 caídas cambian por el azar de las fases.

### Validación
- **Pruebas:** motor, tiempo real, auditoría 515/515, LOOP 103/103, interfaz (con el nuevo interruptor), carga del VST3.
- **Motores que no cambian en ningún caso:** A, B y D.
- **Escucha A/B de volumen igualado** en `release/ab_motores_2026-11-22`:
  - `motor_C`: 8 samples anchos;
  - `motor_E`: 6 (normal frente a Coherente);
  - `motor_F`: 4 (escuchar también en mono).
- **Copia de seguridad:** `backups/instalado_antes_motores_2026-11-22`.

- **Escucha del usuario (22-11):** tras el A/B de C, E y F, «Todo se queda». Los cambios quedan aprobados.
