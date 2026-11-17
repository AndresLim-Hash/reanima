# Rendimiento a 64 muestras — 2026-11-15

Equipo de medida: Intel Core 11.ª gen. (Tiger Lake-H, 8 núcleos, muestra de ingeniería a 2,6–3,1 GHz; un i7/i9 comercial
de 11.ª gen. llega a 4,6–5 GHz, así que este equipo está por debajo del objetivo mínimo). Una instancia, hilo con prioridad
de audio (MMCSS "Pro Audio"), tiempo de cada bloque medido aparte (`ReanimaCpuBench suite`). Carga = tiempo del bloque /
tiempo real del bloque (64 muestras = 1,33 ms a 48 kHz, 0,67 ms a 96 kHz).

## Criterios de aceptación
1. Sonido idéntico: prueba nula bit a bit contra la versión anterior (10 configuraciones, 6 motores, filtro en 3 circuitos, efectos, F con armónicos/curva/FFT 1,4 s).
2. 48 kHz / 64: **0 bloques por encima del 100 %** en todos los motores, con 4 voces, notas largas, granular, cadenas de efectos completas (14 ranuras), filtro y 8 ranuras de matriz; también con 32–36 voces sonando (releases largos).
3. Cambio de los 206 presets mientras suenan acordes: 0 bloques por encima del 100 %.
4. 0 asignaciones de memoria en el hilo de audio (ReanimaRtCheck); memoria estable en sesión larga.
5. Todas las pruebas existentes pasan (Tests, Audit 316, Snapshot 187, RtCheck, HostCheck).

## Resultados (48 kHz, 64 muestras; antes → después)
| | antes | después |
|---|---|---|
| Bloques > 100 % (78 escenarios, 2 samples) | 9 154 | **0** |
| Cambio de 206 presets | 50 bloques > 100 %, peor 181 % | **0**, peor 93–98 % |
| A todo, cello, acordes (36 voces) media / peor | 56,6 % / 74,8 % | 38,2 % / 57,3 % |
| C todo, cello, acordes media / peor | 92,7 % / 158 % (8 752 > 100 %) | 61,2 % / 89,5 % (0) |
| F todo, pluck, acordes p99.9 / peor | 132 % / 206 % | 83 % / 89 % |
| 4 voces con todo activado (todos los motores), media | 10,5–13,9 % | 7,4–10,9 % |

## Otras frecuencias (cello, 6 motores, 64 muestras)
- 44,1 kHz: 1 bloque > 100 % (F todo, 36 voces, 118 %); el resto, 0.
- 88,2 / 96 kHz con 4 voces y todo activado: 0 bloques > 100 % salvo 1 bloque en F a 96 kHz (120 %).
- 88,2 / 96 kHz con 32–36 voces y todo activado: **no cumple** (C, F y en menor medida A, B, D, E). A 96 kHz cada bloque de 64 dura 0,67 ms; en ese caso conviene un buffer de 128.

## Sesión larga (48 kHz, 15 min, motor A + todo + cambios de preset)
2 bloques > 100 % (101,6 %) en un preset pesado de motor F; memoria entre 225 y 283 MB sin crecer de forma continua (no hay fuga: sube al cargar los análisis de otros motores y vuelve).

## Qué se cambió (todo bit a bit igual)
- `Dsp::floorFast`: `std::floor` sin la llamada a librería (x64 sin SSE4.1), en lecturas, tabla de seno y fases.
- Motor C: `PartialBank`, los parciales de cuatro en cuatro (SSE2) sumados en el mismo orden.
- Filtro por voz: medias bandas del sobremuestreo 4x en estéreo SSE (`HalfBand::Stereo*`) y la ruta de estado variable L+R a la vez (`VoiceFilter::corePair`); el Morph calcula su mezcla una vez por bloque.
- Motor F: pesos de armónicos y de la curva dibujada guardados mientras no cambian (antes: log+exp por bin y frame, la etapa más pesada); el trabajo de los frames se reparte según la demanda de todas las notas (`Stream::demand`).
- Granular (E y Textura): lectura cúbica estéreo con una sola posición; sólo se visitan los granos activos (máscara de bits, mismo orden).
- Con alternativa escalar para ARM (MacBook) en todo lo vectorizado.

No se hizo: hilos propios dentro del plugin (compiten con los del anfitrión y producen picos); aproximaciones nuevas (rcp, FFT radix-4) que cambiarían el resultado.

## Corrección del filtro (decidida por el usuario, instalada: sha af3d63bc)
El sobremuestreo del filtro (circuitos OSR, MS2, SMP, PRD) procesaba cada par de muestras del diezmador interno en orden
invertido (dos llamadas como argumentos de otra; el compilador las evalúa de derecha a izquierda). Efecto medido en el
plugin (filtro MS2 con mezcla al 0 % frente a filtro apagado): −0,5 dB a 5–8 kHz, −1,4 dB a 8–12 kHz, −3,0 dB a 12–16 kHz,
−3,4 dB a 16–19 kHz; corregido: 0,00 dB en todas las bandas. Presets y proyectos con esos circuitos suenan ahora con sus
agudos completos. La versión con el orden antiguo (ya optimizada) está en `backups/Reanima_rendimiento_filtro_antiguo`.

Datos: `research/rendimiento_2026-11-15/` (CSV antes/después, perfiles, pruebas nulas). Copia previa: `backups/antes_rendimiento_2026-11-15`.
