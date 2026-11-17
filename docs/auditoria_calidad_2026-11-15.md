# Auditoría técnica integral — 2026-11-15 (noche)

Continúa `rendimiento_2026-11-15.md` (optimización a 64 muestras). Equipo de medida: Intel 11.ª gen. de portátil
(2,6–3,1 GHz, por debajo de un i7/i9 de sobremesa). Todas las cifras salen de los ejecutables de prueba del proyecto;
nada se da por hecho sin ejecutarlo.

## Problemas encontrados y corregidos
| # | Dónde | Qué pasaba | Corrección | Comprobación |
|---|---|---|---|---|
| 1 | Filtro por voz, circuito MS2 | Saturación asimétrica sin bloqueo de continua: con 24 dB de drive dejaba +0,02 de offset (−17 dB bajo la señal): golpes al empezar y acabar notas, menos margen | Bloqueo de continua a 5 Hz sólo en MS2 (los demás circuitos son simétricos y quedan bit a bit) | Audit: continua −95 dB bajo el tono |
| 2 | Etapa de salida | El techo a −0,3 dBFS daba +0,26 dBTP entre muestras al forzarla 12 dB | Techo a −0,6 dBFS (0,9333) | Audit: true peak 4x −0,05 dBTP; Snapshot |
| 3 | Filtro por voz a 88,2/96 kHz | Sobremuestreo 4x también a frecuencias altas (352–384 kHz internos): el doble de trabajo sin más calidad | 2x a ≥ 88,2 kHz (176–192 kHz internos, lo mismo que 4x a 44,1/48) | Audit: misma respuesta a las 4 frecuencias (±0,17 dB); aliasing < −150 dB |
| 4 | Textos | «El volumen empieza en −12 dB» en 4 sitios; el valor de serie es −6 dB | Corregidos (ayuda, tour, guía rápida, guía de pestaña) | — |
| 5 | Tour de bienvenida | Una preferencia global para todas las instancias | Estado por instancia (en el proyecto) + casilla «no volver a mostrar» que lo apaga en todas; el ? siempre lo abre | Snapshot: prueba de 12 condiciones |
| 6 | Código | Variable `engine` que ocultaba un miembro (aviso del compilador) | Renombrada | Compilación sin avisos propios |

## Auditoría de audio (nuevo bloque «Calidad de sonido» de ReanimaAudit, 25 comprobaciones; 343 en total, 0 fallos)
- **THD del filtro por voz** (1 kHz a −6 dBFS, corte abierto, sin drive): Clean −138 dB; OSR −35; MS2 −32; SMP −35; PRD −28 dB. Los circuitos con color saturan desde cero por diseño (PRD lleva 1,5x de entrada: 3,7 %).
- **Aliasing de la saturación** (6 kHz, 24 dB de drive, espurias no armónicas bajo 16 kHz): todos < −150 dB.
- **Respuesta igual a 44,1 / 48 / 88,2 / 96 kHz** (paso bajo 24 dB a 1 kHz): 1 kHz ±0,00 dB; 2 kHz ±0,17 dB.
- **Intermodulación SMPTE** (60 Hz + 7 kHz 4:1): sin filtro ni efectos −127 dB; filtro MS2 −24 dB (0 drive) / −3 dB (12 dB); Distorsión Tube −18 / −15 / −7 / −0,3 dB a 0 / 10 / 30 / 50 % (satura desde cero: así es una válvula).
- **Graves mono** (100 Hz; un sub de 40 Hz sólo a la izquierda): lado −32 dB (lo que da un Linkwitz-Riley de 24 dB/oct), centro ±0,00 dB, 2 kHz intactos; igual a 44,1 y 96 kHz.
- **Motor F, Estéreo 100 %**: 55 Hz en el centro (lado −133 dB: compatible en mono); 1 kHz lado −4 dB (ancho).
- **Unison x4, Ancho 100 %** (motores A y B, 55 Hz): la suma a mono de los graves pierde 1,0 dB frente a Ancho 0 (no se cancela).
- **Polaridad y retardo de cada efecto con mix 0**: nivel exacto en los 15; en fase y sin retardo salvo los all-pass por diseño (Distorsión: 6 muestras por su sobremuestreo; splitters: cruce all-pass).
- **Ruido de fondo tras la nota**: silencio absoluto (−240 dBFS) a los 7,5 s sin efectos y a los 19,5 s con distorsión, chorus, phaser, delay, reverb y compresor.
- **True peak** de la salida forzada 12 dB: −0,05 dBTP.
- Ya existentes (y pasan): latencia cero a todos los tamaños de bloque y frecuencias; pureza de la interpolación (espurias < −95 dB); aliasing al transportar +24 st < −107 dB; respuesta a 20 kHz −1,1 dB; mono exacto; sample sólo en L no invade R; «sin tocar = archivo original» 131 dB de nulo; estabilidad de todos los efectos en extremos; determinismo; 0 asignaciones en el hilo de audio.

