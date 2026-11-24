# Redimensionar la ventana de Reanima: auditoría y correcciones (24-11-2026)

Rama `beta-master-valvula`. Nada instalado y nada confirmado con commit.

## Resumen para el usuario

- **No he logrado reproducir el cuelgue.** Lo he intentado con el plugin instalado (sha 395eeba2) en Ableton Live 12, en este equipo. He probado la esquina de Reanima (lento, rápido y con saltos del 60 al 200 %), el menú «100 %», el borde de la ventana de Ableton (con ratón y con teclado), la ventana medio fuera de pantalla y la ventana en el TV a 300 %. En todos los casos los controles siguieron respondiendo.
- Sí he encontrado y corregido **cinco defectos reales del redimensionado**. El primero es el más visible: en Ableton, **el borde de la ventana solo dejaba hacerla más pequeña**. Si se intentaba agrandar, se quedaba igual o encogía. Hay que comprobar si esto era lo que parecía un cuelgue (apartado 9).
- El sonido no cambia. El render es idéntico bit a bit con y sin redimensionado, y redimensionar no toca ningún parámetro. Comparados: 392 parámetros dentro del plugin y 2473 a través de VST3.
- Para cerrar la causa necesito saber cómo ocurre exactamente: con qué se redimensiona, si se recupera, qué sonaba y en qué monitor estaba.

## 1. Causa exacta

**No confirmada**, porque no he podido reproducir el cuelgue. Los cinco defectos de abajo son seguros y cualquiera de ellos puede dejar la ventana «inservible» sin llegar a colgarla:

1. **El borde de la ventana del host no agrandaba.** El envoltorio VST3 de JUCE 9.0.4 resuelve la proporción fija así: cuando se arrastra un solo borde, recalcula ese mismo borde a partir del otro. Por eso el ancho o el alto nuevos se deshacían.
   - Comprobado en Ableton: al arrastrar el borde derecho a 1120 px, la ventana no creció. Al volver a 700 px, encogió, y ya no volvió a crecer.
   - JUCE solo tenía la regla correcta para Cubase 9.
2. **Recordar el tamaño provocaba un redimensionado más.** Medio segundo después de cada arrastre, el temporizador de la interfaz llamaba a `setScale(..., true)`, que llama a `setSize()` y obliga al host a redimensionar la ventana otra vez (`resizeView`) solo para guardar un número. Si el ratón seguía sobre el borde de Ableton, el plugin redimensionaba la ventana a la vez que el host.
3. **El tamaño guardado podía no caber en pantalla.** Un 200 % elegido en el menú se volvía a abrir al 200 % aunque la pantalla no tuviera sitio. La esquina y el botón de tamaño quedaban fuera, sin forma de volver atrás desde la propia ventana. Lo comprobé en Ableton: al 200 % la esquina queda fuera de la pantalla de 1920×1080.
4. **La escala solo miraba el ancho.** Si un host daba una ventana más baja de lo proporcional, la parte de abajo (SALIDA, el botón de tamaño y la esquina) quedaba cortada y no se podía pulsar.
5. **El arrastre no se recordaba si nunca se había usado el menú.** La condición era `savedScale > 0`, así que sin un tamaño elegido antes en el menú, el arrastre no se guardaba.

## 2. Archivos implicados

- `Source/PluginEditor.cpp` y `.h`: `resized`, `setScale`, el nuevo `rememberScale`, el nuevo `screenMaxScale`, `paint`, `timerCallback` y `showSizeMenu`.
- `external/JUCE/modules/juce_audio_plugin_client/juce_audio_plugin_client_VST3.cpp`: `checkSizeConstraint`. Es un parche local de 8 líneas, marcado con «Reanima:».
- Pruebas:
  - `tests/ResizeCheck.cpp` (nuevo, objetivo `ReanimaResizeCheck`, solo Windows);
  - `tests/HostCheck.cpp` (nueva sección «redimensionar desde el host»);
  - `CMakeLists.txt`.

Revisado y sin cambios:
- `Look.h`: `Section::resized`, `Knob`, `Theme::onScreen`.
- `Displays.cpp`, con el `MasterPanel` y sus medidores a 30 Hz.
- `FxPage`, `EditPage`, `WaveformView` (imagen en caché), `Guide` / `TabGuides` y `SampleBrowser`.

Redimensionar no reconstruye ningún componente: solo cambia la transformación de `content` y las fuentes de las etiquetas.

## 3. Por qué podían dejar de responder los controles

He revisado las vías por las que la ventana podría dejar de responder del todo, y ninguna tiene que ver con el redimensionado:

- No hay recursión entre `resized`, `setSize` y `setTransform`.
- No hay bloqueos compartidos entre interfaz y audio: el editor lee el procesador con atómicos y `takeMeterReading`.
- Redimensionar no abre nada modal.

