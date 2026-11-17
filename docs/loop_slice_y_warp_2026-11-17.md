# LOOP: Slice y Normal con Warp (2026-11-17)

## Diagnóstico de lo que sonaba mal
1. **Chipmunk en la nota original**: el loop aplicaba *Snap a C*. Un loop en Do3 se desplazaba hasta ±6 semitonos de su grabación. Ahora Snap a C no toca los loops: en Do3 suena como se grabó.
2. **Tonal (granos solapados) seguía al tempo**: con el proyecto a 100 y un loop a 122, el tono bajaba 3,4 st. Además, sus granos se cancelaban entre sí (sonido "aguado", picos del espectro movidos). Se ha retirado: los proyectos guardados con "Tonal" suenan ahora con Complex.
3. **Un tempo calculado casi igual al del proyecto** (121,9998 frente a 122) forzaba el estiramiento. Ahora, si la deriva es de menos de una muestra por vuelta, se lee el archivo tal cual.

## Algoritmos comparados
| Opción | Calidad | Licencia | Decisión |
|---|---|---|---|
| Signalsmith Stretch 1.4 | vocoder de fase multicanal, formantes | MIT | **Complex / Complex Pro** |
| Rubber Band | muy alta | GPL o comercial | descartado (licencia) |
| élastique | muy alta | comercial | descartado |
| SoundTouch | WSOLA, peor en material tonal | LGPL | descartado |
| Granos propios (Tonal) | metálico, aguado | propio | retirado |
| Beats (cortes en golpes) | perfecto en baterías | propio | se mantiene |

## Modos
- **Normal**: el loop en bucle mientras se mantiene la tecla. Warp, Tempo del host, BPM original (0 = Auto), Duración y Calidad (Auto / Beats / Complex / Complex Pro). Auto elige Beats si el loop es rítmico y Complex si no.
- **Slice**: un trozo por tecla desde *Primera nota* (Do3 = MIDI 60, numeración de Ableton). Cada trozo suena entero aunque la tecla se suelte, a su velocidad y tono originales; Transponer y Fino lo afinan.
  - Cortes: Transitorios (con Sensibilidad), Rejilla (División de 1 compás a 1/32; cada línea se adelanta al golpe que empiece hasta 12 ms antes) o A mano.
  - En la forma de onda: arrastrar mueve un corte; doble clic lo añade o lo quita (Cortes pasa a "A mano" y se guarda con el proyecto).
  - Fundido de entrada de 0,3 ms y de salida de 2 ms antes del corte siguiente. Máximo 128 trozos.

## Medido
- **Nota original y a su tempo, todos los algoritmos**, con Snap a C activado:
  - idéntico al archivo (diferencia máxima 0,000000 en el Audit);
  - en archivos reales a 44,1 kHz reproducidos a 48 kHz, residuo de −73 a −105 dB.
- **Complex a otro tempo**:
  - el tono no sigue al tempo (la nota de 220 Hz queda 63 dB por encima de la de +20 %);
  - una quinta arriba es una quinta exacta;
  - nivel ±0,0 dB en el Audit, −0,1 a −1,5 dB en loops reales;
  - balance espectral igual al original (pad: 9,5/0/−15,7/−37,4 dB frente a 9,6/0/−15,8/−37,4 dB);
  - 0 clics.
- **Complex Pro**:
  - conserva mejor los formantes al bajar el tono (acordes −7 st, banda 1–4 kHz: −17,9 dB frente a −15,5 del original; con Complex, −30,2 dB);
  - queda 1–3 dB más bajo;
  - en bajos graves transpuestos −7 st da chasquidos, así que se recomienda Complex para bajos.
- **Complex en baterías**: los golpes se desplazan hasta 8 ms (un vocoder de fase emborrona el ataque). Por eso Auto usa Beats en baterías.
- **Beats al ralentizar** (122 a 100 BPM): deja pequeños huecos entre golpes, igual que la versión anterior. No es un fallo nuevo.
- **SLICE**:
  - cada tecla da exactamente su trozo del archivo (diferencia 0,00000);
  - una tecla sin trozo calla;
  - Rejilla 1/8 da 8 trozos; Transitorios encuentra los 8 golpes; los marcadores a mano funcionan.
