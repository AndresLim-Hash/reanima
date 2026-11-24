# Optimización y decisiones pendientes (23-11-2026)

**Encargo:** reducir CPU, memoria y latencia sin perder nada de calidad, y resolver las decisiones pendientes (A–E y las demás que contestaste).

**Datos:** `research/optimizacion_2026-11-23/`.
- **Prueba nula:** `nulltest.sh`, 29 escenarios. Los seis motores, LOOP, efectos, funciones del 23-11, y 44,1, 48, 88,2 y 96 kHz.
- **CPU:** `cpu/`, medida con `ReanimaCpuBench suite`, buffer de 64 y todo activado.
- **Escucha:** `release/escuchar_optimizacion_2026-11-23/`.

> **Cómo leerlo**
> - **[medido]**: dato del banco.
> - **[A/B]**: cambia el sonido; necesita tu escucha antes de instalarse.
> - Nada está instalado ni guardado en git.

## 1. Optimización segura (idéntica bit a bit) [medido]

**Motor F, vectorizado con SSE haciendo las mismas operaciones en el mismo orden:**
- su FFT, dos mariposas a la vez;
- la lectura del sample (interpolación Hermite);
- la suma de salida en el anillo.

**Prueba nula:** 29/29 escenarios idénticos bit a bit frente a la versión `dc7f8d3`, también a 88,2 y 96 kHz.

| F, todo activado, buffer de 64 | CPU media | Bloques fuera de tiempo |
|---|---|---|
| 44,1–48 kHz | −5 a −10 % | acordes a 48 kHz: de 106 a 3 |
| 88,2–96 kHz | −10 a −13 % | aproximadamente la mitad |

**Lo que no se puede acelerar sin cambiar bits (estudiado):**
- **Filtro de voz:** ya está vectorizado y es recursivo. A 88,2 kHz o más ya baja solo a 2× de sobremuestreo.
- **Banco de osciladores de C:** ya está vectorizado. Su orden de suma fija el resultado.
- **FFT del ruido de C:** pffft la aceleraría, pero no es idéntica (−140 dB). Decidiste no usarla.

## 2. Decisiones aprobadas, aplicadas

- **A. Sostenido Granular afinado** (`LoopGrains::inTune` encendido).
  - La prueba nula confirma que solo cambian los 6 escenarios con sostenido Granular de B y D.
  - A/B en `release/escuchar_auditoria_2026-11-23/1_sostenido_granular`.
- **B. Cada nota conserva su motor**, release incluida; las nuevas usan el motor elegido.
  - [medido] Antes, al pasar de A a C la nota quedaba a −59 dB y al pasar a F se cortaba.
  - Ahora, hacia los cinco motores, la nota sigue igual: salto ×0,91 (como sin cambio) y −0,5 dB, que es la caída natural del pluck.
  - Comprobación nueva en `ReanimaAudit tecnica`.
- **C. Fundido anticipado de las voces robadas.**
  - Si no queda voz libre, el resto del fundido de 4 ms de la voz robada se calcula en ese momento y se mezcla desde un anillo reservado de antemano, en lugar de cortarla.
  - [medido] 24 notas nuevas de golpe sobre 32 sonando: salto ×0,71 (antes ×4,5, un clic). Ahora es una comprobación.
  - Sin memoria extra apreciable. Sin efecto (bit a bit) cuando no hay robo.

## 3. Cambios para tu A/B

### D. C y F a media frecuencia en proyectos de 88,2/96 kHz [A/B]
- **Qué hace:** la nota de C o F se calcula a 44,1/48 kHz y se sube a la frecuencia del proyecto con un filtro de media banda (Kaiser, 95 coeficientes):
  - plano hasta 20 kHz a 96 kHz (±0,0001 dB), con las imágenes 109 dB por debajo;
  - −0,12 dB a 20 kHz a 88,2 kHz.
- **Corregido durante la prueba:** F salía una octava más grave (la relación de tono se calculaba a la frecuencia interna). Medido y arreglado antes de presentártelo.
- **Resultado [medido]:**
  - mismo nivel (±0,05 dB) y misma afinación;
  - mismo espectro por bandas de 100 Hz a 22 kHz (±0,3 dB);
  - en las bandas casi vacías de 20–200 Hz, hasta ±2,6 dB.

| A 88,2/96 kHz | CPU media | Bloques fuera de tiempo |
|---|---|---|
| F | −19 % | −70 a −98 % |
| C | −11 a −18 % | 32 voces a 88,2 kHz: de 10.816 a 156 |

- **Acumulado:** con la optimización segura, F a 96 kHz con 32 voces pasa de ~110 % a ~76 % de un núcleo.
- **Escucha:** `1_C_y_F_a_96kHz`.

