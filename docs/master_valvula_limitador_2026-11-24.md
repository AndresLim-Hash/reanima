# Salida MASTER: válvula, OUTPUT y limitador True Peak (24-11-2026)

Estado: **aprobado de oído por el usuario e INSTALADO el 24-11-2026 (sha 398e670f; copia previa en `backups/instalado_antes_master_2026-11-24`). SIN commit.** Escuchas: las escuchas A/B están en `release/escucha_master_2026-11-24/` (con LEEME.txt).

Material de trabajo: `research/master_2026-11-24/` (renders, barridos, scripts y binarios de referencia `base_bin/`). Herramienta nueva: `ReanimaMasterLab` (`tests/MasterLab.cpp`). La etapa anterior se conserva para comparar en `tests/MasterStageLegacy.h`.

---

## 1. Cómo funcionaba antes (Fase 0)

**Confirmado leyendo el código** (`Source/MasterStage.h` y `PluginProcessor::processBlock` antes del cambio):

- No existían AMOUNT, CHARACTER ni OUTPUT. El único control de nivel de la salida era **Volumen** (de −60 a +12 dB, por defecto −6 dB), aplicado en `SynthEngine` después de los FX (Main y buses), de Mono y de Graves mono.
- Después de Volumen: un protector contra descontroles en `SynthEngine` (`limiterGain`, techo +12 dBFS, ataque instantáneo), que solo actúa con niveles absurdos.
- Luego, en el procesador: «= nivel» de la A/B, el vúmetro de SALIDA y, al final, `MasterStage`:
  1. ganancia fija ×0,631 (−4 dB);
  2. sobremuestreo 4× (2× a 88,2/96 kHz) con FIR de fase lineal de JUCE;
  3. una curva de **rodilla fija**: lineal hasta −4 dBFS; de −4 a −1 dBFS, un `tanh` que se aplana contra −1 dBFS;
  4. vuelta a la frecuencia del DAW;
  5. un «guard» `tanh` **sin sobremuestrear**, de −0,5 a −0,3 dBFS.
- Estéreo: los dos canales por separado, sin enlace.
- No había bypass ni parámetros: la etapa estaba siempre activa.
- Latencia declarada: 38 muestras a 44,1/48 kHz y 31 a 88,2/96 kHz.
- El vúmetro medía antes de la etapa, así que su 0 dB era justo el inicio de la rodilla.

**Comprobado con audio** (`research/master_2026-11-24/base_audit_master.txt` y `sweep_legacy.*`, 77 renders: 18 instrumentos en acordes de 1, 3, 4, 6, 8 y 10 notas):

| Vúmetro | THD de un tono | Distorsión media en acordes (dB bajo la señal) | Peor caso | Pico real de salida |
|---|---|---|---|---|
| 0 dB | −140 dB (limpio) | −140 | −110 | −3,6 dBTP |
| +1 dB | −59 dB | | | |
| +3 dB | −31 dB | −50 | −37 (bombo) | −1,3 dBTP |
| +6 dB | −18 dB | −32 | −20 (trompa) | −0,75 dBTP |
| +12 dB | — | −17,5 | −10 (trompa) | **+0,04 dBTP** |

- Batería sintética con el vúmetro a +6 dB: **+0,70 dBTP** a 48 kHz. El guard sin sobremuestrear deja pasar los picos entre muestras.
- Con Volumen a −6 dB (por defecto), un acorde de 10 notas ya llega a +5,7 dB en el vúmetro.

## 2. Esquemas: antes y después

```
ANTES
FX Main/buses → Mono/Graves mono → VOLUMEN → [protector +12 dBFS] → "= nivel" → [vúmetro]
   → ×0,631 → 4× → rodilla fija −4…−1 dBFS → 1× → guard tanh a 1× (−0,5…−0,3 dBFS) → salida

AHORA
FX Main/buses → Mono/Graves mono → VOLUMEN → [protector +12 dBFS] → "= nivel" → [vúmetro = ENTRADA]
   → ×0,631 → 4× ┬─ (válvula apagada: nada) ────────────────┬→ 1× → OUTPUT → LIMITADOR TRUE PEAK → salida
                 └─ VÁLVULA (AMOUNT, CHARACTER) ─ fundido 30 ms ┘          (lee 8× entre muestras,
                                                                             mira 1,5 ms por delante)
```

## 3. Por qué se rompía el sonido al subir el nivel