- **CPU**: Complex cuesta unos 1,8 % de un núcleo por nota sostenida, con 8 estiradores preparados de antemano. La 9.ª nota simultánea usa granos.
- **Tiempo real**: 0 asignaciones de memoria en todos los modos LOOP y en Slice.
- **Tests**: ReanimaTests todo bien; Audit 388 + 20 de LOOP, 0 fallos; HostCheck OK; UiSnapshot OK.

## Compatibilidad
- Proyectos antiguos: Calidad 0–1 conserva su significado; 2 (Tonal) pasa a Complex.
- Los parámetros nuevos tienen valores por defecto que reproducen el comportamiento anterior en Normal.
- Snap a C, los presets y las preferencias globales no cambian.

## Pendiente
- Escucha del usuario: carpeta `release/escuchar_loop_pro_2026-11-17` (originales con prefijo 00_).
- macOS sin probar.

## Corrección: asignación MIDI de SLICE (2026-11-17, sha e45ed1eb)
- Causa 1, Legato: un slice no se suelta nunca (suena hasta su final), así que con Modo Legato cada nota nueva solo cambiaba el tono de la voz que ya sonaba y no arrancaba su trozo (prueba: «111106666055550»). En Slice, Legato funciona ahora como Mono y soltar una tecla ya no hace glide a otra.
- Causa 2, Transitorios: un falso corte en la caída de un sonido (45 ms después del golpe) movía todas las teclas siguientes un trozo atrás. Ahora un corte solo cuenta si el nivel sube al menos 3 dB.
- Prueba por el plugin completo (`ReanimaAudit loops`, `auditSliceMidi`, 15 comprobaciones), con 8 trozos de tonos distintos:
  - C3…G3 tocan los trozos 1…8 (separación ≥ 92 dB) y G#3 calla;
  - en una secuencia rápida (una tecla cada 60 ms, solapadas) se oye 123456787654321;
  - funciona con Poly, Mono y Legato, por Rejilla y por Transitorios;
  - tras reabrir el proyecto, mismos cortes;
  - con marcadores a mano, las teclas siguen los marcadores al instante.
- Audit 403/403, Tests, RtCheck y HostCheck correctos. La versión anterior está en backups/antes_slice_midi_2026-11-17.

## Reconstrucción de la verificación SLICE por VST3 (sha 81d58b21)
- Recorrido trazado: `handleMidi` → `noteOn(note)` → `Voice::start(newNote)` → `index = nota − Primera nota` → cada voz guarda su propio `sliceFrom/sliceTo` (no hay variable global de reproducción) → `renderSlice`. Fuera de rango: la voz no suena.
- `ReanimaHostCheck <vst3> slices`: carga el VST3 instalado como un host, pone el loop y los ajustes por el estado del proyecto y toca MIDI real. Con 16 trozos de tonos distintos:
  - las 16 teclas, C3 (60) … D#4 (75), dan los 16 trozos, con separación de 29 a 74 dB;
  - E4 calla;
  - polifonía de 4 trozos a la vez;
  - una tecla cada 40 ms, solapadas: 31/31;
  - al reabrir el proyecto y al regenerar a 1/8, las teclas siguen bien.
- `ReanimaHostCheck <vst3> slices <archivo>` (informativo) y `ReanimaAudit cortes <archivo>`: tabla trozo → tecla → inicio/fin y el trozo que tomó la voz. Con pluck y acordes reales, el VST3 lee cada tecla del inicio exacto de su trozo.
- Corregido: la regla de 3 dB dejaba un loop de acordes en 6 trozos de 134 golpes (trozos larguísimos). Ahora basta con que el nivel suba +0,4 dB: acordes 64, pluck 39, pad 27, batería 24.
- Diagnóstico en pantalla (Slice, bajo los controles): «Última tecla: D3 (MIDI 62) → trozo 3 (0,250–0,375 s)».
- Error de las pruebas, ya corregido: la prueba de resistencia de HostCheck no cargaba samples (el estado VST3 va envuelto en `VST3PluginState/IComponent`).

