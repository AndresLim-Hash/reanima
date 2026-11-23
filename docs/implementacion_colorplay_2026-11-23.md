# Implementación de las cuatro mejoras de la investigación Color Play (23-11-2026)

**Encargo:** implementar las cuatro propuestas elegidas de `docs/investigacion_colorplay_2026-11-23.md`. Cada una se prueba por separado, después juntas y en los seis motores, sobre la biblioteca «all samples».

**Estado:**
- Compilado en una carpeta aparte, `build_cp/`, porque había otro `ReanimaAudit` en marcha usando `build/`.
- **No está instalado ni subido a git.** El plugin instalado en Ableton sigue siendo el anterior.
- Datos, scripts y capturas: `research/colorplay_2026-11-23/`.
- Archivos de escucha: `release/escuchar_colorplay_2026-11-23/`.

> **Cómo leer los datos**
> - **[medido]**: calculado sobre el audio.
> - **[hipótesis]**: interpretación sin demostrar.
> - **Escucha:** nada se ha valorado de oído. Las decisiones de sonido quedan para tu A/B.

---

## 1. Correspondencia con las propuestas originales

| Tu elección | Propuesta original | Qué se ha hecho | Decisión tuya (23-11) |
|---|---|---|---|
| Filtro armónico | **CP1**: filtro por voz «Armónico» | Nuevo tipo **Armónico** en el filtro de cada voz, en los seis motores | — |
| Sostenido en B y D | **CP3**: el sostenido recupera la evolución de color del sample | Mando **Movimiento** en B y D, más la revisión de energía del sostenido | «CP3 + revisar energía» |
| LFO DERIVA | **CP7**: forma «Deriva» en los LFO | Forma **Deriva** en los 3 LFO, mando **Suavidad** en cada LFO y destino **Tono fino** en la matriz | «Forma + Suavidad» |
| Sidechain que colorea | **CP2**: «Impronta» con la entrada lateral | Efecto **Impronta** en el rack de FX y fuente **Sidechain** en la matriz, con su panel | «Impronta + fuente» |

---

## 2. Qué hace cada función y dónde está

### 2.1 Filtro Armónico (CP1)
**Dónde:** FILTRO · MOD → Tipo → Armónico.

**Qué es:** hasta 32 resonancias, colocadas en los armónicos (1×, 2×, 3×… la frecuencia) de la nota que suena en ese momento. Siguen el bend, el vibrato, el glide y la modulación de tono.

**Cómo actúa:**
- Cada resonancia suma o resta una banda estrecha del propio sonido, como un ecualizador de picos.
- A 0 dB deja el sonido igual.
- Lo que hay entre armónicos (ruido, golpe) pasa sin tocar.
- La fundamental nunca se baja.
- El volumen se mantiene: la salida sigue la potencia de la entrada (80 ms).

**Mandos:**

| Mando | Qué hace |
|---|---|
| **Realce** (nuevo, −24…+24 dB, 9 dB) | Sube o baja los armónicos de la región |
| **Región** (nuevo, 0,5…10 octavas, 4) | Anchura de la zona trabajada, alrededor de Frecuencia |
| **Frecuencia** | Centro de la región; sigue a Keytrack, Env 2, Follower y la matriz |
| **Resonancia** | Lo estrecho de cada pico |
| **Pares/impares** | Es el mismo parámetro Morph/DJ, con otra etiqueta en este modo: 0 % impares, 50 % todos, 100 % pares |
| **Dry/Wet** | Mezcla con el sonido sin filtrar |

- En la matriz, «Drive filtro» mueve Realce y «Morph filtro» mueve Pares/impares.
- Drive y Circuito no actúan en este tipo: se ocultan o se atenúan.
- La curva del panel dibuja la respuesta sobre una nota La1 (110 Hz).

**Archivos:** `Source/Filters.h` (`setHarmonic`, `processHarmonic`) y la llamada por voz en `Source/SynthEngine.cpp` (`postProcess`).

