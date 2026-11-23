# Auditoría técnica integral de Reanima (23-11-2026)

**Punto de partida:** el commit `0238236` (todo el trabajo hasta el 23-11).

**Commits de la auditoría:**

| Commit | Contenido |
|---|---|
| `509650c` | Errores de memoria, robo de voces y optimizaciones |
| `85fe7bb` | Frecuencias de muestreo |
| `733f3e2` | Estado y sincronía |
| siguientes | Presets e informe |

**Encargos** (son dos, unidos en uno):
- auditoría técnica: errores, rendimiento y latencia;
- revisión integral: recursos, frecuencias de muestreo, sincronía, calidad, uso intensivo, ciclo de vida, persistencia, interfaz y coherencia.

**Criterios acordados:**
- **Se corrige directamente:** los errores confirmados y las optimizaciones idénticas bit a bit.
- **Se propone con A/B antes de aplicarse:** lo que cambie el sonido, aunque sea para corregirlo.
- **Objetivo de rendimiento:** buffer de 64 muestras, 32 voces, 48 kHz.
- **Latencia:** máxima calidad sin perder la posibilidad de tocar en directo (límite de trabajo: unos 5 ms).

**Datos, scripts y resultados de cada prueba:** `research/auditoria_tecnica_2026-11-23/`.

**Archivos de escucha:** `release/escuchar_auditoria_2026-11-23/`.

> **Cómo leer este informe**
> - **[medido]**: comprobado con una prueba que se puede repetir (el comando está indicado).
> - **[revisión]**: leído en el código, sin prueba propia.
> - **[hipótesis]**: interpretación sin demostrar.
> - **Escucha:** nada de esto se ha valorado de oído. Lo que cambia el sonido espera tu A/B.

---

## 1. Resumen por gravedad

| # | Gravedad | Problema | Estado |
|---|---|---|---|
| 1 | **Alta (sonido)** | El sostenido **Granular** de B y D (su modo por defecto) **desafina**: un peine de 50 Hz fuerza los parciales a su rejilla (440 Hz → 450 Hz; en samples reales, 26 cents de media y hasta 53) | Corrección preparada, **desactivada hasta tu A/B** |
| 2 | Alta (estabilidad) | Quitar el plugin con un render de Rubber Band en marcha podía escribir en memoria ya liberada o cortarse a la fuerza | **Corregido** |
| 3 | Media | Render de Rubber Band identificado por dirección de memoria: otro loop podía sonar con el render del anterior y no rehacerse nunca | **Corregido** |
| 4 | Media | Cambiar la frecuencia de muestreo dejaba sonar el estado viejo de «Graves mono»: ráfaga de ~1 ms al reanudar | **Corregido** |
| 5 | Media | Un proyecto sin su sample (ni archivo ni copia) no avisaba | **Corregido** (aviso con la ruta) |
| 6 | Media | En exportaciones con bloques grandes (más de 4096 muestras), Impronta y la fuente Sidechain se quedaban sin señal lateral | **Corregido** |
| 7 | Media | Cambiar de motor con una nota sonando: hacia C se queda casi en silencio (−59 dB) y hacia F se corta; hacia B y D, salto de ×1,7 | **Propuesta** (cambia el comportamiento) |
| 8 | Media | Más de 8 notas nuevas en 4 ms con 32 voces sonando: las que pasan de la reserva se cortan de golpe (clic) | Mejorado en parte; **propuesta** para el resto |
| 9 | Baja | La curva dibujada de F se guardaba con 2 decimales (cambiaba hasta 0,005 dB al reabrir) | **Corregido** |
| 10 | Baja | Historial de deshacer sin límite de memoria: 100 ediciones de un sample largo podían retener más de 4 GB | **Corregido** (máximo 512 MB) |
| 11 | Baja | Fuga posible en la previsualización del explorador (un clip perdido) | **Corregido** |
| 12 | Baja | La estimación de tonalidad retenía el análisis entero (decenas de MB) hasta entregar su mensaje | **Corregido** |
| 13 | Baja | Robo de voces: se cortaba la voz más antigua aunque otra estuviera casi apagada | **Corregido** |
| 14 | Prueba | `EngineTests` leía fuera de su lista de nombres (lo encontró AddressSanitizer) | **Corregido** |