## Prueba en REAPER 7.86 (instalado en C:\Users\YinYan\REAPER), sha 81d58b21
- REAPER se lanzó con una configuración aparte (`-cfgfile`, ruta de plugins = VST3\Blackpanda), así que tu reaper.ini no cambió. El único efecto es que la caché de plugins de REAPER ahora incluye a Reanima.
- Un script Lua (`run2.lua`):
  - inserta una pista con Reanima y le pone el estado con el loop de 16 tonos (Slice, Rejilla 1/16, Primera nota 60);
  - escribe un clip MIDI de REAPER: 17 toques C3…E4, el acorde C3+E3+G3+C4 y una secuencia de 31 notas cada 40 ms;
  - renderiza a WAV de 48 kHz.
- Resultado (`analyse.py`):
  - 16/16 teclas tocan su trozo (separación de 29,6 a 73,6 dB), y cada trozo acaba en su final;
  - E4 calla;
  - el acorde suena con 4 trozos a la vez;
  - la secuencia rápida da 31/31.
- Archivos en release/prueba_slice_reaper_2026-11-17.

## SLICE: visualización por voz, retrigger e imán (sha 08d410d3)
- **Causa del indicador fijo**: `Voice::getPlayhead()` devolvía la posición de lectura del sampler (`lanes[0].pos`), que en LOOP se queda en el inicio, es decir, en el primer trozo.
- **Ahora el indicador sale del motor**:
  - devuelve la lectura real: en Slice, `loopRead` dentro del trozo; en Normal, `loopTime`;
  - el motor publica tras cada bloque `playheads[v]` y `playSlices[v]` por voz (atómicos, sin asignar memoria);
  - la ventana ilumina cada trozo que suena, en su color, con una barra por voz;
  - al acabar el trozo la voz se libera y se apaga.
- **Retrigger**: en Slice, repetir una tecla corta su propio trozo anterior (un redoble no se amontona). Las teclas distintas siguen sonando juntas en Poly.
- **Marcadores**: al arrastrar o añadir con doble clic se pegan al golpe detectado más cercano (a 10 px o menos). Con Mayús quedan libres, a la muestra.
- **Pruebas**:
  - Audit «SLICE en pantalla»: indicador en su trozo y avanzando a su ritmo; acorde de 3 trozos; nada queda encendido al acabar; retrigger.
  - REAPER 7.86 con MIDI real: 8/8, 16/16 y 32/32 teclas; secuencias rápidas de 15/15, 31/31 y 63/63; acordes bien.
  - VST3 a 64 muestras: CPU media 1,18 %, peor bloque 8 % del tiempo disponible, 0 asignaciones de memoria (RtCheck).
  - Audit 404/404 y Tests correctos.
- **Clics**: 0 en los finales de trozo. En los inicios solo aparecen los que ya tiene el archivo (los tonos de prueba arrancan de golpe; el original da 18).

## Play con el host, tono por MIDI, Thru, Fundido y clip MIDI (sha d6bb5d68)
- **Play** (Normal; parámetro loopPlay, apagado de serie):
  - con el transporte del host en marcha, una sola voz toca el loop donde está el host (ppq; el primer tiempo del loop cae en el compás 1);
  - se detiene al parar el transporte y, al volver, retoma desde la posición musical;
  - sigue los cambios de tempo;
  - un salto de más de 4 ms se reengancha con 3 ms de fundido de entrada.
- **Tono por MIDI con Play**: las teclas solo afinan la voz (glide de 15 ms como mínimo), sin reiniciarla.
  - Con el tono original suena el archivo tal cual: lectura a muestra entera; el tempo calculado, 121,9998 frente a 122, la desplazaba en fracciones de muestra.
  - Al primer cambio de tecla pasa al estirador (que ya estaba preparado) en 10 ms con curva suave; la rampa recta dejaba un tic a −50 dB.