### 2.2 Movimiento en el sostenido de B y D (CP3)
**Dónde:** SAMPLE → MOTOR, motores B y D, mando **Movimiento** (0 por defecto). Solo actúa con Sostenido.

**Análisis al cargar:** el color del sample cuadro a cuadro en 10 bandas de octava (31 Hz a 16 kHz) y el color de cada fuente de sostenido (`SampleData::Colour`).

**Durante la nota:**
- La parte de la fuente de sostenido se ecualiza con 10 campanas de octava hacia el color que tenía el sample en el punto que la nota ha alcanzado.
- Después del último cuadro claro, se mantiene ese color.
- Solo cambia la forma del espectro: la potencia se compensa (300 ms).
- Entra con el paso al sostenido.
- El transitorio original de D no se toca.

**B y D usan el mismo algoritmo.** Comparten el mismo lector (`renderSampled`), así que no hacía falta una solución distinta.

**Archivos:** `MotionEq` y `applyMotion` en `Source/SynthEngine.cpp`; `measureColour` en `Source/SampleAnalysis.cpp`.

### 2.3 LFO Deriva y Suavidad (CP7)
**Forma «Deriva»** (7.ª forma de cada LFO):
- Puntos aleatorios unidos por una curva suave (sin esquinas, sin clics), más dos capas más rápidas para el temblor.
- **Cada nota deriva distinto**, como las voces de un analógico.

**Lo que ya existía y se reutiliza:**
- Velocidad, Sync/División: deriva al tempo del DAW o libre.
- Retrig.
- La cantidad y los destinos de la matriz.
- El aro de los mandos muestra la posición modulada.

**Mando «Suavidad»** (nuevo en cada LFO, 0 por defecto):
- En Deriva quita el temblor rápido.
- En las demás formas redondea esquinas y saltos.
- A 0 las formas antiguas quedan exactamente igual.

**Destino nuevo «Tono fino»** (±100 cents):
- Con «Tono», que mueve ±24 semitonos, una deriva sutil caía en el primer 1 % del mando y no se podía ajustar.
- Con Tono fino al 15 %, la deriva es de ±15 cents.
- El aro de Fino lo muestra.

### 2.4 Sidechain que colorea (CP2 + fuente)
**Efecto «Impronta»** (rack FX, cualquier ranura):
- Divide la pista lateral en 8 a 24 bandas, sigue su nivel con Ataque y Release, y da a Reanima esa forma con una cadena de campanas.
- Tres respuestas:

| Respuesta | Qué hace |
|---|---|
| **Color** | Solo el timbre; el volumen se compensa. Pista lateral en silencio: sonido intacto |
| **Vocoder** | Timbre y volumen |
| **Invertido** | Suena la pista lateral con el timbre y el volumen de las notas de Reanima |

- Mandos: Profundidad, Sensibilidad, Ataque, Release, Bandas, Inclinación, Desplazar (formantes ±24 st), Desde y Hasta.
- Sin entrada lateral deja el sonido igual, bit a bit.

**Fuente «Sidechain» de la matriz y panel SIDECHAIN** (página MATRIZ):
- **Respuesta:** Nivel, Graves, Medios, Agudos o Brillo.
- **Sensibilidad, Ataque y Release.**
- Una barra muestra el valor en directo.
- Puede llevarse a cualquier destino: filtro, Morph (que en Armónico son Pares/impares), Drive (Realce del Armónico), granos, Respiración, Arco, velocidad de los LFO, etc.

**Formatos y hosts** (la declaración de buses es la que existía desde el 22-11; no se ha cambiado):

| Formato | Entrada lateral | Estado |
|---|---|---|
| VST3, Windows y Mac | Bus auxiliar mono o estéreo | En Live aparece en el panel Sidechain del dispositivo. **Sin probar en Live** |
| AU, Mac | El mismo bus | Logic lo ofrece como «Side Chain» en instrumentos. **Sin probar: no hay Mac aquí** |
| Aplicación independiente | No tiene, a propósito: sería el micrófono | El panel lo dice |
| Otros hosts | Según documentación de cada DAW, **sin probar** | Reaper: conexión de pines, canales 3/4. Cubase: side-chain de instrumentos VST3. FL Studio: los generadores no reciben audio con facilidad |