(Las secciones siguientes dan el detalle, las pruebas y las cifras.)

---

## 2. Errores corregidos: causa, alcance y prueba

Todas las correcciones se han verificado con la **prueba nula**: 22 configuraciones que cubren los seis motores, filtros con circuitos, cadenas de efectos, unison y Mecanismo, matriz y LFO, Armónico, Movimiento, Deriva e Impronta, LOOP Beats, Complex y SLICE. Cada una toca 6–7 minutos de notas. **Las 22 son idénticas bit a bit a las de `0238236`** [medido] (`research/auditoria_tecnica_2026-11-23/nulltest.sh`).

1. **Rubber Band al cerrar el plugin** [revisión + medido]
   - **Causa:** el render de Rubber Band se hace en su propio hilo, y el destructor del plugin no lo esperaba. El hilo solo se esperaba al final, con el tiempo máximo interno de JUCE (5 s).
   - **Efecto:** quitar el plugin con un render largo en marcha podía escribir en un objeto ya medio destruido o cortar el hilo a la fuerza.
   - **Corrección:** el render se puede cancelar entre trozos y el destructor lo cancela y lo espera.
   - **Prueba:** cerrar con un render en marcha tarda **17 ms** (`ReanimaAudit tecnica`).
2. **Render de Rubber Band de otro loop** [revisión]
   - **Causa:** el render reconocía su sample por la dirección de memoria, sin mantenerlo vivo.
   - **Efecto:** un loop cargado después, en la memoria que dejó otro y con la misma duración (habitual en packs de 4 compases), podía sonar con el render del anterior y no volver a calcularse nunca.
   - **Corrección:** el render mantiene vivo su sample y se retira al cambiar de sample. Además, se libera por audio reproducido y no por tiempo de reloj: antes, con el host en pausa durante un fundido, podía liberarse estando aún en uso.
3. **Ráfaga al cambiar la frecuencia de muestreo** [medido]
   - **Causa:** los filtros de «Graves mono» guardaban la última nota y no se reiniciaban.
   - **Efecto:** al reanudar sonaba una ráfaga de ~1 ms antes de cualquier nota, de hasta 0,07 del máximo.
   - **Corrección:** se reinician con el motor. La prueba da ahora 0,0000 en los 18 casos (`ReanimaAudit frecuencias`).
4. **Sample perdido sin aviso** [medido]
   - **Efecto:** un proyecto sin archivo ni copia del sample se abría sin decir nada.
   - **Corrección:** ahora avisa con la ruta buscada y conserva los ajustes (`ReanimaAudit estado`).
5. **Sidechain en exportaciones** [revisión]
   - **Causa:** la copia de la señal lateral tenía sitio para 4096 muestras.
   - **Efecto:** los DAW que exportan en bloques mayores (FL Studio, por ejemplo) dejaban sin señal lateral a Impronta y a la fuente Sidechain.
   - **Corrección:** ahora hay sitio para 65 536, reservado fuera del hilo de audio.
6. **Curva dibujada de F** [medido]
   - **Causa:** se guardaba con 2 decimales.
   - **Corrección:** ahora con todos los dígitos de un float. Tras reabrir el proyecto queda exacta.
7. **Historial de deshacer** [revisión]
   - **Causa:** cada edición del audio guarda una copia entera del sample, y había 100 pasos sin límite de memoria.
   - **Corrección:** se descartan los pasos más antiguos si las copias pasan de 512 MB.
8. **Previsualización del explorador** [revisión]
   - **Efecto:** con la papelera llena, un clip se perdía sin liberarse.
   - **Corrección:** el hilo de audio espera un bloque antes de tomar el clip nuevo.
