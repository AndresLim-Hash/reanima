# Mecanismo: investigación, rediseño y resultados (13-11-2026, Reanima 0.9.5 alpha)

Informe del proyecto "Investigación, rediseño y optimización profesional del sistema de articulaciones e impactos
instrumentales". Cada punto dice si está **implementado**, **propuesto** o **pendiente de validar**. Las cifras salen
de mediciones con material real; los scripts que las reproducen están en `research/referencias_2026-11-13/scripts/`.

---

## Resumen

1. **El golpe llegaba tarde, siempre.** En instrumentos reales, la excitación (macillo, púa, mordida del arco,
   lengua) llega antes de que el tono crezca: 6-9 ms antes del primer pico en golpes y 35-45 ms en arco y soplo.
   Mecanismo la ponía en el primer pico del sample. **Implementado:** el golpe cae donde el ataque del sample alcanza
   una décima de su altura (+2 ms); Tecla, que en el órgano real llega tarde, sigue en el primer pico. Desfase medido:
   Piano 6 → 0 ms, Púa 8 → -1, Arco 18 → 0, Soplo 19 → -3,5, Tecla 0 (sin cambio).
2. **Fuentes reales nuevas, como opciones y no como sustitutos** (**implementado**): Piano real, Pulsado real, Púa
   guitarra, Arco real y Soplo real, con ataques sacados de notas reales (Iowa, VSCO, Karoryfer). Cada una mejora
   claramente a su versión clásica en su familia (tabla de la sección 5); fuera de ella, no. El usuario prefirió de
   oído Piano real para teclados reales y Piano clásico para sintes.
3. **Ninguna capa de otra grabación reproduce el ataque real**, ni la nota vecina del mismo piano perfectamente
   alineada, ni un banco de resonadores con los parciales del sample. El ataque auténtico es coherente con su propia
   nota (cómo arrancan sus parciales). Consecuencia de diseño: Mecanismo es una capa mecánica sutil (el ruido del
   gesto), no un sustituto del ataque. Comprobado, no es una hipótesis.
4. **Integración sonora corregida** (**implementado**): fase de "Del sample" (restaba en 5 de 33 samples, ahora suma
   en fase), picos de los golpes acotados a -6 dB del pico del sample (máximo de True Peak +6,35 → +4,18 dB), y en
   Arco y Soplo real el roce de la grabación ya no sigue sonando a otro nivel ni se corta de golpe.
5. **Recomendación por el nombre del archivo** (**implementado**) y botón **Oír todo / nota / golpe** (**implementado**).
6. **Pendiente de validar:** la escucha del usuario en Ableton de Pulsado, Arco, Soplo real y Púa guitarra; la
   batería completa de pruebas (EngineTests, Audit, validadores VST3, ASan, sesión larga) de toda la 0.9.5.

---

## 1. Investigación externa y fuentes consultadas

**Verificado con fuentes primarias o técnicas:**