- **Snap a C** se aplica ahora también a los loops cuando está activo (si el sample tiene tono detectado).
- **Thru** (Slice): de su trozo hasta el final del sample.
- **Fundido** (Slice): de 0 a 100 ms, 2 ms de serie.
- **Arrastrar MIDI** (Slice):
  - genera un .mid de 9600 pulsos por negra (antes 960, que desplazaba los trozos hasta 0,26 ms);
  - una nota por trozo desde Primera nota, con velocidad 127 para que el loop salga con su nivel y brillo;
  - en el sitio y con la duración de cada trozo, con el tempo del loop, 4/4 y la longitud del loop;
  - se arrastra con el arrastre externo de archivos de JUCE; con un clic se guarda.
- **REAPER 7.86** (release/prueba_play_host_reaper_2026-11-17, 9/9):
  - Play desde el compás 1, con Beats y con Complex: −44,4 dB frente al archivo, el mismo límite que la etapa de salida da fuera de REAPER (−44,5 dB);
  - arrancando el transporte en 2,0 s: −44,6 dB;
  - cambio de tempo de 122 a 140: golpes en la rejilla (100 % antes y 91 % después);
  - teclas: 0 / +7,01 / +0,02 semitonos, sin reinicio y sin clics;
  - Snap a C correcto (el loop ya estaba en Do);
  - clip de 64 notas tocado en Slice: dentro de cada trozo −40 dB o mejor, colocado a 18 muestras como mucho (la resolución MIDI de REAPER, 960);
  - Thru y normal correctos.
- **Audit**: 414/414, incluidas 9 comprobaciones nuevas de transporte, Thru, Fundido y clip. Tests, RtCheck (0 asignaciones de memoria) y HostCheck correctos; Slice a 64 muestras: CPU media 1,17 %.
- **No probado**: el arrastre real a Ableton (es un archivo .mid estándar; REAPER lo recibe por script, no por arrastre) y la parada y reanudación en vivo en REAPER (el render offline no se detiene; se comprobó en el Audit).

## Forma de onda estable, Por golpe, orden de los trozos y exportación en EDICIÓN (sha a3b666df)
- **Forma de onda**: en LOOP la vista mide el sample entero. Antes sumaba la envolvente (Ataque + Decay + sostén + Release), que en LOOP no se dibuja, así que un preset con Release largo encogía el loop a parte del ancho.
- **Por golpe** (loopHitEnv; en MOTOR, para Normal y Slice):
  - **Normal**: en cada corte que cruza el loop (los de Slice; Sensibilidad decide cuántos), la envolvente de amplitud vuelve a empezar desde su nivel actual (sin bajar a cero, para no dar clic) y la del filtro (Env 2) vuelve a su ataque.
  - **Slice**: cada trozo lleva la envolvente entera y suelta con Release al levantar la tecla.
  - **Arquitectura**: las marcas de golpe se calculan dentro del bloque, sin reservar memoria. Cada trozo de Slice ya era una voz con su filtro, así que el coste extra es casi nulo.
- **Orden de los trozos** (estado del proyecto «sliceOrder»; tira en EDICIÓN; se cambia arrastrando y el doble clic devuelve el orden del archivo):
  - la tecla k toca el trozo order[k];
  - lo siguen la forma de onda (nombres de tecla y regiones iluminadas), EXPORTAR MIDI (la secuencia en ese orden, cada trozo con su duración) y EXPORTAR AUDIO.
- **EXPORTAR AUDIO**:
  - WAV, AIFF o FLAC; 44,1, 48, 88,2 o 96 kHz; 16, 24 o 32 bits (coma flotante en WAV);
  - render offline con el mismo motor (efectos, buses y modulación incluidos) y la etapa de salida, con la latencia compensada;
  - Slice: la secuencia al tempo del proyecto; Normal: un loop entero; One shot: la nota con su cola.
- **EXPORTAR MIDI**: el botón que estaba en MOTOR («Arrastrar MIDI»), ahora en EDICIÓN; se arrastra al DAW o se guarda con un clic.
- **Pruebas**:
  - Audit 420/420, con 6 nuevas: orden en teclas, clip y audio; exportación idéntica a lo que suena (diferencia 0,0000000); Por golpe en amplitud y en filtro.
  - REAPER 12/12: orden 16→1 con MIDI real y Por golpe en Normal (23 golpes; apagado, el loop tal cual a −44,4 dB).
  - Captura de EDICIÓN con el orden cambiado: 11d_edicion_slices.png.