9. **Estimación de tonalidad** [medido]
   - **Causa:** el mensaje pendiente retenía el análisis entero del sample, unos 20 MB.
   - **Efecto:** con el hilo de la interfaz ocupado o parado, se acumulaban: el banco medía +15–20 MB por instancia creada y destruida.
   - **Corrección:** el mensaje ya no retiene nada. Procesando mensajes, como en un DAW, la memoria se mantiene estable (`ReanimaCpuBench memoria`).
10. **Robo de voces** [medido]
    - **Antes:** sin voces libres, se cortaba la más antigua, aunque estuviera sonando entera.
    - **Ahora:** se corta la que está más avanzada en su fundido.
    - **Prueba:** 8 notas de golpe sobre 32 voces, ningún corte (salto ×0,71 del normal).
11. **`EngineTests`** [medido con AddressSanitizer]: leía 11 nombres de una lista de 6. Era un error de la prueba, no del plugin.

---

## 3. Propuestas que cambian el sonido o el comportamiento (necesitan tu aprobación)

### 3.1 Sostenido Granular de B y D desafinado (prioridad 1)
**Causa:** el sostenido lanza un grano cada cuarto de grano desde el mismo sitio del sample.
- Esa repetición regular crea un peine de frecuencias en una rejilla de 4/grano Hz: 50 Hz con el grano de 80 ms por defecto.
- Los parciales del sostenido se desplazan a la línea más cercana [medido]:
  - un tono de 440 Hz sostenido suena a **450 Hz**;
  - con grano de 40 ms, la nota de 659 Hz sale a 700 Hz (+101 cents);
  - con grano de 100 ms, a 640 Hz.
- Afecta a **Sostenido + Granular**, que es el sostenido por defecto de B y D. En Natural afecta a la cola, donde entra el sostenido.

**Por qué no lo vieron las auditorías anteriores:** medían el sostenido Natural y los modos Loop y Freeze, que están bien.

**Corrección preparada** (`LoopGrains::inTune`, **desactivada**):
- los granos de un sonido con nota entran separados por periodos enteros de la nota;
- **resultado** [medido], frente al mismo sostenido en modo Loop como referencia, en 8 samples reales (bajo eléctrico y sintético, chelo, flauta, voz, pad, cuerdas y piano):

| Versión | Desviación media | Desviación máxima |
|---|---|---|
| Antes | 26 cents | 53 |
| Corregida | 11,7 cents | 50 |

- quedan **exactos** el tono de prueba, el bajo sintético, el chelo y el pad;
- el piano pasa de +33 a +8 cents;
- la voz, la flauta y las cuerdas, con vibrato, acordes o parciales inarmónicos, siguen con desviaciones de algunos parciales.

**Probada y descartada:** intervalos aleatorios para todos los sonidos (empeora: el tono queda a −13 cents).

**A/B:** `release/escuchar_auditoria_2026-11-23/1_sostenido_granular/`. Cada sample en tres versiones: `__1_antes`, `__2_corregido` y `__3_referencia_loop`.

**Si lo apruebas:** activarlo es cambiar una línea (`inTune = true`). Cambia el sonido de B y D con Sostenido Granular en tus proyectos: se afinan.

### 3.2 Cambiar de motor con una nota sonando (prioridad 2)
- **Medido:** hacia B y D la nota sigue, con un salto de ×1,7; hacia E baja −3,6 dB; hacia C queda a −59 dB; hacia F se corta.
- **Causa:** cada voz lee el motor en cada bloque y cambia de algoritmo en seco. C y F necesitan preparar su estado al empezar la nota, y no lo tienen.
- **Propuesta:** que cada nota conserve el motor con el que empezó y que las nuevas usen el nuevo, como en la mayoría de sintes.
- **Coste:** bajo. **Riesgo:** cambia lo que pasa al automatizar el motor.

### 3.3 Ráfagas de notas por encima de la reserva de voces (prioridad 3)
- **Medido:** con 32 voces sonando, hasta 8 notas nuevas de golpe se apagan sin cortes. Con 24 notas, las 16 que pasan de la reserva se cortan (salto ×4,5, un clic).
- **Opciones:**
  - **(a)** subir la reserva de 8 a 24 voces: más memoria, sobre todo con F y LOOP, que reservan estiradores y flujos por voz;
  - **(b)** calcular de antemano los 4 ms de fundido de la voz robada: sin memoria extra, pero con un pico de CPU en ese bloque.