- **Síntesis conmutada** (excitación × cuerpo, el cuerpo plegado en la excitación): J. O. Smith, CCRMA,
  [Commuted Synthesis](https://ccrma.stanford.edu/~jos/pasp05/Commuted_Synthesis_Strings.html),
  [DSPRelated](https://www.dsprelated.com/freebooks/pasp/Commuted_Synthesis.html). Límite conocido: excitaciones no
  lineales (arco fuerte).
- **Síntesis modal a partir de grabaciones:** Sirdey, Derrien, Kronland-Martinet, Aramaki,
  [ESPRIT en transformadas de Gabor](https://hal.archives-ouvertes.fr/hal-00881725); SAMPLE
  ([Zenodo](https://zenodo.org/records/3898795)); Van den Doel y Pai (modelo de contacto + banco de resonadores).
- **Separación armónico/percusivo/residuo:** Fitzgerald 2010
  ([arrow.dit.ie](https://arrow.dit.ie/argart/9)); Driedger, Müller y Disch,
  [ISMIR 2014](https://audiolabs-erlangen.com/resources/2014-ISMIR-ExtHPSep/2014_DriedgerMuellerDisch_ExtensionsHPSeparation_ISMIR.pdf)
  (factor de separación β, clase residual); [FMP, AudioLabs](https://audiolabs-erlangen.de/resources/MIR/FMP/C8/C8S1_HRPS.html).
- **Fabricantes:** Spectrasonics Keyscape, controles de ruidos mecánicos y de soltar
  ([manual](https://support.spectrasonics.net/manual/Keyscape/11/en/topic/performance),
  [ruido](https://support.spectrasonics.net/manual/Keyscape/11/en/topic/noise)); Native Instruments Kontakt, grupos
  de release y round robin ([manual](https://native-instruments.com/ni-tech-manuals/kontakt-manual/en/the-source-module));
  AAS Chromaphone, mazo + ruido + resonadores emparejados ([manual](https://applied-acoustics.com/chromaphone-3/manual));
  Modartt Pianoteq, ruido de macillo y de soltar ([características](https://www.modartt.com/pianoteq_features));
  Ample Sound, ruidos de púa, soltar y traste ([manual](https://www.amplesound.net/en/Ample_Guitar_Manual.pdf):
  la cuerda tarda unos 50 ms en vibrar del todo tras la púa).
- **Capas y reemplazo de transitorios:** Slate Trigger, alineación "con el grueso del transitorio" y polaridad
  ([Sound On Sound](https://www.soundonsound.com/reviews/steven-slate-trigger)); guías de capas
  ([Nail The Mix](https://www.nailthemix.com/?p=9840)).
- **IA:** DDSP en tiempo real ([arXiv 2103.07220](https://arxiv.org/pdf/2103.07220)): funciona, pero los autores
  señalan problemas de calidad en la transferencia de timbre y de distribución del plugin.

**Hipótesis o propuestas no verificadas:** que un modelado físico completo (tipo Chromaphone o Elements) superaría
a las grabaciones para ruidos mecánicos; los detalles internos de Keyscape y Ample (no publicados).

## 2. Tecnologías, modelos y algoritmos

| Técnica | Qué resolvería | Probado aquí | Resultado | Decisión |
|---|---|---|---|---|
| Alineación por inicio de subida | golpe tarde | sí, 47 notas reales | desfase de 6-19 ms a 0-3,5 ms | **implementado** |
| HPSS con margen β | sacar el golpe de una nota real | sí | β = 2 quita el resto tonal sin vaciar el golpe | **implementado** (generación de golpes) |
| Golpe de nota vecina como capa | ataque real | sí (límite teórico) | no acerca a la real (piano +1,0 dB, peor) | descartado |
| Resonadores con los parciales del sample (síntesis conmutada / modal) | ataque coherente con la nota | sí, prototipo corregido | 0 a +0,5 dB (no mejora) | descartado |
| Filtro sin desfase (adelante y atrás) | cancelación de "Del sample" | sí | de -1,3 dB a +0,6 dB (suma en fase) | **implementado** |
| Clasificador sinte / instrumento real por sonido | recomendar fuente | sí | datos sesgados (aprende grave/agudo) | descartado; se usa el nombre |
| DDSP / redes neuronales | timbre | no | coste y dependencias sin ventaja demostrada | no adoptado |

Coste en tiempo real de lo implementado: ninguno nuevo en el hilo de audio salvo la copia sin desfase de "Del
sample" (calculada al cargar: 2 s del sample, unos 0,4 MB por sample) y la búsqueda de su pico al tocar (unos 4000
valores). Latencia: 0. Tamaño del plugin: 13,0 → 23,4 MB por los 323 golpes nuevos.

## 3. Librerías y fuentes sonoras encontradas

| Librería | Licencia | Uso | Estado |
|---|---|---|---|
| University of Iowa MIS (piano pp/mf/ff, violín, violonchelo, flauta, clarinete, trompa) | "sin restricciones" (su web) | Piano real, Arco real, Soplo real | **incorporado** |
| VSCO 2 CE (piano vertical, arpa, pizzicato, violín, vientos, órgano) | CC0 | Piano/Pulsado/Arco/Soplo real; banco | **incorporado** |
| Karoryfer Shinyguitar, Emilyguitar, Black and Green Guitars | CC0 | Púa guitarra | **incorporado** (152 MB bajados) |
| Karoryfer Black and Blue Basses (bajo con púa y con dedos) | CC0 | candidato a Púa de bajo | propuesto, no bajado (961 MB) |
| Salamander Grand Piano v3 | CC BY 3.0 | otro piano real | propuesto |
| jRhodes3d, GregSullivan E-Pianos (Rhodes, CP80, Pianet) | sin licencia declarada en GitHub | Rhodes real | no usar sin aclarar la licencia |
| Good-sounds (UPF) | CC BY-NC 4.0 en Zenodo | — | no apto (no comercial) |
| Freesound: púas aisladas | no se encontró nada CC0 fiable | — | descartado |

Licencias y procedencia de lo incorporado: `Resources/Mechanism/CREDITOS.txt`; todas las descargas, con su URL y
SHA-256, en `samples_referencia/descargas.csv`.

## 4. Las articulaciones actuales (auditoría)

| Tipo | Fuente | Función | Limitación medida |
|---|---|---|---|
| Piano | VCSL Steinway, ruidos de apagador; pedal del piano vertical | tecla y apagador | no es un golpe de macillo: forma 0 frente al real, -13 dB |
| Púa | Growlybass (bajo eléctrico con púa) | púa de bajo | color tras el golpe 16,5 dB del real; paso alto fuerte |
| Arco | VSCO violín y chelo spiccato | mordida y roce | llegaba 18 ms tarde |
| Tecla | VSCO órgano (contacto) y VCSL clave | tecla | bien alineada; forma 0,17 |
| Soplo | VSCO flauta y clarinete, VCSL saxo | lengua y aire | 9-12 dB más fuerte que el aire real a 0,6-0,7 |
| Del sample | el propio ataque, paso alto 1 kHz | brillo del ataque | restaba en fase (corregido) |

## 5. Comparación y alternativas que han demostrado mejores resultados

Banco de pruebas (`mechbench.py`): a cada nota real se le separa el golpe (HPSS); Reanima recibe la nota sin golpe y
lo que añade Mecanismo se compara con el golpe verdadero. La librería de prueba excluye siempre las notas medidas.

| Familia | Clásico (color tras el golpe, forma) | Real (color, forma) | Veredicto |
|---|---|---|---|
| Piano (18 notas) | 47,6 dB, 0,41 | **10,2 dB, 0,51** | mejor; confirmado de oído para teclados reales |
| Guitarra (10 notas) | 16,5 dB, 0,47 | **10,1 dB, 0,75** (Púa guitarra) | mejor |
| Arpa y pizzicato (9) | 16,6 dB, 0,34 | **12,5 dB, 0,59** (Pulsado real) | mejor |
| Arco (8) | 14,9 dB, 0,49 | 10,4 dB, 0,31 | mejor color, peor forma: a juzgar de oído |
| Soplo (12) | 14,6 dB; +9,5 dB sobre el aire real | 13,4 dB; +4,6 dB | algo mejor |

Mezclar fuentes de familias distintas en un mismo tipo empeora las dos (guitarra + arpa: +0,77 dB de realismo en
arpa frente a +0,37), por eso cada familia tiene su tipo.

## 6. Cambios en la arquitectura (implementados)

- `Mechanism::Type` con cinco tipos "real" al final del menú (los proyectos guardados no cambian). Se comportan como
  su gesto (nivel, soltar, pedal, roce continuo); solo cambian los golpes de pulsar. Púa guitarra y Pulsado real
  usan el tratamiento ligero del piano; Arco y Soplo real tocan solo la mordida o la lengua (120 ms) y el roce
  continuo sigue a la nota.
- Librería con conjuntos de reserva: lo que un tipo real no tiene (soltar, pedal) lo toma de su gesto clásico.
- `Excitation::riseAt` (inicio de la subida) y la nueva regla de alineación; fundido de entrada de arco y soplo de
  5 ms como mucho.
- Tope de pico de cada golpe; copia sin desfase para "Del sample" (`SampleData::attackHigh`).
- Recomendación: `SampleData::mechanismHint`, calculada al cargar a partir del nombre del archivo.
- `SynthEngine::listen` y el botón "Oír todo / nota / golpe" (no se guarda en el proyecto).

## 7. Fase, estéreo y ganancia

Auditoría de la capa (`mechmix.py`, 33 samples de 15 familias, mando a 0,7):

- **Fase:** ningún tipo con grabaciones resta energía en el ataque (interacción 0,0 dB); "Del sample" restaba hasta
  -1,3 dB y ahora suma (+0,6). Polaridad: la regla existente (misma polaridad que el golpe del sample cuando éste
  golpea) se mantiene; no se encontraron inversiones problemáticas.
- **Estéreo:** correlación de la capa de 0,33 a 1,00; al sumar a mono pierde como mucho 1,8 dB, sin cancelaciones.
  Púa y Púa guitarra son prácticamente mono (fuentes mono).
- **Picos:** mediana +0,0-0,2 dB; tras el tope, el máximo de True Peak sube +4,2 dB solo con el roce continuo de Arco
  en un sample con poco margen. No se limita ni comprime nada: el tope reduce la ganancia de ese golpe.
- **Niveles frente a la realidad** (no cambiados, a criterio del usuario): a 0,7, Soplo queda unos 12 dB sobre el
  aire real y Arco unos 4 dB bajo la mordida real; Piano, Púa y Tecla, a 1-3 dB de lo real.

## 8. Naturalidad e integración

Lo que más acerca el resultado a una única interpretación, medido: el golpe en su sitio (sección 1), golpes del
mismo tipo de instrumento que el sample (sección 5) y que el roce siga a la nota en vez de tener vida propia. Lo que
no lo consigue: capas de otras notas o resonadores genéricos (sección 2). Por eso Mecanismo se queda en lo que una
capa puede hacer bien: el ruido mecánico del gesto, en tiempo y en su familia.

## 9. Pruebas realizadas y resultados

| Prueba | Qué mide | Resultado |
|---|---|---|
| `mechbench.py` (57 notas reales, 5 familias) | desfase, color, nivel, forma, realismo | tablas de las secciones 1 y 5 |
| `oracle.py`, `oracle2.py` | límite teórico de capas y resonadores | ninguna alcanza al golpe propio |
| `mechmix.py` (33 samples, 11 tipos) | picos, estéreo, mono, fase con el sample | sección 7 |
| Escucha A/B del usuario (piano) | naturalidad | Piano real para teclados reales; Piano para sintes |
| Escucha separada (Batch `listen=`) | "Oír nota" idéntico a sin Mecanismo; mezcla = nota + golpe | exacto (-300 y -141 dB) |
| Comprobación de la interfaz (Snapshot) | controles y paneles | correcta |
| Prueba de tiempo real (RtCheck), sin los cambios de Mecanismo | reservas en el hilo de audio | 0 en 432 escenarios (antes de esta fase) |

## 10. Pendiente y limitaciones

- **Pendiente de validar:** escucha en Ableton de Pulsado real, Púa guitarra, Arco real y Soplo real; batería
  completa de la 0.9.5 (EngineTests, Audit, Steinberg, pluginval, ASan, RtCheck con los tipos nuevos, sesión larga).
- **Propuesto:** Púa de bajo real (Black and Blue Basses, CC0, 961 MB); Rhodes y Wurlitzer reales si se encuentra una
  librería con licencia clara (las de GitHub no la declaran); calibrar el nivel de Soplo y Arco si el usuario lo
  prefiere cercano al real; un segundo nivel de recomendación por el sonido cuando haya datos sin sesgo (por ejemplo,
  pares sinte/real de la misma familia).
- **Limitaciones:** el banco mide el golpe con HPSS, que deja parte del arranque de los parciales en el componente
  percusivo; la medida de "realismo" premia más ese arranque que el ruido mecánico. Por eso las decisiones de fuente
  se han tomado con varias medidas y con el oído del usuario, no con una sola cifra. macOS no se ha podido probar.