## Motor Warp nuevo: Bungee para Complex (sha f6e3fd36)
- **Investigación**:
  - candidatos: Signalsmith (MIT, el actual), Rubber Band R2 y R3 (GPL o licencia comercial de pago), Bungee (MPL-2.0), élastique (solo comercial), SoundTouch (inferior);
  - técnicas: reinicio de fase en transitorios, PGHI («Phase Vocoder Done Right», Průša y Holighaus) y separación armónico-percusiva (Driedger y Müller).
- **Banco** (research/stretch_2026-11-17, herramienta ReanimaStretchLab + bungee_cli): 8 loops (batería, bajo, voz, guitarra, piano, acordes, pad, textura) × 0,82 y 1,2 de tempo, estirando y desestirando.

| | Signalsmith | SS corto | RB R2 | RB R3 | Híbrido SS | Híbrido R3 | **Bungee** |
|---|---|---|---|---|---|---|---|
| Distancia espectral de ida y vuelta (dB) | 7,73 | 8,25 | 8,96 | 7,70 | 11,71 | 11,44 | **6,24** |
| Pre-eco en golpes (dB) | 6,11 | 5,72 | 6,51 | 6,30 | 7,40 | 6,65 | **5,22** |
| Desviación de timbre (dB) | 0,80 | 1,55 | 1,22 | 0,76 | 3,05 | 3,07 | **0,65** |
| CPU (s por s de audio) | 0,016 | 0,015 | 0,030 | 0,096 | 0,056 | 0,137 | **0,011** |

- **Beats en batería**: pre-eco de −7 dB (los golpes no se tocan). Es lo mejor para batería y Auto lo sigue eligiendo para loops rítmicos.
- **Arquitectura**:
  - Auto: Beats si el loop es rítmico, Complex si no;
  - Complex = Bungee: granos pedidos por posición sobre el loop leído a la frecuencia del host, con caché en anillo y una FIFO de salida; el arranque descarta lo anterior a la posición de la nota, así que va a tiempo con 0 ms de retraso;
  - Complex Pro = Signalsmith con formantes;
  - presupuesto de 6 granos de arranque por bloque para todo el motor.
- **CPU a 64 muestras** (una voz): media 1,4 % del bloque, pico 42 % al arrancar la nota y 17 % después. Con 4 teclas a la vez, peor bloque 47 %. 0 asignaciones de memoria (RtCheck).
- **Fallo corregido** (afectaba a la versión anterior): con Play y el tempo del proyecto distinto del loop, la lectura «tal cual» se desfasaba y reiniciaba el estirador cada pocos ms (412 clics en REAPER). Ahora: 0.
- **Validación**: Audit 422/422 (+2: cuatro teclas a la vez a 64; Play a otro tempo sin clics), Tests, RtCheck y HostCheck correctos. REAPER 13/13 (nuevo: Complex a 100 BPM con tono intacto, +5,05 st con la tecla, −0,22 dB y 0 clics).
- **Escucha**: release/escuchar_motor_warp_2026-11-17. Por sample: 0 original, 1 anterior (Signalsmith), 2 Bungee, 3 Rubber Band R3 (referencia), 4 Beats (batería), 5 Reanima nuevo (×0,82, ×1,2, +5 st) y 6 Auto.
- **Pendiente**: tu escucha. En guitarra, R3 mide mejor que Bungee (8,19 frente a 10,62); habría que revisarlo por oído.

## Beats más lento: colas continuas (sha 36787c33)
- **Escucha del usuario**: por debajo de 1x, Beats separaba bien los golpes, pero la reverb y la sala del sample caían entre golpe y golpe («diferencias de volumen, no admisible»). Por encima de 1x, bien.
- **Medido**: al 0,82x, el 9 % del tiempo quedaba a más de 12 dB por debajo de su entorno (silencio total, −165 dB, antes de cada golpe). El original da 0 %.
- **Ahora**:
  - cada golpe sigue saliendo del archivo, intacto;
  - cuando su trozo se acaba antes del golpe siguiente, toma el relevo el mismo loop estirado por Bungee, fijo 60 ms antes del golpe siguiente: es el sonido que había justo antes del golpe, alargado sobre el hueco;
  - el relleno se retira 3 ms antes del golpe;
  - con el relleno siguiendo la línea de tiempo había golpes falsos a 0,5x, y fijo a 30 ms la cola quedaba 7 dB por encima; con 60 ms la cola queda a −37 a −39 dB, frente a −37 a −38 dB en el original.
