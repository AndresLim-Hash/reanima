# Auditoría completa de los motores A–F con toda la biblioteca (22-11-2026)

**Encargo:** probar los seis motores con **todos** los samples de «all samples», separar defectos reales de rasgos del material o del diseño, arreglarlos sin perder carácter y demostrar la mejora sin regresiones.

Datos y scripts: `research/auditoria_completa_2026-11-22/`. Banco: `ReanimaBatch calidad` (`tests/Batch.cpp`).

> **Cómo leer los datos**
> - **[medido]**: calculado sobre el audio renderizado.
> - **[causa]**: comprobada en el código y con renders concretos.
> - **[diseño]** o **[artefacto]**: no es un defecto del motor; se explica por qué.
> - **Escucha:** nada de esto está valorado de oído todavía. Los A/B están en `research/auditoria_completa_2026-11-22/escucha/`.

## 1. Cobertura y método

- **Samples:** los 7.289 legibles de «all samples» (inventario `research/biblioteca_2026-11-22/inventario.json`). Ninguno se queda fuera; ninguno se ha modificado.
- **Pasadas:** cada una de 7.289 samples × 6 motores × 6 casos = **262.404 renders**.
  - **v1:** versión instalada (punto de partida).
  - **v2:** primeros arreglos (puntos 1–4).
  - **v3:** A–D con la corrección de las colas.
  - **v3ef:** E y F con los cambios hechos tras tu escucha.
  - **vfinal:** unión de v3 (A–D) y v3ef (E y F), con `unir.py`. Es el código instalado.
  - Ninguna dio valores no finitos (NaN o infinito).
- **Los seis casos de cada sample:**
  1. raíz;
  2. +7 semitonos;
  3. −12 semitonos;
  4. velocidad suave;
  5. acorde de tres notas;
  6. «repetida»: la misma nota cuatro veces, cada 150 ms.
- **La nota** se mantiene lo que dura el sample (entre 0,3 y 4 s) y luego se suelta, con 0,8 s de cola.
- **Siempre se compara con el motor A en el mismo caso:** el archivo tal cual, o remuestreado si se transporta. Así no se cuenta como defecto lo que ya está en el sample. Lo hace `analizar.py`.
- **Qué se mide y cuándo cuenta como defecto** (columnas del CSV):

  | Defecto | Cuándo cuenta |
  |---|---|
  | casi mudo frente a A | ≥ 20 dB por debajo de A. Regla añadida tras v1: E y F podían quedar mudos sin que otra regla lo viera. |
  | clics | Saltos de la segunda derivada 10 veces por encima de su entorno de 5 ms y sobre −54 dBFS, más que A. |
  | DC | Continua > −30 dB del RMS y 10 dB más que A. |
  | pico por encima | > 6 dB sobre el pico de A. |
  | caída / subida | Frente al sample en la raíz; contra sí mismo y 10 dB peor que A en el resto. |
  | timbre lejos | Fidelidad espectral > 6 dB en la raíz. |
  | ataque más lento | Más del doble que el sample + 15 ms. |
  | mono | Pierde > 3 dB al sumar a mono. |
  | cola tras soltar | 12 dB más que A. |
  | clic al soltar | Salto 12 dB mayor que A. |
  | notas repetidas desiguales | Dispersión de picos 3 dB mayor que A. |

- **Comprobación de regresiones tras cada cambio:**
  - en la biblioteca de referencia (120 samples, `research/biblioteca_2026-11-22/scripts/regresion.py`);
  - y luego en la biblioteca completa (v2 frente a v1).

## 2. Resultado de v1 (versión instalada) [medido]

Combinaciones sample × caso con defecto grave o medio, con la regla de «casi mudo» ya incluida:

| Motor | grave | media | samples afectados (de 7.289) |
|---|---|---|---|
| A | 13 | 41 | 54 |
| B | 240 | 3.504 | 2.017 |
| C | 13 | 4.049 | 3.065 |
| D | 212 | 3.401 | 1.964 |
| E | 3.175 | 9.498 | 4.016 |
| F | 5.561 | 10.610 | 6.445 |

Lo más grave: E y F **casi mudos** en 7.287 y 14.541 combinaciones.

## 3. Diagnóstico por grupo

### Defectos reales, arreglados