- **Causa (confirmada por código y por medida):** el control que se subía (Volumen) estaba **antes** de una etapa con techo fijo a −1 dBFS y solo 3 dB de rodilla (de −4 a −1 dBFS).
- Pasado el 0 dB del vúmetro, cada dB más no daba volumen: se convertía en **recorte**.
  - THD de −140 a −18 dB en solo 6 dB.
  - Por encima, la onda queda plana arriba: casi un recorte duro.
  - El pico de salida se queda clavado en −1 dBFS y el nivel no sube.
- En acordes, el recorte de la suma produce **intermodulación**: notas falsas y «barro». Es lo que suena a roto.
- Encima, el guard final trabaja sin sobremuestrear: crea aliasing y deja pasar picos entre muestras (+0,7 dBTP).
- Hipótesis descartadas: ni los FX, ni el protector de +12 dBFS, ni el techo por voz intervienen en este rango.

## 4. Qué se conserva, qué se corrige y qué se sustituye

| Pieza | Decisión | Por qué |
|---|---|---|
| −4 dB de margen (0 dB del vúmetro → −4 dBFS) | **Se conserva** | Mantiene el nivel de todos los presets y proyectos. |
| Sobremuestreo 4×/2× con FIR de fase lineal (JUCE) | **Se conserva** | Comparado con 8× (punto 6): no mejora nada audible. El camino limpio es el mismo de antes. |
| Rodilla fija −4…−1 dBFS | **Se sustituye** por la válvula (opcional) + limitador | Era la causa del problema. |
| Guard a 1× | **Se elimina** | Aliasing y picos entre muestras. Lo sustituye el limitador True Peak. |
| Protector +12 dBFS del motor | **Se conserva** | Solo para descontroles (resonancias, realimentación). |
| Volumen | **Se conserva** en su sitio | Cambiarlo alteraría todos los proyectos (ver limitaciones). |

## 5. Arquitectura y decisiones DSP

**Válvula** (`MasterStage`, sobremuestreada 4×; 2× a 88,2/96 kHz):

- **Curva:** `g(s) = tanh(d·s) / (d·(1 + b·tanh(d·s)))`.
  - Ganancia exactamente 1 para señales pequeñas: AMOUNT no cambia el volumen de lo flojo.
  - Redondea los picos al crecer (3.º armónico).
  - Con `b > 0`, comprime más la mitad positiva que la negativa. Es la asimetría de un triodo: 2.º armónico, «cálido».
  - Es analítica: sin esquinas ni recortes.
- **Solo se colorea el residuo** (lo que la curva añade):
  - se le quita la continua (paso alto de 5 Hz), así que la señal limpia no pasa por ningún filtro de fase;
  - con WARM y BODY se suavizan sus agudos: paso bajo de 18 kHz en TRANSPARENT, 7 kHz en WARM y 5 kHz en BODY.
- **CHARACTER** es continuo:
  - TRANSPARENT: `b` = 0,10, menos excitación;
  - WARM: `b` = 0,30;
  - BODY: además, un estante de +4 dB por debajo de 160 Hz antes de la curva, deshecho exactamente después. Los graves empujan más la válvula y la respuesta en frecuencia queda plana.
- **Compensación de nivel:** se iguala la fundamental de un tono a −6 dB del vúmetro en todos los ajustes. Diferencia entre CHARACTER inferior a 0,1 dB a ese nivel.
- **AMOUNT:** `d = 0,6 · AMOUNT^1,5 · (0,85…1,05 según CHARACTER)`.
- Ajustes suavizados (~20 ms).
- **ON/BYPASS:** fundido de 30 ms dentro del dominio sobremuestreado. Apagada, la señal pasa por los mismos filtros y el retardo no cambia.

**OUTPUT:**

- Ganancia lineal (de −24 a +12 dB, 0 dB = sin cambio), en rampa por bloque.
- Va después de la válvula, a la frecuencia del DAW: no puede alterar la saturación.

**Limitador True Peak** (`TruePeakLimiter`):

- **Detección:** 8 puntos entre muestras, interpolador sinc con ventana Kaiser de 64 coeficientes por fase (SIMD). Estéreo enlazado (la imagen no se mueve).
- **Ganancia:** mínimo sobre 1,5 ms y media móvil de 1,5 ms. Es una rampa suave que ya ha bajado cuando llega el pico: nunca recorta.
- **Release:** 60 ms tras un golpe aislado, hasta 500 ms si la reducción se mantiene (antibombeo).
- Margen interno de 0,05 dB.
- Por debajo del techo la ganancia es exactamente 1: sale idéntico bit a bit, solo retrasado.
- **Techo:** de −12 a −0,1 dBTP, por defecto −1 dBTP.
- Su retardo existe siempre, encendido o apagado, así que el PDC no cambia al pulsar.