- **Recomiendo (a)** si te parece bien el aumento de memoria (se mide al aprobarlo).

### 3.4 FFT optimizada (pffft) en el motor C (y quizá F) (prioridad 4)
- **Medido:** la FFT genérica de JUCE es el 17 % del tiempo de C con 36 voces.
- pffft ya está en el proyecto, dentro de Bungee, con licencia compatible. Sería varias veces más rápida [hipótesis: 3–4×].
- **Inconveniente:** no es idéntica bit a bit (diferencias del orden de −140 dB).

### 3.5 Motor F con tonos puros graves (prioridad 5)
- F, como PaulStretch, sitúa cada parcial en el centro de su banda de FFT.
- Con un tono puro la afinación se desvía hasta media banda: −12 a +4,5 cents a 440 Hz [medido]. **En notas graves, la desviación en cents es mayor** [hipótesis: decenas de cents a 55 Hz].
- **Propuesta:** estimar la frecuencia real de cada parcial (fase entre cuadros). Cambia el carácter de F, así que necesita A/B.

### 3.6 Revisión de energía del sostenido (prioridad 6, del encargo Color Play)
Lo de 23-11 sigue pendiente de tu decisión:
- el temblor de nivel del sostenido Granular (±4–5 dB en 18 de 82 samples) **está ligado al problema 3.1**;
- el sostenido en contrafase de la caja de música.

---

## 4. Lo que funciona como se acordó (pruebas superadas)

Todas son repetibles con `build_cp/ReanimaAudit_artefacts/Release/ReanimaAudit.exe <modo>`.

| Prueba | Resultado |
|---|---|
| Auditoría general (`ReanimaAudit`, todas las frecuencias, mandos muertos, efectos, resistencia) | **516/516** |
| `frecuencias` (nueva) | **25/25** |
| `sincronia` (nueva) | **7/7** |
| `estado` (nueva) | **10/10** |
| `presets` (nueva) | **4/4** |
| `tecnica` (nueva) | **6/6** |
| `colorplay` | 33/33 |
| `loops` | 103/103 |
| `tonalidad` | 34/34 |
| `arp` | 17/17 |
| `golpes` | 17/17 |
| `ventana` | 12/12 |
| `aros` | 6/6 |
| `timbre` | 5/5 |
| `explorador` | 4/4 |
| Pruebas unitarias (`ReanimaTests`) | todas, salvo una de tiempo del motor F medida con el equipo cargado (se repite en la sección 6) |
| Interfaz (`ReanimaSnapshot`) | **219/219** y «UI OK» |
| Tiempo real (`ReanimaRtCheck`) | **ninguna reserva ni liberación de memoria en el hilo de audio**, en todos los escenarios, ahora también con Armónico, Movimiento, Deriva y Sidechain |

**Interfaz (`ReanimaSnapshot`):** controles sin solaparse, tooltips en todos, cada «?» abre su guía y cada paso apunta a su control, tour de bienvenida por instancia, A/B al mismo nivel y vúmetro.

### 4.1 Frecuencias de muestreo [medido]
- **Sin conversión previa:** los samples no se convierten de antemano; se leen con sinc a la velocidad de la relación de frecuencias. No hay conversiones duplicadas.
- **Mismo sonido a cualquier frecuencia:** un sample a 44,1 kHz en proyectos a 32, 44,1, 48, 88,2, 96, 176,4 y 192 kHz suena en **cada motor igual que a 48 kHz** (nivel ±0,5 dB, afinación ±1,5 cents). **A y C quedan a 440 Hz exactos.**
- **Cambio de frecuencia con una nota sonando** (48→96, 96→44,1 y 44,1→88,2): se reanuda limpio (0,0000 de residuo, tras la corrección 3), sin NaN, y la nota nueva con su afinación.
- **Se recalcula todo** al cambiar de frecuencia: filtro, estiradores, efectos, etapa de salida, Movimiento, seguidor de la señal lateral, tonalidad lateral y latencia declarada.
- **Particularidades del método** (no son errores de frecuencia): el sostenido granular de B/D y la nube de E dispersan la afinación de un tono puro, y F la cuantiza en bandas de FFT (sección 3).