1. **B y D se callaban tras el primer golpe en stems** (un stem de claps solo sonaba el primero).
   - [causa] Pasada la caída ajustada, la envolvente y su modelo valían casi 0, y su cociente 0/0 dejaba la nota muda.
   - Arreglo: con la envolvente detectada, donde el modelo ha muerto pero el archivo sigue, el archivo suena tal cual (`renderSampled`, `modelBelowFloor`).
2. **Colas fantasma en B, C y D.**
   - Con un one-shot sostenido más allá de su final, A se calla y B/C/D seguían sonando: −12 dB durante ~1 s en un kick, y un riser sostenía su pico 1,1 s.
   - [causa] B y D reciclan la fuente de sostenido del final del archivo. C extrapola sus parciales con su caída propia, que es lenta.
   - Arreglo (decisión tuya: «caer al ritmo del final»):
     - nuevo análisis `endFallDbPerSec`: lo rápido que cae el sonido al acabar dentro del archivo;
     - nuevo análisis `soundEnd`: dónde acaba, es decir, el primer momento a 60 dB por debajo de lo más fuerte;
     - pasado ese punto, B/C/D caen a ese ritmo, solo con la envolvente detectada;
     - un archivo cortado mientras suena conserva la cola de antes.
   - **Regresión encontrada en v2 y corregida en v3:**
     - en la primera versión, C empezaba a caer en el último fotograma de su modelo, que en algunos samples llega antes que el final del sonido;
     - un bajo se callaba mientras el archivo aún sonaba a −33 dB, y una alarma perdía su último segundo;
     - B/D caían desde −40 dB y bajaban algo antes de tiempo la cola de un triángulo.
     - Ahora los tres empiezan a caer en `soundEnd`. Comprobado: esos samples quedan idénticos a v1 y las colas fantasma siguen eliminadas.
3. **F casi mudo con golpes** (kicks, cajas, claps, hats: 1.086 samples en la raíz).
   - [causa] F seguía la ADSR detectada (caída rápida, sostenido 0). Su primer fotograma llega a los ~0,17 s, cuando la envolvente ya está cerrada.
   - Arreglo (decisión tuya: «sostener con la detectada»):
     - con la ADSR detectada, F se mantiene mientras pulsas, al **nivel medio de la parte que suena** (`meanLevel`), nunca por debajo de su sostenido;
     - con otra ADSR, manda la tuya.
     - Primero probé sostener al nivel del golpe más fuerte: el pico quedaba hasta 5 dB por encima del sample. Por eso se usa el nivel medio.
4. **E y F leían en silencio** (E mudo en 597 samples en la raíz).
   - [causa] La Posición por defecto (30 %) caía en una pausa de loops y stems.
   - Arreglo (decisión tuya: «saltar al sonido más cercano»):
     - si el punto está 40 dB por debajo de lo más fuerte, E y F leen desde el siguiente punto con sonido (`readingPosition`);
     - el mando no se mueve, y la zona de la forma de onda enseña dónde lee de verdad.
5. **Kicks, hi-hats cortos y graves en E y F, tras tu escucha** («pasa de no oírse nada a oírse algo de calidad muy poco aceptable»).
   - Primer intento:
     - en E encogí el grano al tamaño de los golpes muy cortos;
     - medido, sonaba a tartamudeo (aspereza 9–12 dB frente a 4 dB del original).
     - Con F, los kicks y el bajo sub quedaban emborronados o «rugían».
   - Arreglo (decisión tuya: «tocar el golpe limpio» y «Coherente en graves»):
     - `playingEngine`: E y F tocan el golpe como A cuando el sonido dura menos que el grano (E) o es un golpe grave sin sostenido;
     - un golpe grave sin sostenido es: sostenido < 0,3 y más de la mitad de la energía por debajo de 120 Hz (dato nuevo `lowShare`);
     - en la biblioteca, los kicks dan 0,87–0,99 de esa energía, y los claps y hats menos de 0,01, así que los claps no cambian;
     - E coloca los granos en fase por sí mismo cuando la nota del sample está por debajo de 150 Hz.
   - Tu veredicto (23-11): «algo mejor, nos quedamos con los cambios». Lo retomaremos más a fondo más adelante.

### No son defectos del motor (se quedan como están)