**Parámetros nuevos** (al final de la lista, antes de las ranuras de FX):

- `mstValveOn` (apagada), `mstAmount` (35 %), `mstCharacter` (50 = Warm), `mstOutput` (0 dB), `mstLimiterOn` (encendido) y `mstCeiling` (−1 dBTP).
- En el DAW aparecen como «Master …».
- Un proyecto antiguo, que no los tiene, abre con estos valores.

**Interfaz:** panel **MASTER** en la pestaña FX, a la derecha del editor de ranura, color plata (familia SALIDA).

- **Válvula dibujada:** clic para encender o apagar. Su filamento ámbar brilla según el residuo real que añade la válvula (de −66 a −18 dB). Fría si está apagada o en silencio.
- Telemetría por atómicos: el audio solo escribe y la interfaz lee a 30 Hz, con calentamiento ~60 ms y enfriamiento ~400 ms.
- Mandos Amount, Character (con texto «Transp./Warm/Body»), Output y Techo, y botones Válvula y Limitador.
- **Medidores:** ENTRADA, SALIDA en dBTP con la línea del techo, REDUCCIÓN y CLIP (por encima de 0 dBTP; clic para apagarlo).
- Captura: `research/master_2026-11-24/ui_new2/5_fx_master_activo.png`.

## 6. Curvas y medidas armónicas

Tono de 1 kHz con el vúmetro a 0 dB (`harm_final.txt`):

| AMOUNT | TRANSPARENT (THD / H2 / H3) | WARM | BODY |
|---|---|---|---|
| 10 % | −62 / −62 / −93 | −51 / −51 / −93 | −52 / −52 / −92 |
| 25 % | −50 / −50 / −69 | −39 / −39 / −70 | −41 / −41 / −69 |
| **35 %** (por defecto) | −46 / −46 / −61 | **−35 / −35 / −61** | −36 / −36 / −60 |
| 50 % | −41 / −41 / −51 | −30 / −30 / −52 | −32 / −32 / −51 |
| 75 % | −35 / −36 / −41 | −25 / −26 / −41 | −27 / −27 / −40 |
| 100 % | −30 / −33 / −34 | −22 / −22 / −34 | −23 / −24 / −33 |

- **BODY a 100 Hz** (35 % / 100 %): −30 / −18 dB, frente a −35 / −22 en Warm. Su diferencia está en los graves.
- **Con el nivel** (35 %, Warm): −47, −41, −35, −32, −29, −26 y −24 dB de THD con el vúmetro a −12, −6, 0, +3, +6, +9 y +12 dB.
  - Es ~1 dB de THD por dB, sin salto.
  - Antes eran 140 dB de cambio en 6 dB.
- **Aliasing** (`alias.py`; tonos de 3,1, 7,3 y 12,3 kHz al 100 %, vúmetro de 0 a +6):

| Frecuencia | 2× | 4× | 8× |
|---|---|---|---|
| 48 kHz, tono de 12,3 kHz, +6 dB | −70 dB | −114 dB | −115 dB |
| 44,1 kHz, tono de 12,3 kHz, +6 dB | −59 dB | −68 dB | −69 dB |

- El caso de 44,1 kHz es el filtro de bajada de JUCE (el armónico 2 cae en su banda de transición), no la curva: 8× no lo mejora.
- Los tonos de 3 y 7 kHz están por debajo de −110 dB en todos los factores.
- **Decisión: 4× (2× a 88,2/96).** Usar 8× costaría un 30–45 % más de CPU para la etapa y no mejoraría nada.

## 7. Comparativas (barrido sobre los 77 renders)

Distorsión media en dB bajo la señal, sin contar la bajada lenta de ganancia del limitador. Entre paréntesis, el peor caso.

| Vúmetro | Antes | Ahora, válvula apagada | Ahora, válvula 35 % Warm |
|---|---|---|---|
| 0 dB | −140 | −140 (idéntico) | −40 (armónicos buscados) |
| +3 dB | −50 (−37) | −77 (−57) | −37 (−31) |
| +6 dB | −32 (−20) | −44 (−33) | −34 (−28) |
| +12 dB | −17,5 (−10) | −37 (−30) | −29 (−22) |
| Pico real máx. | +0,04 dBTP | −1,02 dBTP | −1,02 dBTP |