## Parámetros y tooltips
- 118 parámetros base: todos con código en el motor y texto de ayuda; los generados (LFO, matriz, 14 ranuras de efectos) con ayuda genérica y por efecto. Audit: «ningún control muerto sin avisar» por motor (los inactivos en un motor lo dicen en su ayuda); «todos sus parámetros actúan» en los 15 efectos.
- Cifras de las ayudas y guías cotejadas con el código: ±50 ct de vibrato, ±50 ct de detune, matriz ±24 st / ±24 dB / ±60 st, keytrack 100 % = octava por octava, release mínimo 60 ms, Muestreo igual a cualquier frecuencia, Tamaño FFT 0,7 s a 96 kHz, Normalizar (edición) −0,1 dB y Normalizar (parámetro) 0 dB, 206 presets, 16 modos de distorsión, 5 salas. Única discrepancia: el volumen (corregida).

## Código
- Compilación completa con las banderas de aviso recomendadas de JUCE: 0 avisos propios (quedaba uno, corregido; el de relleno por alineación de `PartialBank` es informativo).
- Rastreo de funciones sin uso (`research/rendimiento_2026-11-15/deadcode.py`): ninguna muerta; las únicas referenciadas una sola vez son sobrecargas de JUCE y accesores de prueba.
- Hilo de audio: sin asignaciones ni bloqueos (RtCheck, 0 en 432 escenarios); el cambio de sample pasa por un try-lock; los módulos de efectos se crean en el hilo de mensajes y el de audio sólo lee punteros atómicos; el rack procesa en trozos de 128 sea cual sea el bloque del anfitrión.

## Rendimiento (64 muestras; antes → después de esta pasada, véase también el informe anterior)
- 44,1 y 48 kHz: 0 bloques fuera de tiempo en todos los motores, 4–36 voces, cadenas completas, 206 cambios de preset.
- 88,2 y 96 kHz (cello, 6 motores, compilación final, máquina sin otra carga; antes → después del 2x):
  - 4 voces con todo activado: 0 bloques fuera de tiempo en los seis motores a las dos frecuencias; media 10–20 %.
  - Hasta 32–36 voces con todo activado (releases largos de acordes): medias 37–100 %; A/B/D/E bajan de 50–76 % a 37–56 %
    y quedan con 0–4 bloques sueltos; C y F siguen fuera de tiempo en ese caso extremo (C 92–100 % de media, F 55–63 % con
    picos de FFT). Es el límite de CPU por voz: a 96 kHz un bloque de 64 dura 0,67 ms. Con esa carga, usar 128.
  - Datos: `research/rendimiento_2026-11-15/v6_rates.csv` y `comparacion_88_96k_antes_despues.txt`.