**Alternativa si un host no lo permite**, sin cambiar la finalidad: una versión «efecto» de Reanima (el mismo motor como efecto de inserción). No implementada; sería un producto aparte.

---

## 3. Compatibilidad
- **Parámetros nuevos:** 13. Son Suavidad ×3, Realce, Región, Movimiento y los 4 de Sidechain, más las opciones nuevas añadidas a menús ya existentes:
  - Armónico al final de Tipo de filtro;
  - Deriva al final de las formas;
  - Sidechain al final de las fuentes;
  - Tono fino al final de los destinos;
  - Impronta al final de los efectos.
- **Proyectos y presets guardados:** suenan igual. Se guardan por nombre de parámetro y los nuevos valen por defecto lo que equivale a «no hacer nada»:
  - Suavidad 0;
  - Movimiento 0;
  - Realce solo actúa con Armónico;
  - Sidechain solo si se elige en la matriz.
- **El dado (aleatorio)** no elige las opciones nuevas: usa listas cerradas.
- **Automatización:** los parámetros nuevos aparecen con prefijo, por ejemplo «Filtro Realce» o «Sidechain Ataque».

---

## 4. Pruebas y resultados

### 4.1 Comprobaciones automáticas [medido]

| Prueba | Resultado |
|---|---|
| `ReanimaAudit colorplay` (nueva, 33 comprobaciones de las cuatro funciones) | **33/33** |
| `ReanimaAudit fast` y completa (todas las frecuencias de muestreo, mandos muertos, efectos, resistencia) | **516/516** |
| `ReanimaTests` (pruebas unitarias) | **sin fallos** |
| `ReanimaRtCheck` (tiempo real, 3 samples, todos los motores) | **sin fallos** |
| Capturas de la interfaz (`ReanimaSnapshot`) | **UI OK**; capturas en `research/colorplay_2026-11-23/capturas/` |

**Lo que comprueba `colorplay`:**
- **Deriva:** se queda dentro de lo pedido (±20 cents al 20 %); se mueve de verdad (desviación de 10 cents); sin saltos (como mucho 3 cents cada 50 ms).
- **Suavidad:** en Deriva quita el temblor rápido (2,0 → 1,15 cents por paso); en una cuadrada redondea los saltos (7,1 → 0,7 dB cada 5 ms).
- **Armónico:**
  - +12 dB realza su región: +12,2 dB del 6.º armónico frente a la fundamental;
  - −12 dB la atenúa: −13,5 dB;
  - no baja la fundamental;
  - mantiene el volumen;
  - Pares/impares cambia el equilibrio en ±10 dB;
  - convierte un ruido en una nota (+18,8 dB de armónicos frente a lo que hay entre ellos);
  - sin clics en un glide.
- **Movimiento, en B y en D:** el sostenido se oscurece como el sample (centroide final/inicial 0,86 frente a 1,00 quieto); volumen −0,25 dB; sin saltos.
- **Sidechain:**
  - la fuente abre el filtro con los golpes (×2,13 con golpe, ×1,73 sin golpe);
  - se queda entre 0 y 1;
  - sin entrada lateral, el sonido es idéntico al de sin fuente.
- **Impronta:**
  - sin entrada, o con la pista lateral en silencio: el sonido queda idéntico;
  - una pista oscura oscurece 17 dB y una brillante abre 21 dB (agudos frente a graves);
  - en Color el volumen se queda en ±0,03 dB;
  - Vocoder calla con la pista lateral en silencio (−60 dB);
  - Invertido hace sonar la pista lateral.
- **Ninguna de las cuatro reserva memoria en el hilo de audio.**

### 4.2 Compatibilidad: el sonido de siempre, bit a bit [medido]
- **La prueba:** la herramienta de antes (copiada en `research/colorplay_2026-11-23/antes_bin/`) frente a la nueva, con los ajustes de siempre.
  - 3 samples (pluck, piano, voz) × 6 motores.
  - Además, una configuración con LFO S&H hacia Tono, seno hacia el filtro y Variación, en 4 motores.