6. **C «subida de nivel» en loops** (2.274). Tres causas:
   - las colas fantasma (arreglado, punto 2);
   - C pasa al modo muestreado cuando no hay modelo de parciales (arreglado con el punto 1);
   - y el alisado propio de la resíntesis, que rellena los huecos entre golpes [diseño: C no es un motor para loops de batería].
7. **C «ataque más lento»** (838) [artefacto]: en loops y sonidos sostenidos, la medida de ataque coge otro golpe del loop.
8. **B/D «caída» en notas repetidas** (~300) [diseño, leve]:
   - al soltar, B cae un poco más deprisa que A en los primeros 5 ms (−32 frente a −19 dB);
   - los dos se apagan en 30 ms, antes de la nota siguiente.
9. **B/D «clics» en notas repetidas** [diseño]: es la saturación suave del techo de voz (curva tanh al nivel del pico del sample), con samples a fondo de escala y notas que se solapan. No es un corte del proceso.
10. **E «clics»** [artefacto]:
   - son flancos propios del sample (subidas de 0,2 en cinco muestras) colocados por los granos donde alrededor hay menos nivel;
   - en el sample hay el mismo nivel alrededor y el detector no los marca;
   - el resto son falsos positivos junto al umbral (saltos de 0,001–0,003).
11. **E «DC»** [artefacto]: se medía sobre salidas casi mudas (E a −86 dB). Desaparece con el punto 4.
12. **«Clic al soltar»**:
    - en E: salidas casi mudas (un resto a −64 dBFS);
    - en C (159, casi todos loops): un golpe del loop cae justo al soltar la nota. Es contenido, no un clic.
13. **B/D «pico por encima» a −12** [diseño]:
    - B/D mantienen el tiempo al transportar, y A se ralentiza como una cinta;
    - en un riser, B llega en la ventana medida a la parte fuerte y A no.
14. **E/F «notas repetidas desiguales»** [diseño]: los granos de E y las fases de F son aleatorios, y cada golpe es distinto.
15. **E «pérdida al sumar a mono»** [diseño, sin verificar con Ancho 0]: los granos se reparten por el estéreo.
16. **«Cola tras soltar» en E, B y D** [diseño, se deja como observación]:
    - la Release detectada es larga en muchos samples (mediana 1 s en one-shots; > 2 s en el 13 % de los loops);
    - E, B y D sostienen la textura durante la Release, mientras que A se queda sin archivo.
17. **F con notas muy cortas** (repetida, 120 ms) [límite del motor]:
    - con la FFT por defecto (0,34 s), F tarda ~0,17 s en sonar;
    - una nota más corta no llega a oírse; un tamaño de FFT menor lo acorta.
    - Queda como posible mejora futura: arrancar sin esa espera.

## 4. Regresiones en la biblioteca de referencia (120 samples × 6 motores × 6 casos) [medido]

`linea_base/calidad_aud1.csv` (con el arreglo de stems) frente a `calidad_aud2.csv` (con los tres arreglos):

- **A, B, C, D:** fidelidad mediana sin cambios (0,00; 0,00; 2,31; 0,00 dB).
- **B/C/D, cada «empeoramiento» revisado:**
  - el loop de batería en acorde y el riser a +7 ahora **coinciden con A** (antes sostenían lo que A ya no tiene);
  - los «clics al soltar» que pasan a 0,0 son silencio antes y después de soltar: el detector da 0 cuando no hay nada.
  - Ninguno es una regresión audible.
- **Mejoras:** 397 colas tras soltar, 114 clics al soltar, 39 subidas y 17 caídas.
- **E y F:** casi todos los cambios vienen de que ahora suenan donde antes había silencio. Las métricas de caída, subida, ataque y cola se aplican ahora a sonido real.

Con el código final (`calidad_aud3.csv`), B/C/D quedan en 14 «empeoramientos» en la medida, los mismos casos ya explicados. Mejoran 464 colas tras soltar, 193 clics al soltar, 148 caídas y 139 subidas.

## 5. Resultado final (vfinal) frente a v1, toda la biblioteca [medido]

Samples con algún defecto grave o medio, de 7.289:

| Motor | v1 | final | graves v1 → final |
|---|---|---|---|
| A | 54 | 54 | 13 → 13 |
| B | 2.017 | 1.936 | 240 → 221 |
| C | 3.065 | 3.063 | 13 → 12 |
| D | 1.964 | 1.882 | 212 → 193 |
| E | 4.016 | 3.357 | 3.175 → 1.294 |
| F | 6.445 | 5.300 | 5.561 → 1.831 |

Por tipo (combinaciones sample × caso, graves y medias, v1 → final):

| Tipo | B | C | D | E | F |
|---|---|---|---|---|---|
| casi mudo frente a A | 96 → 96 | 0 → 0 | 97 → 97 | 7.287 → 4.306 | 14.541 → 4.714 |
| cola tras soltar | 1.377 → 910 | 164 → 163 | 1.373 → 907 | 1.715 → 1.649 | 0 → 0 |
| clics | 393 → 381 | 54 → 54 | 333 → 321 | 842 → 736 | 2 → 9 |
| clic al soltar | 14 → 15 | 159 → 161 | 14 → 15 | 214 → 133 | 0 → 0 |
| caída de nivel | 316 → 302 | 123 → 122 | 259 → 245 | — | — |
| subida de nivel | 973 → 965 | 2.274 → 2.273 | 974 → 966 | — | — |
| timbre lejos del sample | 9 → 2 | 197 → 196 | 8 → 1 | — | — |
| DC | 11 → 8 | 5 → 5 | 7 → 5 | 223 → 177 | 1 → 4 |
| pico por encima del sample | 451 → 449 | 198 → 198 | 450 → 448 | 1.388 → 1.520 | 860 → 1.143 |
| notas repetidas desiguales | — | — | — | 562 → 477 | 767 → 770 |
| pérdida al sumar a mono | 73 → 73 | 50 → 50 | 71 → 71 | 442 → 419 | 0 → 2 |

Cómo leerlo:

- **F casi mudo:** fuera de las notas repetidas, baja de 9.076 a 372 combinaciones.
  - Lo que queda son sobre todo las notas de 120 ms del caso «repetida» (4.342). F tarda ~0,17 s en sonar: es un límite del motor (punto 17).
- **E casi mudo:** baja a 3.017 fuera de «repetida».
  - Solo 186 son graves: sobre todo loops de batería, hats, cajas y claps, donde el punto de lectura es más flojo que la media del sample.
  - Por debajo de −20 dB frente a A, la mayoría están entre −20 y −30 dB. Es el nivel del punto que se lee, no silencio.
- **B/D:** desaparecen 529 defectos cada uno y aparecen 17 en la medida.
  - Revisados uno a uno, son casos que ahora **coinciden con A**: un kick repetido que ya no rellena los huecos, loops y risers que terminan donde termina el archivo.
  - Los «clic al soltar» de 185 dB son cuentas sobre silencio.
- **Los «picos por encima del sample» de E y F suben** porque ahora suenan donde antes no: 1.143 casos en F, sobre todo en acordes. El techo suave de la voz los redondea.
- **C:** sin cambios de fondo (−4 / +2).

## 6. Escucha, pruebas e instalación

- **Escucha:** `research/auditoria_completa_2026-11-22/escucha/` (versión 2, con LEEME.txt). Aprobado por ti el 23-11.
- **Baterías con el equipo libre:**
  - ReanimaTests: 151/151;
  - RtCheck: sin asignaciones en el hilo de audio;
  - Snapshot: interfaz OK;
  - HostCheck: VST3 OK, 8 instancias;
  - ReanimaAudit: 515/515.
- **Instalado** el 23-11 en `C:\Program Files\Common Files\VST3\Blackpanda\Reanima.vst3`:
  - sha256 `bb5c1f24de460061…`;
  - HostCheck OK sobre el instalado;
  - copia de la versión anterior en `backups/instalado_antes_auditoria_completa_2026-11-23`.
- **No hay nada guardado en git** (como pediste).

## 7. Pendiente para más adelante

- **E y F con kicks, hats y bajos, a fondo** (decidido contigo): hoy tocan el golpe tal cual, que es correcto pero no es una textura.
- **F con el bajo sub:** sigue algo áspero (5,6 dB de aspereza frente a 4,7 del original) por sus fases aleatorias.
- **F sin la espera inicial de ~0,17 s.**
- **La Release detectada en loops** puede ser muy larga (hasta 20 s).
