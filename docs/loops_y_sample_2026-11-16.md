# Loops, Warp, presets y carga de samples — 2026-11-16

Instalado: `VST3/Blackpanda/Reanima.vst3`, sha 7f9a829d. Copia previa (VST3 y fuentes): `backups/antes_loops_2026-11-16`.

## 1. LOOP con Warp
- **Detección al cargar** (`SampleAnalysis`, `detectLoop`): tempo del nombre ("122bpm", "_120_" si la duración cuadra), duración exacta de DAW, golpes en la rejilla de semicorcheas desde la primera muestra, cola viva al final, palabras (loop / one shot / golpe suelto). Sólo pasa a LOOP solo si es claro (confianza ≥ 0,65); en duda queda en ONE SHOT y la forma de onda sugiere «¿LOOP?».
  Medido sobre 1 200 archivos de la biblioteca (etiquetas claras por nombre: 496 loops, 216 one shots): con nombre, 98,0 % de loops y 98,6 % de one shots bien; tempo exacto en 47/47. Sólo audio: 34,9 % loops seguros, 41,7 % en duda, 0,9 % de one shots confundidos.
- **Loops con cola** (reverb, nota final): con "bpm" en el nombre se toman los compases enteros.
- **Reproductor** (`Voice::renderLoop`): lee el archivo sin los microfundidos de sus extremos, en bucle sin costura.
  - Beats: corta en los golpes y cada trozo suena en su sitio del compás, a su tono; final de trozo con fundido de hasta 15 ms, cruce de 4 ms.
  - Tonal: el lector Warp en Textura con bucle; granos de 60 ms, o de cuatro ciclos de la nota al bajar de tono (bajo a −3 st: de 36 a 0 clics).
  - Auto: Beats si más de la mitad de la energía sobre 150 Hz es de golpes (separación armónica/percusiva por medianas; calibrado con 400 loops: 88 % de baterías a Beats, 87 % de tonales a Tonal).
  - A su propio tempo y tono suena el archivo exacto (diferencia 0).
  - Sin Warp: sampler clásico.
  - Sin tempo del host: el suyo.
- **Controles** (SAMPLE › MOTOR): ONE SHOT/LOOP en el título; Duración (Auto o 1 tiempo… 16 compases), Algoritmo (Auto/Beats/Tonal), Warp y Tempo del host. En LOOP la envolvente es una puerta (Ataque/Release; al pasar a LOOP 1 ms/30 ms; al volver a ONE SHOT, el ADSR detectado). Rejilla de tiempos y cortes sobre la forma de onda.
- **Probado** (Audit, loop de 100 BPM a 120): golpes en las corcheas de 120 con 0–3 ms de error; nivel ±0,8 dB; tecla +7 sube el bombo de 60 a 90 Hz sin mover los golpes; Tonal sin saltos; duración corregida a mano. CPU a 64 muestras: 1,3 % (Beats) y 2,3 % (Tonal) con 4 voces; 0 asignaciones en el hilo de audio.
- **Comparado con Signalsmith Stretch** (incluido en el proyecto): mejor sólo en el bajo bajado, peor en el bajo subido (42 clics) y con ataques desdibujados (pluck: 14–31 % de golpes en la rejilla frente a 95–100 %). No se usa.
- **Escucha:** `release/escuchar_loops_2026-11-16` (batería, acordes, pluck, pad y bajo a otros tempos y tonos).

## 2. Tono y duración por separado en ONE SHOT
Ya existía: Warp (SAMPLE › MOTOR). Re-Pitch es un sampler clásico (+12 st: 0,94 s; −12 st: 3,70 s frente a 1,86 s). Tonos y Textura mantienen la duración (1,80–1,90 s) y mueven el tono; el ataque se lee entero (0 dB). Tonos cae solo a Textura en los samples donde falla su prueba de carga.

## 3. Presets
La tarjeta de presets ocupa ahora el hueco junto a REANIMA (de 278 a 332 px).

## 4. Arrastrar desde Ableton / Splice
- Toda la ventana acepta archivos y también rutas o URLs `file://` en texto.
- Las extensiones salen de los formatos que el sistema sabe leer: en Windows WAV/BWF, AIFF, FLAC, Ogg/Opus, MP3 y WMA; en macOS además M4A y CAF. Probado: MP3, OGG, FLAC, AIFF y WAV en mayúsculas.
- Ableton y Splice entregan archivos ya descargados en disco. El sonido se guarda dentro del proyecto, así que sigue sonando aunque el archivo temporal desaparezca.
- No probado dentro de Ableton desde aquí: arrastrar del navegador de Live a la ventana del plugin.

## 5. Borrar presets
Papelera junto al corazón en los presets del usuario, con confirmación. El archivo va a la papelera del sistema y sale de favoritos y de los usos contados. Los de fábrica no se pueden borrar. Lo cargado sigue sonando: el proyecto lleva todo dentro. Los favoritos de presets que ya no existen dejan de mostrarse.

## 6. Sample nuevo: sesión limpia
Al cargar con Cargar o arrastrando:
- Vuelve a sus valores todo lo que da forma al sonido: motor A, Warp Re-Pitch, sin reverse, Textura, filtro, modulación, matriz, efectos ni unison, Transponer a 0.
- Entra el ADSR detectado y el preset queda sin nombre.
- Se conservan Volumen, Pan, Mono, Graves mono, Snap a C, Modo, Glide, Bend, aftertouch, vibrato, Vel → Amp y Fijar ADSR (cerrado, la envolvente también).
- El análisis y el reconocimiento (nota, envolvente, instrumento de Mecanismo, loop) funcionan igual.
- No se aplica al abrir proyectos ni presets.
- Deshacer devuelve el sample anterior con todos sus ajustes.

## Pruebas
Motor: todas · Audit 365/365 · Interfaz 194/194 · RtCheck 0 asignaciones (con LOOP) · HostCheck VST3 OK (la prueba del fabricante, actualizada a «Blackpanda» tras el cambio de nombre de hoy).

## No verificado
Oído; arrastre real desde el navegador de Ableton y desde Splice (aquí sólo la ruta de código con archivos y URLs); macOS.