- **Resultado: idéntico bit a bit en los 22 casos.**
- **Un fallo que esta prueba encontró y que ya está corregido:** la Deriva sacaba números del mismo generador aleatorio de la nota aunque no se usara. Eso cambiaba las tiradas aleatorias de Variación, los granos y las fases del motor F: el carácter era el mismo, pero no idéntico. Ahora tiene su propio generador.

### 4.3 Biblioteca «all samples» [medido]
**Qué se ha medido:**
- **Samples:** 82 (los 77 tonales de `tonales_motores` y 5 loops o texturas):
  - plucks, pianos, teclados, guitarras, arpas, sitar, metales, cuerdas, vientos y voces;
  - bajos y 808, pads, sintes, acordes, cajas de música y mazos;
  - loops melódicos y de batería.
- **Renders:** 3280, en los seis motores.
- **Pista lateral:** un loop de batería (`OSS_TL_82_drum_loop_full_pusherman`).
- **Datos:** `research/colorplay_2026-11-23/colorplay.csv`, resumidos en `resumen.txt` (script `resumen.py`).

**Cada caso frente al mismo sonido sin la función** (mediana de los 82 samples). El tono es la energía en los armónicos frente a la que hay entre ellos.

| Caso | Nivel | Tono | Brillo (centroide) | CPU por voz (ms por s de sonido) |
|---|---|---|---|---|
| Deriva (±15 cents) | 0,00 dB (peor −2,1 dB, motor E) | ±0,1 dB | igual | +0,1 a +0,6 |
| Armónico +9 dB | +0,1 dB (peor +2,7 dB) | **+1,5 a +1,9 dB** | ×1,06–1,11 | **+3,3 a +3,9** |
| Armónico −12 dB | 0,0 dB (peor −5,5 dB) | −0,2 a −0,3 dB | ×0,93–0,95 | +3,3 a +3,9 |
| Impronta (Color, global) | −0,25 dB (peor −2,7 dB) | — | ×0,84–0,90 (la batería es oscura) | +4,5 a +6,3 (una sola vez, no por voz) |
| Fuente Sidechain → filtro | +0,7 dB | — | ×1,05–1,08 | +0,6 |

**Movimiento** (B y D, Sostenido 6 s, Movimiento al 100 % frente a 0):
- **Nivel:** mediana −0,02 dB; extremos −2,7 y +1,0 dB.
- **Cuánto sigue el sostenido el color del sample** (correlación del centroide): sube de 0,06 a **0,44** de mediana; mejora en **47 de 70** samples con duración suficiente para medirlo.
- **Clics:** no aumentan (105 frente a 119 del sostenido actual).
- **CPU:** 9,6 → 12,8 ms/s por voz.

**Coste en el peor caso [hipótesis, a partir de lo medido por voz]:** con 32 voces, Armónico añade unos 120 ms/s, el 12 % de un núcleo.

### 4.4 Correcciones hechas durante las pruebas [medido]
1. **Movimiento subía el volumen** (+3 dB en un teclado oscuro). Las campanas de octava solapadas y los límites de ±12 dB no conservaban la energía. Ahora se mide la potencia antes y después y se compensa; el resultado es −0,02 dB de mediana.
2. **Impronta Color subía el volumen** (hasta +11 dB con una pista brillante). Se compensa de la misma forma: ahora ±0,03 dB.
3. **Armónico subía hasta +3,3 dB en plucks cortos.** La compensación de 300 ms era lenta para notas tan breves; ahora es de 80 ms y el peor caso queda en +2,7 dB (mediana +0,1).
4. **La Deriva cambiaba las tiradas aleatorias del resto** (sección 4.2).