- **OUTPUT de −18 a +12 dB** (válvula 75 %): tras igualar nivel, la diferencia máxima es 1,2·10⁻⁷ (redondeo de coma flotante). La válvula hace exactamente lo mismo.
- **Válvula apagada y bajo el techo:** lo mismo que la etapa vieja, solo más tarde.
  - Bit a bit con la etapa vieja en bloques de 512.
  - En bloques de 64, diferencia de 9·10⁻⁸ (−140 dB): redondeo interno de JUCE.

## 8. Acordes

- Acordes de 3, 4, 6, 8 y 10 notas: piano Iowa, piano de la biblioteca, guitarra acústica, violín, cello, flauta, trompa, saxo y pluck.
- Graves solo hasta 4 notas.
- **Válvula a 35 %:** la distorsión de los acordes densos queda en −37 dB (media) con el vúmetro a +3 dB.
- **Sin válvula:** el limitador no añade distorsión apreciable. Media de −77 dB con el vúmetro a +3 dB; con más nivel, solo la bajada de volumen.
- Escucha: carpeta `06_acordes`. Hay que juzgar el «barro» de oído: las medidas no lo sustituyen.

## 9. Limitador

| Prueba | Resultado |
|---|---|
| Tono de 1 kHz de 0 a +12 dB sobre el techo | THD tras el limitador −134 a −149 dB (estable, sin distorsión). |
| Pad de 220 Hz con bombo 12 dB sobre el techo | Ningún salto propio: los que detecta son los del propio bombo, también sin limitar. |
| Bombeo: el pad baja con el bombo a +3 / +6 / +12 dB | 1,2–1,6 / 2,1–2,8 / 3,1–4,8 dB. Sube y baja como mucho 1,7 dB entre golpes. |
| Encender o apagar la válvula cada 250 ms y automatizar AMOUNT, CHARACTER y OUTPUT a la vez | 0 clics, a las 4 frecuencias. |

## 10. Techo y picos entre muestras

Pico real medido de forma estricta (16 puntos, sinc de 128 coeficientes), techo −1 dBTP:

| Señal (de 0 a +20 dB sobre el techo) | Pico real máximo |
|---|---|
| Tono a fs/4 a 45° (sus muestras 3 dB bajo el pico) | −1,00 dBTP |
| Golpes con ruido hasta 0,9 de Nyquist | −1,00 / −1,05 dBTP |
| Seno de 19,5 kHz, onda cuadrada limitada, impulsos | ≤ −0,99 dBTP |
| Samples reales (clap, bombo, loop, bajo, pad) por el procesador, vúmetro hasta +12 dB | −1,05 dBTP |
| Los 77 renders de acordes, vúmetro hasta +12 dB | −1,02 dBTP |
| **Ruido blanco hasta Nyquist** (caso extremo sintético) | **−0,6 dBTP** (límite, punto 12) |

## 11. CPU y latencia

**CPU** de la etapa sola, % de un núcleo (`cpu_stage.txt`, ruido estéreo, bloques de 64 y 128):

| | 44,1 kHz | 48 kHz | 88,2 kHz | 96 kHz |
|---|---|---|---|---|
| Etapa anterior | 0,59 | 0,64 | 0,64 | 0,69 |
| Ahora, válvula apagada | 1,03 | 1,10 | 1,51 | 1,65 |
| Ahora, válvula 60 % | 1,6 | 1,7 | 2,1 | 2,3 |
| (8×, como referencia) | 2,4 | 2,7 | 4,6 | 5,1 |

- **Latencia declarada** al DAW, medida con impulso (es la real), válvula encendida o apagada:

| | 44,1 kHz | 48 kHz | 88,2 kHz | 96 kHz |
|---|---|---|---|---|
| Muestras | 136 | 142 | 195 | 207 |
| Milisegundos | 3,08 | 2,96 | 2,21 | 2,16 |
| Antes | 38 (0,86 ms) | 38 (0,79 ms) | 31 (0,35 ms) | 31 (0,32 ms) |

- ReanimaRtCheck: 0 reservas de memoria en el hilo de audio.

**Baterías:**

| Batería | Resultado |
|---|---|
| ReanimaAudit completo | **550 comprobaciones, 0 fallos** (antes 516/0; 34 nuevas del MASTER). |
| ReanimaTests | Todas pasan. |
| ReanimaSnapshot | UI OK (incluye el filamento y los medidores). |
| ReanimaHostCheck (VST3) | OK. |
| Presets de fábrica (206) | OK. |
| ReanimaRtCheck | OK. |