### Release de los loops [A/B]
- **Causa:** en el 92 % de los loops con Release > 2 s, la Release salía de dividir 60 dB entre la caída ajustada a todo el archivo, casi plana en un loop (3 dB/s → 20 s). Afecta a 680 de los 5.206 loops.
- **Cambio:**
  - en samples con 4 golpes o más, la Release es lo que tardan en apagarse sus propios golpes: la mediana de cada golpe cayendo 20 dB, ×3 para 60 dB, con un máximo de 2 s;
  - solo si sale más corta;
  - los proyectos guardados no cambian, porque guardan el valor del mando.
- **[medido]** En 120 loops al azar de los que pasaban de 2 s, cambia en 43. El 10 % más corto queda en ~0,2 s. Los demás son texturas cuyos golpes no caen 20 dB antes del siguiente.
- **Escucha:** `2_release_de_loops`.

### Contrafase del sostenido [A/B]
- **Causa:** la caja de música ya viene así de origen. Su cola llega a correlación −0,57, y el sostenido podía quedarse con ese tramo.
- **Cambio:** si la fuente de sostenido de B/D tiene más lados que centro, se estrecha hasta igualarlos, con la misma potencia total.
- **Límite:** no reproduje la anulación de 11 dB medida en la revisión. En el ajuste probado, la nota sostenida queda 1 dB más baja.
- **Escucha:** `3_contrafase_caja_de_musica`.

### F suena desde el primer momento [A/B]
- **Por qué no se calculan los fotogramas de golpe:** calcularlos al pulsar costaría ~16 ms de CPU por acorde de 8 notas en un bloque de 1,3 ms. Habría cortes.
- **Cambio:** mientras entran los fotogramas, granos de la zona central de lo que lee el primer fotograma rellenan lo que falta, a potencia constante. Sin picos de CPU.
- **[medido]:**
  - suena desde los primeros 20–40 ms (antes callaba ~0,1 s y tardaba ~0,2 s en llegar);
  - cello y pluck entran continuos;
  - en la voz el relleno queda ~9 dB por encima del drone durante 0,15 s;
  - en el clap hay un pico de ~+4 dB.
- **Escucha:** `4_F_arranque_inmediato`.

### ColorPlay CP5 · «Seguir» en el DRONE MAKER [A/B]
- **Qué es:** un mando nuevo, a 0 por defecto, que con 0 suena idéntico a antes.
- **Qué hace al subirlo:** el color del drone cambia como cambiaba el del sample al sonar:
  - 16 bandas, con un suavizado de 0,3 s;
  - se aplica dentro de F a cada fotograma, que vuelve después a su energía;
  - cambia el timbre, nunca el volumen.
- **Corregido durante la prueba:**
  - la primera versión seguía también el volumen del sample (el drone caía 24 dB con él);
  - la segunda compensaba con el reparto medio del sample y seguía moviendo el nivel ±10 dB.
- **[medido]** Mismo nivel (±1 dB). En el cello el brillo se mueve entre 4.900 y 5.900 Hz siguiendo al sample.
- **Escucha:** `5_F_seguir_color`.

### ColorPlay CP9 · mejoras de flujo [A/B donde aplica]
- **Hechas, con mando a 0 o «Libre» por defecto (sin cambios al cargar):**
  - **«Vel → Ataque»** (sección de amplitud): tocando suave el Ataque llega a ser hasta 4 veces más largo;
  - **«Ritmo»** en E (Libre, 1/4 … 1/32, tresillos): los granos nacen al tempo del DAW, a intervalos exactos.