Lo que sí ocurría: con el tamaño guardado mayor que la pantalla (defecto 3) o con la ventana cortada (defecto 4), los controles de abajo a la derecha, la esquina y el botón de tamaño quedaban inalcanzables.

Una sospecha aparte que no he podido confirmar: un aviso de carga (`AlertWindow`, que es modal) que quede detrás de la ventana flotante de Ableton bloquearía todos los clics. Eso no depende del redimensionado.

## 4. Correcciones

- `resized()` usa la escala `min(ancho/1180, alto/760)` y centra el diseño. Con cualquier forma que dé el host, todo queda dentro y nada se corta.
- Recordar el tamaño ya no redimensiona. Tras un arrastre, cuando la ventana lleva 0,5 s quieta, solo se escribe el número (`rememberScale`), una vez por elección y nunca en cada paso. El arrastre se recuerda también si nunca se usó el menú.
- Al abrir, el tamaño guardado se limita a lo que cabe en la pantalla (`screenMaxScale`). El valor guardado no se toca: en una pantalla más grande se abre a su tamaño.
- JUCE `checkSizeConstraint`: si el host cambia solo un lado, se conserva ese lado y el otro se ajusta a la proporción. Antes esto valía solo para Cubase 9; ahora vale para todos los hosts, con tolerancia de 1 px.
- `Theme::uiScale` (el mínimo de 8 px del texto) es global al proceso:
  - cada editor lo vuelve a fijar a su escala al pintar;
  - las fuentes de las etiquetas se recalculan comparando con la escala propia de cada editor (`fontScale`), no con la global.

  Antes, dos ventanas de Reanima a tamaños distintos se pisaban esa escala.

## 5. Cambios de arquitectura

- El redimensionado es solo presentación: un diseño fijo de 1180 × 760 dibujado con una transformación.
- No se reinicializan el motor, las voces, los parámetros, la automatización ni el MIDI learn, y no se envía nada al procesador.
- El tamaño tiene tres estados separados:
  - el pedido por el propio editor (`requestedScale`);
  - el que se muestra (`shownScale`);
  - el guardado en el equipo (`savedScale`).

  Solo los cambios que no vienen del editor (la esquina o el borde del host) se guardan, y solo al detenerse.

## 6. CPU y memoria, antes y después

Medido con `ReanimaResizeCheck` en este equipo. «Antes» es el código previo a las correcciones, con la misma herramienta.

| | antes | después |
|---|---|---|
| Paso de arrastre (evento + pintado), media | 0,7–0,8 ms | 0,6–0,8 ms |
| Paso de arrastre, peor | 2–16 ms | 2–16 ms |
| Pintado completo de una pestaña (software) | 8–17 ms al 100 %, 20–39 ms al 200 % | igual |
| Memoria en los primeros tamaños (cachés de glifos e imagen) | +32 a +47 MB, se estabiliza | +31 a +33 MB, se estabiliza |
| Memoria en 960 pasos más | — | −0,7 MB (sin fuga) |
| Objetos GDI/USER | 63 → 75, estable | 63 → 77, estable |
| Cerrar y abrir 20 veces | +2 a +8 MB, 112–142 ms | +6 MB, 113 ms |

El paso de arrastre por el camino VST3 real (`ReanimaHostCheck`) cuesta 12–21 ms en el peor caso.

## 7. Pruebas con reproducción y polifonía

`ReanimaResizeCheck`: 59 comprobaciones, 0 fallos.

- Pestañas: arrastre de la esquina en las 6, en lento (160 pasos) y en rápido (saltos del 60 al 200 %).
- Controles: después de cada tamaño (60 / 83 / 100 / 137 / 200 %) responden mandos de SAMPLE, FILTRO·MOD, MATRIZ, FX y ARP·SEQ. Responden también tras tamaños del host, incluso sin proporción, y con LOOP, SLICE, el tour, un tooltip y un menú abiertos.
- Con un hilo-host tocando acordes de 16 notas, 4 voces de unísono y automatización de corte y macro mientras se redimensiona sin parar:
  - el audio no se detiene: 202 bloques de los 183 esperados;
  - el bloque más lento tardó 6,8 ms (límite 10,67 ms);
  - la interfaz nunca se quedó más de 50 ms sin latido.

`ReanimaHostCheck` (VST3 real, con notas sonando):
- el borde derecho y el inferior agrandan la ventana manteniendo la proporción;
- saltos del mínimo al máximo y tamaños sin proporción, sin cuelgues;
- el tamaño se conserva al reabrir;
- deja el tamaño guardado del usuario como estaba.

## 8. Prueba de que no cambia ningún parámetro ni el estado sonoro