**Tres pruebas cambiadas a propósito:**

- Las de la etapa vieja («la saturación crece de 0 a +6 dB») se sustituyen por las del diseño nuevo.
- La latencia esperada en HostCheck pasa de 38 a 142 muestras.
- «Salida a +12 dB»: ahora exige el techo de −1 dBTP en vez de 0 dBFS.

**Prueba nula de motores** (CpuBench, a la latencia nueva):

- Idénticos bit a bit: motor E, motor F, cello con filtro MS2 y cello con cadena de FX.
- Diferencias solo en los plucks que pasaban de −4 dBFS (ver punto 12).

## 12. Limitaciones y decisiones pendientes

1. **Proyectos que ya pasaban del 0 dB del vúmetro.**
   - Por debajo, todo suena igual que antes (bit a bit).
   - Por encima, antes había saturación suave entre 0 y +3 dB y recorte después. Ahora sale limpio hasta −1 dBTP y el limitador baja lo que sobra.
   - Resultado: más limpio y hasta ~2,6 dB más de pico. Le pasa al pluck de demostración a −6 dB cuando toca 4 voces.
   - Era precisamente el problema, pero es un cambio de carácter.
   - **Decidido por el usuario (24-11-2026): dejarlos limpios.** No hay modo compatible; los proyectos anteriores usan la etapa nueva.
2. **Volumen sigue antes de la válvula:** con la válvula encendida, subir Volumen la satura más, como en una mesa analógica. OUTPUT, en cambio, no la toca.
   - Alternativa: que la válvula ignore Volumen, midiendo siempre a su propio nivel. Cambiaría el sentido del vúmetro.
3. **Ruido blanco hasta la misma frecuencia de Nyquist:** el pico entre muestras puede quedar 0,4 dB por encima del techo (−0,6 dBTP, nunca cerca de 0 dBTP).
   - Ningún interpolador finito lee bien lo que está pegado a Nyquist.
   - El material real (hasta 0,9 de Nyquist, 20 kHz a 44,1) cumple el techo.
4. **Latencia:** +2,1 ms a 44,1/48 kHz. El DAW la compensa en reproducción, pero se nota algo más al tocar en directo con monitorización.
5. Válvula y limitador **sin juzgar de oído**. CHARACTER distingue Warm de Body sobre todo en los graves: con material agudo se parecen.
6. **CPU:** la etapa pasa de 0,6 % a 1,0–2,3 % de un núcleo.
7. **Sin probar en macOS** (no hay código específico de plataforma, pero no está compilado allí).
8. Los presets de fábrica cargan la válvula apagada: no guardan ajustes del MASTER.

---

## Añadido: válvula más presente (24-11-2026, a petición del usuario)

Pidió que la válvula se notara más y que al 100 % quedara «al límite de distorsionar sin llegar».

**Cambios:**
- AMOUNT al 100 %: el empuje de la curva pasa de 0,6 a 1,0. Sube 1,7 veces en todo el recorrido; el 35 % de ahora equivale al ~60 % de antes.
- BODY: el estante de graves pasa de +4 a +2,5 dB y desaparece su empuje extra. Así los graves al 100 % tampoco cruzan el borde.

**Tono con el vúmetro a 0 dB** (THD / 3.er armónico):

| Ajuste | Antes | Ahora |
|---|---|---|
| 1 kHz, Warm 35 % | −35 / −61 dB | −31 / −52 dB |
| 1 kHz, Warm 100 % | −22 / −34 dB | **−19 / −25 dB** |
| 100 Hz, Body 100 % | −17 dB (con el +4 dB) | −16 / −19 dB |

- Los picos a 0 dB del vúmetro se redondean unos 1,5 dB al 100 % (Warm).
- Con Volumen muy alto (+6 dB del vúmetro) y la válvula al 100 % ya hay distorsión (THD −14 dB): es lo esperable en una válvula empujada.

**Pruebas:** MASTER 47/0, ReanimaTests, interfaz (UI OK) y presets 4/0. La auditoría completa no se ha repetido: el cambio solo toca dos constantes de la curva.

**Escuchas:** `release/escucha_master_2026-11-24/07_valvula_mas_presente`.

**Estado:** INSTALADO el 24-11-2026 (sha 395eeba2; copia previa en `backups/instalado_antes_valvula_mas_2026-11-24`). Sin commit.