## Latencia
- Latencia algorítmica del plugin: **0 muestras** a todos los tamaños de bloque (16–1024) y frecuencias; no se declara latencia al anfitrión. El buffer de 64 muestras del anfitrión = 1,33 ms a 48 kHz (0,67 a 96); la latencia de ida y vuelta la pone la interfaz de audio (entrada + salida + sus propios búferes), no el plugin.
- Retardos internos sólo dentro de la Distorsión (6 muestras en su señal seca, para quedar en fase con la húmeda) y los splitters (cruce all-pass).

## Pruebas superadas (compilación final)
Tests del motor: todas · Audit: 345/345 · Snapshot (interfaz): 187/187 · RtCheck: 0 asignaciones · HostCheck VST3: OK.
Instalado: sha b22633db. Copia previa: `backups/antes_auditoria_2026-11-15` (VST3 af3d63bc).

## Pendiente / no verificado
- Prueba de oído del usuario (MS2 ya sin offset, techo de salida −0,6 dBFS, 2x a 96 kHz).
- A 88,2/96 kHz con 32–36 voces sonando y todo activado (caso extremo), ver tabla; el límite práctico es la CPU por voz.
- macOS / ARM: compilado sólo en Windows x64 (las rutas SSE tienen alternativa escalar, no probada).

## El encargo, punto por punto
| Punto | Estado |
|---|---|
| 1 Auditoría de audio (respuesta, fase, estéreo/mono, THD, IMD, ruido, aliasing, transitorios, true peak, SNR, filtros, saturaciones, interacciones) | Hecho y medido (bloque «Calidad de sonido» + 318 comprobaciones previas); 2 defectos corregidos (MS2 DC, true peak) |
| 2 Panel por panel y cada control | Cubierto por Audit (control muerto por motor, cada parámetro de cada efecto actúa, guardado/recuperación de todos los parámetros, MPE en los seis motores) y Snapshot (187 pruebas de interfaz); cifras de ayudas cotejadas |
| 3 CPU/RAM/latencia | Hecho: 48 kHz/64 sin sobrecargas; 2x a ≥88,2 kHz; 0 asignaciones; memoria estable 15 min; latencia 0 |
| 4 Pruebas de rendimiento (4 frecuencias, motores, 4+ voces, granular, efectos, modulación, presets, sesión larga) | Hecho (suite); límite documentado: 32–36 voces con todo a 88,2/96 kHz en C y F |
| 5 Código | Avisos 0, rastreo de código muerto, revisión de concurrencia y tiempo real; sin fugas en sesión larga |
| 6 Tooltips | Revisadas; 1 incoherencia (volumen) corregida; ninguna vacía (prueba) |
| 7 Tour por instancia + casilla «no volver a mostrar» global + ? siempre | Hecho y probado (12 condiciones) |
| 8 Optimización y corrección priorizando sonido | Todo sin tocar algoritmos ni resolución; cambios de sonido sólo los defectos (y el techo −0,6 dBFS) |
| 9 Validación y entrega | Todas las pruebas; instalado con copia; este informe |
| No verificado | Oído del usuario; macOS/ARM; buffers de 64 a 96 kHz con más de ~30 voces y todo activado en C y F |

## Pasada de resolución y portabilidad (misma noche; instalado sha 71f81bab; anterior en `backups/antes_sinc_granos_2026-11-15`)

### Hallazgo mayor: los lectores de granos no estaban limitados en banda
Al transportar hacia arriba, los motores B, D y E (y la Textura de A, y los transitorios de C y D) leían el archivo con
una interpolación cúbica: todo lo que queda por encima de la nueva Nyquist se plegaba dentro de la banda. Medido con un
tono de 19 kHz a 44,1 kHz: **−5 a −13 dB** de plegado a +7/+12/+19 st (el motor A, que lee con sinc, −92 a −108 dB).
Ahora todos los lectores usan la misma sinc de Kaiser de A con el corte de su velocidad (`readBandLimited`,
`readBandLimitedWrap`): B/D −101…−115 dB, E −98…−114 dB. Coste: B/D/E suben del 1,2–2,2 % al 2,3–4,3 % con 4 voces y
del 15 al 28–30 % con 36 voces a 48 kHz; siguen con 0 bloques fuera de tiempo (`comparacion_granos_sinc_48k.txt`).
Warp Tonos/Textura en A (PSOLA) dejan −32…−43 dB entre armónicos de una nota de 440 Hz subida: energía de sus ventanas,
no plegado; queda como guarda de regresión. B y D en Sostenido sostienen con granos (líneas ensanchadas a propósito).