### 4.5 Avisos y límites [medido / hipótesis]
- **Armónico −12 dB y los «clics» del detector:** el detector cuenta más clics en dos samples, el acorde `cw_chord_rust_Dm` (13) y la voz `de2_vox100_yo_Ab` (8).
  - [medido] En el acorde, esos instantes ya tienen chasquidos en el original: la segunda diferencia es 5 veces su entorno.
  - Al atenuar los armónicos y compensar el volumen, esos chasquidos quedan más expuestos (unas 2 veces).
  - No son discontinuidades nuevas: en un glide con tonos limpios no hay ninguna.
- **Deriva y los «clics»:** aparecen 1 o 2 más en samples que ya los tenían (`91V_..._malfunction`, un stab de metales). La afinación que se mueve desplaza dónde los cuenta el detector.
- **Armónico con acordes:** sigue la nota tocada. En un sample que ya es un acorde trabaja sobre los armónicos de la nota raíz [hipótesis: es lo esperable].
- **Entrada lateral:** sin probar en Live, Logic ni otros DAW (sección 2.4).

### 4.6 Revisión de energía del sostenido actual de B y D (la parte «+ revisar energía») [medido]
No he cambiado el comportamiento de siempre: cualquier cambio sonaría distinto en tus proyectos. Esto es lo que encontré:

1. **18 de 82 samples bajan más de 3 dB en los 0,4 s siguientes al paso al sostenido**, frente al nivel estable que viene después (sostenido Granular, que es el modo por defecto).
   - El peor, `AP2_Pluck_Windy_C`, llega a −12,9 dB.
   - Visto de cerca, no es un hueco: **el sostenido Granular hace oscilar el nivel ±4–5 dB cada 50 ms** en ese sample, durante toda la nota. Con o sin Movimiento es igual.
2. **La caída a lo largo de la nota sostenida es pequeña:** mediana +0,2 dB; el peor −0,7 dB. **El sostenido no pierde energía con el tiempo.**
3. **`OS_ATL_C_Music_Box`:** su sostenido está casi todo en los lados del estéreo (el centro queda 11 dB por debajo de los lados): en mono pierde mucho. Ya era así antes.

**Propuestas, sin aplicar, para que decidas:**
- **(a)** Estudiar el temblor de nivel del sostenido Granular en los samples con más oscilación (la lista está en `resumen.txt`).
- **(b)** Que el sostenido evite la contrafase, como se hizo con el motor F el 22-11.

Ambas cambiarían el sonido de configuraciones existentes, así que necesitan tu A/B.

### 4.7 Escucha
**Carpeta:** `release/escuchar_colorplay_2026-11-23/` (instrucciones en `LEEME.txt`).
- 10 samples: pluck, piano, teclado, chelo, cuerdas, flauta, voz, bajo, pad y acorde.
- 10 archivos por sample: A sin nada, Deriva, Armónico + y −, Impronta, fuente Sidechain, y B y D sin y con Movimiento.
- Igualados en volumen.
- **Nada de esto lo he valorado de oído.**

---

## 5. Archivos tocados
**Código:**
- `Source/Params.*`, `Source/SynthEngine.*`, `Source/Filters.h`, `Source/FxRack.*`, `Source/SampleAnalysis.*`;
- `Source/PluginProcessor.*`, `Source/PluginEditor.*`, `Source/Displays.*`;
- `Source/HelpText.cpp`, `Source/TabGuides.cpp`, `Source/DspUtil.h`.

**Pruebas:** `tests/Audit.cpp` (modo `colorplay` y adaptaciones de la auditoría general) y `tests/Batch.cpp` (modo `colorplay`).

**Documentación y otros:** `docs/colores_modulos.md` (panel SIDECHAIN en rosa) y `.gitignore` (`/build_cp/`).

**Instalado el 23-11 a petición tuya.**
- VST3 compilado en `build_cp/` (sha256 642896d3…) y copiado a `C:\Program Files\Common Files\VST3\Blackpanda\Reanima.vst3`.
- `ReanimaHostCheck` sobre el plugin instalado: **VST3 OK**, con la entrada lateral (Sidechain, 2 canales) visible para el host.
- El que estaba instalado (sha256 bb5c1f24…) está guardado en `backups/instalado_antes_colorplay_2026-11-23/`.
- **Sin commit.**