- **Resultado**: huecos 0,1 a 0,2 % a 0,82x, 0,66x y 0,5x; golpes en su sitio.
- **Fallo encontrado con AddressSanitizer**: Bungee solo admite ±2 octavas de transposición y, más allá, escribía fuera de sus búferes (lo destapó la prueba de resistencia aleatoria). Fuera de ese rango, Complex usa Signalsmith y Beats vuelve a su fundido. Con AddressSanitizer: 4 × 60 s de caos sin errores.
- **Pruebas**: Audit 423/423 (nueva: Beats de 100 a 80 sin huecos), Tests, RtCheck y HostCheck correctos. REAPER 14/14 (nueva: Beats de 122 a 100 con Play; huecos 0,1 % y los 17 golpes donde los del archivo).
- **Escucha**: en release/escuchar_motor_warp_2026-11-17, los archivos _7_beats_con_colas y _8_auto_con_colas, en tres baterías a ×0,82 y ×0,66.

## Rubber Band (R3) como calidad «Rubber Band» y licencia AGPLv3 (sha a540c92f)
- **Decisión del usuario**: integrar Rubber Band y publicar Reanima como software libre. Reanima pasa a la licencia **AGPLv3** (LICENSE.md, LICENCIAS_TERCEROS.md); Rubber Band 4.0.0 está en external/rubberband-4.0.0.
- **R3 en tiempo real, medido**: media del 15,8 % del bloque de 64, picos del 146 % al reproducir y del 490 % al arrancar. No cabe en un bloque de 64, así que no se usa en tiempo real.
- **Arquitectura: R3 precalculado**:
  - un hilo aparte renderiza offline el loop (tres vueltas, se queda la central y se cierra la costura con 50 ms de coseno) para el tempo y el tono que pide la voz;
  - la voz solo lee el render: peor bloque del 4,7–7,6 %, lo más ligero de todos los modos;
  - hasta que llega el render (al arrancar, o con una tecla o un tempo nuevos) suena Complex (Bungee), y el paso a R3 es un fundido de igual potencia de 60 ms;
  - el render solo se pide cuando el tono y el tempo llevan 8 bloques quietos (glide, vibrato y pitch bend siguen en Bungee);
  - el render anterior termina de fundirse antes de que entre el nuevo.
- **Render offline del DAW** (isNonRealtime) **y EXPORTAR AUDIO**: esperan al render R3, así que lo grabado es R3 desde el principio.
- **Fallos corregidos por el camino**:
  - la salida offline de R3 no es periódica exacta (se repite 20 muestras antes), lo que daba un clic en la costura;
  - un corte seco del render viejo al cambiar de tecla, otro clic;
  - el glide pedía un render por cada bloque, lo que en REAPER hacía el render lentísimo;
  - el cambio de render en mitad del fundido, otro clic más.
- **Pruebas**:
  - Audit 425/425 y «loops» 57/57 (nuevas: Rubber Band suena al momento, R3 a tempo y en tono, quinta, sin clics, bloques ligeros; EXPORTAR AUDIO en R3);
  - RtCheck sin asignaciones de memoria, también con la calidad 4; HostCheck correcto;
  - REAPER 15/15 (nueva: Rubber Band con Play a 100 BPM, tono intacto, +5,05 st, −0,24 dB, 0 clics);
  - AddressSanitizer: 4 × 30 s de caos sin errores.
- **Motor F**: su prueba de tiempo falló con Ableton Live abierto. Con Ableton cerrado, 3 de 3 pasan (peor bloque 2,38 ms de 5,33, media 10,1 %, 0 bloques por encima de la mitad). Era la carga de Ableton, no una regresión.
- **Escucha**: lo que suena en «Rubber Band» es R3 offline, el mismo de los archivos _3_referencia_rubberband_R3 de release/escuchar_motor_warp_2026-11-17.