- **Nota por el nombre del archivo** (hecha):
  - en un sample sin nota detectada, «Snap a C» usa la nota del nombre («Kick_F», «Piano_C4», «bass_F#2»), y el rótulo lo indica;
  - solo para samples cargados a partir de esta versión: el proyecto guarda la marca, y los antiguos no la tienen, así que suenan igual;
  - prueba con 13 nombres reales, incluidos 5 que no deben dar nota; todos correctos.
- **Botón «Original»** (hecho, en la tarjeta del sample):
  - mientras está encendido, las notas nuevas suenan como el sample tal cual: motor A, su envolvente, los ajustes por defecto y tu Volumen, sin filtro, Mecanismo, modulación ni efectos;
  - las que ya suenan no cambian, y no se guarda con el proyecto;
  - [medido] con motor E, filtro saturado y reverb puestos, la nota «original» queda a −100 dB del sample tal cual en A.

### F con la frecuencia real de cada parcial [A/B]
- **Qué hace:**
  - los picos tonales de cada fotograma se localizan y su frecuencia se estima por interpolación;
  - sus fases avanzan de un fotograma al siguiente según esa frecuencia (vocoder de fase con bloqueo en los picos);
  - el ruido entre picos sigue con fases al azar: F conserva su nube.
- **[medido] En tonos puros:**
  - antes se desviaba hasta 58 cents en graves (55 Hz +38 ct, 73 Hz −58 ct, 110 Hz −22 ct);
  - ahora, menos de 1,4 cents en todos;
  - un tono puro ya no fluctúa de nivel.
- **[medido] En sonidos reales:**
  - mismo nivel (±0,5 dB), algo más tonal;
  - **el bajo sub deja de sonar áspero** (5,2 → 4,2; el original, 4,3), que era uno de los pendientes;
  - CPU: 0–3 %, dentro del ruido de la medida.
- **Escucha:** `6_F_frecuencia_real`.

### E y F con golpes y graves, a fondo [A/B]
- **Lo que ya estaba:**
  - E y F tocan tal cual los golpes graves sin sostenido y los sonidos más cortos que el grano;
  - E se pone en fase sola en notas graves.
- **Nuevo:** con la frecuencia real, F podría volver a hacer drone con los kicks: el cuerpo grave se sostiene como un tono.
  - [medido] Es mejor que el drone de antes en 2 de 6 kicks, y parecido en el resto.
  - Por defecto sigue tu decisión (el golpe tal cual).
- **Escucha:** `7_F_con_kicks`, con las tres versiones.
- **Con hats en E:** el nuevo «Ritmo» permite redobles regulares al tempo.

## 4. Lo que decidiste dejar como está
- **E. Memoria de F y LOOP** (~70 y ~84 MB por instancia).
  - Liberarla o crearla por voz arriesga notas sin flujo al volver a elegir F o LOOP, o en ráfagas.
  - Sin cambios.
- **pffft:** no usado.

## 5. Latencia [medido]
- **Total del plugin:** 0,8 ms. Es la de la etapa de salida sobremuestreada (FIR de fase lineal), declarada al DAW y comprobada con un impulso.
- **Los motores:** 0 ms.
- **F a media frecuencia:** añade 0,25 ms solo a la voz de F (el filtro de media banda). Es inaudible y no se declara, porque la nota ya entra con su ataque.
- **La única espera grande:** la entrada de F (~0,2 s). La propuesta de arranque inmediato la cubre.
- **Sin cambios:** no he quitado ninguna etapa de calidad.

## 6. Pruebas finales (24-11, con el equipo libre)
| Prueba | Resultado |
|---|---|
| ReanimaTests | todas pasan, incluidas las nuevas: nota del nombre, «Original», F con 8 notas (1,71 ms de 5,33) |
| ReanimaAudit | 516/516 |
| RtCheck | sin reservas de memoria en el hilo de audio |
| Snapshot | interfaz OK |
| HostCheck | VST3 OK |

- **Prueba nula:** solo cambian los escenarios de propuestas activas; los otros 15 son idénticos bit a bit.
- **Corregido al final:**
  - **F con frecuencia real:** la búsqueda de picos pasa a una etapa propia del fotograma (con 8 notas y FFT de 1,4 s un bloque pasaba de la mitad de su tiempo).
  - **Anchura estéreo de F:** la separación entre canales de cada parcial oscila despacio, como máximo ¼ de ciclo; con una separación fija los lados quedaban 14–23 dB por debajo. Ahora a 1 kHz están a −4,5 dB y los graves siguen en el centro.
  - **Snapshot:** la prueba de «= nivel» deja que el medidor de A se asiente antes de medir. Ese medidor sigue 3 s de sonido, y la prueba arrastraba lo que habían sonado las comprobaciones anteriores.

### Pruebas del 23-11, 18:20
- **ReanimaTests:** todas pasan. Un fallo de CPU (F con 8 notas) salió con el equipo ocupado; repetido solo, pasa (2,18 ms de 5,33).
- **Snapshot:** OK.
- **Prueba nula (29 escenarios):**
  - cambian solo los afectados por propuestas activas: B/D (Granular afinado), F (arranque con granos), y C/F a 88/96 kHz (media frecuencia);
  - los otros 15, incluidos LOOP, A, E y efectos, son idénticos bit a bit.
- **Auditoría:** los dos mandos nuevos declarados como exclusivos de su motor.
- **Nota:** las propuestas a la espera de tu A/B (media frecuencia, Release de loops, contrafase, arranque de F) están activadas en el código. Si alguna no te convence, se desactiva cambiando una línea antes de instalar.

## 7. Pendiente
- **Tu escucha** de las 7 tandas de `release/escuchar_optimizacion_2026-11-23/`, más la del sostenido Granular.
  - Lo que no te convenza se desactiva cambiando una línea.
- **Instalado** el 24-11 a petición tuya (sha256 `0b8d4942…`, HostCheck OK sobre el instalado). Copia de la versión anterior en `backups/instalado_antes_optimizacion_2026-11-24`.