### Portabilidad Windows / macOS (revisión de código; en macOS no se ha compilado desde aquí)
- CMake: binario universal arm64+x86_64, macOS 11+, VST3 + AU + Standalone, códigos de fabricante/plugin y tipo AU
  MusicDevice definidos; ajustes en `~/Library/Application Support/BlackPanda/Reanima`; presets en Documentos.
- Todo el SSE (`SynthEngine.cpp`, `Filters.h`, `HalfBand.h`) va bajo `__SSE2__ || _M_X64` con ruta escalar equivalente
  (Apple Silicon) y `#include <emmintrin.h>` propio (Intel/clang); `_BitScanForward64` ↔ `__builtin_ctzll`;
  `<intrin.h>` sólo en MSVC; Stretch.h silencia avisos en MSVC, clang y GCC; fuentes: Segoe UI sólo en Windows.
- Herramientas de prueba hechas portables en esta pasada: ReanimaCpuBench (cronómetro `std::chrono`, memoria por
  plataforma con `mach_task_info`, prioridad de audio con MMCSS en Windows y `Process::setPriority` fuera; el perfilador
  de muestreo queda sólo en Windows) y ReanimaRtCheck (`posix_memalign` fuera de Windows). HostCheck ya estaba guardado.
- Queda por hacer en un Mac: compilar, `auval`, pasar las suites y medir CPU (los números de aquí son de un Intel 11.ª gen).

### Revisión módulo a módulo (idiosincrasia y resolución)
| Módulo | Cómo trabaja | Resolución / estado |
|---|---|---|
| Lectura del archivo (A) | Sinc de Kaiser 24–96 taps, 512 fases, corte según velocidad; SSE | Espurias < −95 dB, 20 kHz −1,1 dB; referencia del resto |
| Granos (B, D, E, Textura) | Ventana Hann, posiciones y pasos por grano; **ahora la misma sinc** | Plegado < −98 dB (antes −5…−13) |
| Warp Tonos/Textura | PSOLA/WSOLA en tiempo, sin latencia | −32…−43 dB entre armónicos (ventanas); plegado no aplica |
| Motor C | Hasta 128 parciales, tabla de seno 4096 interpolada (error ≈ −130 dB), ruido por bandas con FFT; SSE x4 | Correcto; el coste manda a 96 kHz con muchas voces |
| Motor F | FFT 2^10–2^16 por etapas, fases aleatorias, graves en el centro < 120 Hz | 55 Hz lado −133 dB; cachés y reparto de trabajo añadidos |
| Envolventes | Exactas cada 8 muestras, lineales entre medias; ganancias con 3 ms de suavizado | Sin clics (auditoría) |
| Filtro por voz | SVF/escalera, 4x (2x a ≥ 88,2 kHz) con medias bandas IIR; MS2 con bloqueo de continua | THD y aliasing medidos; igual a las 4 frecuencias |
| Efectos | Rack en trozos de 128; módulos creados en el hilo de mensajes; Distorsión 8x/4x con alineación de la seca | Polaridad/nivel correctos con mix 0; DC bloqueada |
| Salida | A/B igualado en sonoridad (K), medidor, saturación suave a −0,6 dBFS | True peak −0,05 dBTP a +12 dB |
| Hilo de audio | 0 asignaciones, try-lock para el cambio de sample, atómicos para el resto | RtCheck 432 escenarios |

Pruebas con la compilación final: motor todas · Audit 352/352 · Snapshot 187/187 · RtCheck 0 · HostCheck OK.