- Los 392 parámetros y el estado del proyecto quedan idénticos byte a byte tras 20 ciclos de pestaña, esquina y host, y tras cerrar y abrir 20 veces.
- A través de VST3, los 2473 parámetros quedan idénticos.
- Render de 600 bloques con acordes: diferencia 0 entre sin ventana y con la ventana redimensionándose entre bloques. Dos renders sin ventana también difieren en 0.

## 9. Problemas adicionales

- **Pendiente de escucha y prueba en Ableton**, porque no he instalado nada:
  - que el borde ya agranda la ventana;
  - que la ventana al 200 % abre dentro de la pantalla.
- Ableton (o la propia Claude) en este equipo guarda los ajustes en una carpeta virtualizada (`AppData\Local\Packages\Claude_…\LocalCache\Roaming`). Desde aquí no se ve el archivo real que escribe Ableton.
- Las suites existentes:
  - `ReanimaSnapshot`: «UI OK», 221 PASS;
  - `ReanimaHostCheck`: «VST3 OK»;
  - `ReanimaRtCheck`: 0 fallos;
  - `ReanimaTests` y `ReanimaAudit`: ver apartado 11.
- La escala por ventana (`Theme::uiScale` fijado al pintar) no cubre un caso: un repintado parcial de un control opaco con dos Reanima abiertos a tamaños distintos. JUCE puede saltarse el `paint` del editor en ese caso. Solo afecta al mínimo de 8 px del texto.

## 10. Limitaciones no verificadas

- **macOS y otros hosts** (Logic, Cubase, Reaper, Bitwig) no se han probado. El parche de JUCE aplica la regla de Cubase 9 a todos los hosts, y en macOS el comportamiento debería ser el mismo, pero no está comprobado.
- **Lo que contó el usuario.** Arrastró la esquina de Reanima en la pantalla Full HD. La ventana creció pero el contenido no se reescaló: quedó una zona negra. Al intentar reducirla, ya no pudo pulsar ningún control, y se quedó así para siempre.
- **Lo que he repetido con el plugin instalado**, en la misma pantalla:
  - agrandar con la esquina, con pausas de 1 s con el botón pulsado, hasta salirse de la pantalla por abajo y por la derecha (139 %);
  - reducir después con el borde izquierdo;
  - ventana sin proporción puesta por script (el contenido quedaba cortado abajo, que es el defecto 4).

  Resultado: hubo retraso durante el arrastre, pero ni zona negra persistente ni pérdida de clics.
- **Relación de las correcciones con su descripción:**
  - una zona oscura significa que la ventana del host es más grande que lo que se dibuja; ahora el diseño llena cualquier tamaño (defecto 4);
  - el plugin ya no redimensiona por su cuenta medio segundo después del arrastre (defecto 2);
  - los bordes del host mantienen la proporción (defecto 1).

  Si se repite con la versión nueva, conviene anotar dos cosas: si el resto de Ableton sigue respondiendo (eso distingue un hilo colgado de una ventana desincronizada), y en qué lado estaba la zona negra.
- **El cuelgue original no se ha reproducido.** Si sigue ocurriendo con esta versión, hace falta saber:
  1. si se redimensiona con la esquina de Reanima, con el borde de Ableton o con el botón «100 %»;
  2. si se recupera solo o hay que cerrar;
  3. si sonaba audio y en qué pestaña estaba;
  4. en qué monitor estaba la ventana (el TV a 300 %);
  5. si había otro Reanima abierto.
- La parte gráfica de JUCE 9 (Direct2D) pinta sincronizada con la pantalla, así que el tiempo de pintado real en pantalla no se mide por paso. Por eso medí aparte el pintado completo en software.

## 11. Baterías existentes

| Batería | Resultado |
|---|---|
| ReanimaAudit completo | 545 comprobaciones, 0 fallos. Las 5 que faltan respecto al 550/0 del 24-11 son «muestra real», que buscan samples a partir de la carpeta del proyecto, y se lanzó desde otra; no es una regresión. |
| ReanimaTests | 0 fallos. Una primera pasada dio 1 fallo de tiempo de CPU del motor F porque corría a la vez que el Audit; repetida sola, todo pasa. |
| ReanimaSnapshot | UI OK, 221 PASS. Capturas «escalas» al 60–100 % revisadas: igual que antes. |
| ReanimaHostCheck | VST3 OK, con la nueva sección de redimensionado. |
| ReanimaRtCheck | 0 fallos, 0 reservas de memoria en el hilo de audio. |
| ReanimaResizeCheck (nueva) | 59 comprobaciones, 0 fallos. |

Para reproducir:
- `build\ReanimaResizeCheck_artefacts\Release\ReanimaResizeCheck.exe <carpeta> [rapido]`
- `ReanimaHostCheck.exe build\Reanima_artefacts\Release\VST3\Reanima.vst3`