### 4.2 Sincronía, LOOP y SLICE [medido]
- **Independencia del tamaño de bloque:** LOOP Beats, Complex y SLICE dan **el mismo audio, bit a bit**, en bloques de 64, 128, 512 y 33. Complex Pro, también con 64, 128 y 512. Con bloques de 33 difiere unos −50 dB (la librería Signalsmith reparte su trabajo por bloques); los DAW usan potencias de 2.
- **Cambio de tempo con una nota de LOOP sonando** (100 → 140 BPM): sin saltos (×0,97 a ×1,38 del salto habitual del propio loop).
- **Transporte, tempo del host, Play, posición, parada, tono por MIDI, SLICE, Rubber Band y ventana:** 103/103.
- **MIDI con precisión de muestra:** latencia cero desde la nota (auditoría general).

### 4.3 Proyectos y presets [medido]
- **Guardado y reapertura exactos:** los **383 parámetros** al azar, 14 efectos y la curva dibujada vuelven exactos, también tras 5 ciclos y al reabrir a 96 kHz. Un acorde suena igual (−211 dB de diferencia).
- **Sample que falta:** sin el archivo, suena la copia guardada en el proyecto. Sin archivo ni copia, avisa y conserva los ajustes.
- **Estados dañados** (bytes al azar, cortado a la mitad, de otro plugin): se ignoran sin caerse.
- **Los 206 presets de fábrica:** rangos válidos, efectos reales, sin NaN, bajo el techo y ninguno mudo.
- **Pendiente de prueba real** [no verificado]: cerrar y reabrir el DAW de verdad (Ableton) y cambiar la frecuencia desde Ableton. Estas pruebas reproducen lo que hace el host (`getStateInformation` / `setStateInformation`, `prepareToPlay`), pero no son Ableton.

### 4.4 Interfaz, ayudas y tour [medido + revisión]
- Ver la tabla anterior.
- He revisado que los textos no prometan lo que no existe: no hay afirmaciones de latencia, polifonía ni funciones no implementadas. Las guías y ayudas de las funciones del 23-11 (Armónico, Movimiento, Deriva, Sidechain, Impronta) están al día.
- **No verificado:** la interfaz a escalas distintas de la de diseño en esta auditoría. Se comprobó el 17-11; desde entonces se han añadido el panel SIDECHAIN y los mandos del filtro Armónico.

---

## 5. Herramientas y límites del diagnóstico

- **Análisis estático (MSVC `/analyze`, 21 archivos):**
  - 15 avisos propios, todos **falsos positivos** revisados uno a uno: punteros de componentes que nunca son nulos, `1 << 15` que no desborda, una tabla de 22 copias que llega justo a 22 y pila grande en inicializaciones.
  - Un **código muerto:** `restart` en las colas de Beats se calcula y no se usa. Es el comportamiento que aprobaste de oído; no se toca.
- **AddressSanitizer** (accesos inválidos y desbordes; compilación `build-asan`):
  - pruebas unitarias, `colorplay` y `RtCheck`: **sin errores de memoria en el plugin**;
  - la auditoría general bajo AddressSanitizer corrió unas 3 h hasta la sección de mandos muertos, con **479 aprobadas y ningún error de memoria**. Las 5 que fallan son de tiempo (la instrumentación lo hace todo varias veces más lento) y pasan en la versión normal. La detuve ahí para medir la CPU.
- **No disponibles en este equipo:** ThreadSanitizer, UndefinedBehaviorSanitizer y LeakSanitizer (solo existen con Clang, que no está instalado). Las condiciones de carrera se han revisado leyendo el código: intercambio de sample, Rubber Band, previsualización, señal lateral, cortes de Beats y parámetros.
- **Fugas:** se han medido creando y destruyendo instancias completas repetidas veces (`ReanimaCpuBench memoria`).
- **No probado:** macOS / AU y otros DAW (Logic, Reaper, Cubase, FL Studio). No hay Mac aquí, y los demás DAW no están instalados.

---

## 6. CPU, memoria y latencia

### Cómo se ha medido
- **Herramienta:** `ReanimaCpuBench suite`, que mide el tiempo de cada bloque con el hilo en prioridad de audio («Pro Audio»).
- **Lectura de la carga:** 100 % = el bloque tarda lo que dura; por encima, corte.
- **Equipo:** Intel Tiger Lake, muestra de ingeniería a 2,6–3,1 GHz. Un portátil o sobremesa comercial suele ir más rápido.
- **Datos:** `research/auditoria_tecnica_2026-11-23/cpu/`; comparación con `cmp_cpu.py`.

### 6.1 Optimizaciones aplicadas (todas idénticas bit a bit, prueba nula de 22 configuraciones)
1. **Núcleo sinc:** la interpolación entre filas se hace con SSE, 4 a la vez, con las mismas operaciones.
2. **Elección del antialiasing de cada lectura:** antes calculaba un `log2` por muestra; ahora reutiliza el resultado mientras la velocidad no cambia (4–5 % del tiempo de B, D y E).
3. **Fundido hacia el sostenido de B/D:** `cos` y `sin` solo cuando la mezcla cambia.
4. **Filtro Armónico:** reutiliza pesos y coeficientes mientras la nota y los mandos no cambian. Efecto pequeño, dentro del ruido de medida.

### 6.2 Antes y después (48 kHz, 64 muestras, 73 escenarios: 6 motores × base/todo × 3 patrones × 2 samples, más los 206 presets) [medido]

| | Antes (`0238236`) | Después |
|---|---|---|
| CPU media (media de los escenarios) | 22,97 % | **20,33 % (−11,5 %)** |
| B, D y E | | **−15 a −25 %** |
| Bloques fuera de tiempo | **155** | **0** |
| B, todo, acordes (pluck): peor bloque | 208 % | 69 % |
| F, todo, acordes (pluck): fuera de tiempo | 101 | 0 |
| Cambio de los 206 presets sonando: fuera de tiempo | 25 | 0 |

(`cpu/comparacion_48_64.txt`)

### 6.3 El objetivo: 64 muestras, 32 voces, 48 kHz [medido]

| Configuración | 32 voces sostenidas: bloques fuera de tiempo | Carga media | Peor bloque |
|---|---|---|---|
| Todo activado (14 efectos, filtro MS2 4×, 8 filas de matriz) | **0 en los seis motores** | 38–71 % | 46–88 % |
| Todo + funciones nuevas (Armónico, Movimiento, Deriva, Sidechain, Impronta) | **0 en los seis motores** | | |

**Por encima del objetivo** (acordes que se solapan, 36 voces):
- picos sueltos de algunos bloques que **no se repiten de una ejecución a otra** [hipótesis: interrupciones del sistema en este equipo];
- F con todo activado roza el 100 %.

**Con 128 muestras a 44,1/48 kHz:** casi limpio. Hay 67 bloques en F con acordes de 36 voces, por la FFT del motor.

### 6.4 Límites que siguen [medido]
- **88,2 y 96 kHz con todo activado y 32 voces:** C y F no llegan, ni con 64 ni con 128 muestras. Ejemplos: C a 96 kHz con 32 voces, 116 % de media; F, 106 %. Es el límite ya documentado el 15-11: el coste por voz crece con la frecuencia. A, B, D y E sí llegan con 128 (alguna decena de bloques sueltos en B y D a 96 kHz).
- **Preset «Glaciar»** (F con FFT de 1,4 s y 6 s de release), con la cadena completa de 14 efectos e improvisación de muchas notas: 600 bloques fuera de tiempo en 3 minutos. Son demasiadas voces de F con la FFT más grande a la vez.
- **Lo inevitable frente a lo mejorable:**
  - **Inevitable** (coste del algoritmo):
    - el filtro saturado sobremuestreado 4× en cada voz (≈ 45 % del tiempo con el filtro MS2 puesto);
    - la resíntesis de C;
    - la FFT de F.
  - **Mejorable sin cambiar el sonido:** hecho (6.1).
  - **Mejorable cambiando muy poco el sonido** (propuestas):
    - FFT pffft en C (sección 3.4);
    - procesar C y F a 48 kHz dentro de proyectos a 88,2/96 kHz y subir la frecuencia al final [hipótesis: mitad de coste; cambia algo los agudos de esos motores].

### 6.5 Memoria [medido]

| Situación | Memoria |
|---|---|
| Instancia vacía | ≈ 94 MB (la mayor parte, tablas compartidas: la biblioteca de Mecanismo, tablas sinc) |
| Sample analizado | +22 MB |
| B | +24 MB |
| F | +70 MB (40 flujos) |
| LOOP | +84 MB (40 estiradores) |
| Efectos | +14 MB |
| **Total, todo usado** | **≈ 310 MB** |

- **Sesión de 10 minutos con cambios de preset:** estable (294 → 376 MB al cargar motores, sin crecer después); al destruir la instancia vuelve a 91 MB.
- **Crear y destruir instancias completas 12 veces:** sin crecimiento cuando el host procesa sus mensajes. La retención que había sin mensajes está corregida (error 9).
- **Propuesta:** reducir la memoria de F y LOOP creando flujos y estiradores solo para las voces que los usan, en lugar de 40 de golpe. Ahorraría hasta unos 150 MB por instancia con F o LOOP. Es un cambio de arquitectura medio; no toca el sonido.

### 6.6 Latencia [medido]
- **Latencia declarada al DAW:** 38 muestras a 44,1/48 kHz (0,79–0,86 ms) y 31 a 88,2/96 kHz (0,32–0,35 ms). Es la de la etapa de salida sobremuestreada (FIR de fase lineal). **El retardo real medido con un impulso coincide exactamente** a las cuatro frecuencias (`ReanimaAudit tecnica`). Se vuelve a declarar al cambiar la frecuencia.
- **Motores:** latencia cero (MIDI con precisión de muestra). LOOP Complex/Complex Pro compensa el retardo de su estirador leyendo por delante. Rubber Band se calcula de antemano.
- **Respuesta:** los parámetros se leen cada bloque y se aplican cada 32 muestras (0,7 ms a 48 kHz), con rampas de 3–5 ms en las ganancias para no hacer clic.
- **Única espera conocida:** F tarda unos 0,17 s en sonar (su FFT). Está documentada desde el 22-11.
- **Conclusión:** la latencia total del plugin (0,8 ms) está muy por debajo del límite de tocar en directo (unos 5 ms). La etapa de salida en calidad «máxima» añadiría latencia sin beneficio medido; **no se cambia**.

---

## 7. Pendiente, ordenado por impacto

1. **Tu A/B del sostenido Granular corregido** (3.1): es el defecto de sonido más importante encontrado.
2. **Que cada nota conserve su motor** (3.2).
3. **Más voces de reserva para ráfagas** (3.3).
4. **Rendimiento de C y F a 88,2/96 kHz** (6.4): pffft, o procesarlos a la mitad de frecuencia.
5. **Memoria de F y LOOP por voz usada** (6.5).
6. **Probar en Ableton de verdad:** guardar, cerrar Ableton, reabrir; cambiar la frecuencia desde Ableton; Sidechain desde otra pista. Las pruebas de este informe imitan al host, pero no son Ableton.
7. **Compilar y probar en Mac** (AU, Logic).
8. **Interfaz a escalas distintas** con los paneles nuevos (SIDECHAIN, mandos del Armónico).
9. **F con tonos puros graves** (3.5) y la revisión de energía del sostenido (3.6).

**Instalación:** esta versión **no está instalada todavía**. La instalada en Ableton es la del 23-11 de las funciones Color Play, sin estas correcciones.

