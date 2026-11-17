# Blackpanda Reanima

Sampler VST3 que trata el sample como un sonido sintetizado: detecta su ataque, decay,
sustain y release, pone los mandos en esos valores y reconstruye el sample cuando los mueves.
Diseño y comparativa de funciones: [DESIGN.md](DESIGN.md).

## Compilar e instalar (Windows)

1. Doble clic en **build.bat** (la primera vez tarda unos minutos).
2. Doble clic en **install.bat** y acepta el aviso de administrador: copia `Reanima.vst3`
   a `C:\Program Files\Common Files\VST3\Blackpanda`.
3. En Ableton: *Preferencias → Plug-ins → Use VST3 Plug-in System Folders* activado y **Rescan**.
   Aparece en *Plug-ins → Blackpanda → Reanima*.

También hay una versión independiente para probar sin Ableton:
`build\Reanima_artefacts\Release\Standalone\Reanima.exe`.

**macOS (Apple Silicon e Intel)**: en un Mac con Xcode y CMake, `sh build_mac.sh` compila VST3 y AU en un solo
binario universal (macOS 11 o posterior) y los instala para el usuario. Todavía no se ha probado en un Mac.

## Uso rápido

- **Guía**: el botón **?** (junto a Deshacer) abre un recorrido de 12 pasos por la ventana real.
- **Deshacer / Rehacer** (fila de pestañas): mandos, menús, presets, samples cargados y ediciones del sample. Un
  mando girado a mano es un paso; la automatización del host no se graba.
- **Proyectos**: el sample va siempre dentro del proyecto (FLAC sin pérdida, preparado en segundo plano), así que un
  proyecto abre igual en cualquier ordenador aunque el archivo original no esté. Guardar tarda ~1 ms.

- **Cargar**: arrastra un sample desde el explorador o desde el navegador de Ableton a la forma de onda.
- **Presets**: 12 de fábrica, que son plantillas: cambian motor, envolvente, timbre y efectos y conservan tu sample.
  Los tuyos se guardan con el sample, los mandos y los efectos en Documentos/Blackpanda/Reanima/Presets.
  ◀ y ▶ pasan al anterior y al siguiente.
- **Motor**:
  - *A · Reconstrucción* (por defecto): el sample suena tal cual mientras puede dar lo que pide tu ADSR. Si le pides
    más (más sustain, un decay más largo, un ataque lento), entra una continuación del sonido generada al cargarlo,
    en estéreo, alineada en fase con el original y sin bucles audibles.
  - *B · Reenvolvente*: todo el sample se remodela con granos sincronizados con el tono. Pads y tonos.
  - *C · Resíntesis*: el sonido se rehace como armónicos + ruido + el golpe original. Cada armónico cae por su
    cuenta, como en un instrumento real, y la nota dura lo que quieras.
  - *D · Híbrido*: el golpe inicial queda intacto y el cuerpo se remodela con granos. Pianos, bajos, baterías.
  - *E · Granular*: una nube de granos del sample, con Posición, Grano, Densidad, Spray, Deriva, Pitch aleatorio,
    Al revés y Ancho. Suena mientras mantienes la tecla y la ADSR le da forma.
  - *F · Drone Maker*: estira el sample hasta mil veces con tramos FFT largos y fases al azar (la idea de PaulStretch):
    no queda ritmo ni ataque, sólo el color. Panel DRONE MAKER: Armónicos (sólo los de la nota), Tono/ruido, Spread,
    copias a -2, -1, quinta, +1 y +2 octavas, Desplazar en Hz y un filtro que se dibuja con el ratón; en MOTOR,
    Posición, Estirar, Estéreo, Tamaño FFT y Congelar.
- **Warp** (sección MOTOR, todos los motores): cómo cambia el tono lejos de la nota del sample, como en Simpler.
  *Re-Pitch* (por defecto) es el sampler clásico: más agudo, más rápido y más corto. *Tonos* cambia el tono sin
  cambiar la duración y deja el timbre en su sitio (formantes): notas sueltas, voces, vientos, bajos, teclas.
  *Textura* tampoco cambia la duración, pero el color sube y baja con la nota: pads, acordes, ruidos. El ataque se
  toca siempre entero. Sin latencia y sin memoria extra. Al cargar, Reanima prueba Tonos con el sample: si daría
  la nota equivocada (acordes, voces desafinadas, ciclos irregulares), en A, B y D toca Textura y el menú dice
  «Tonos → Textura».
- **Textura** (motores A a D): mezcla una nube de granos del propio sample en el sonido, sin cambiar de motor.
- **Sustain** (sección MOTOR): *Natural (pedal)* deja sonar el original y lo prolonga cayendo como con pedal;
  *Sostenido* mantiene la nota viva mientras pulsas, con **Respiración** (pequeñas variaciones de nivel y afinación).
- **Modo sustain** (B y D): *Loop*, *Granular* o *Freeze*.
- **ENVOLVENTE** (la forma de onda dibuja el resultado de cada modo):
  - *Remodelar el sample*: tu ADSR cambia la forma del sample y conserva sus altibajos. Con los valores detectados
    suena igual que el original.
  - *Sintetizador* (motores A a D): el sample se vuelve un tono continuo y plano y la ADSR lo modela desde cero, como
    en un Minimoog o un Prophet. Al elegirlo, los mandos pasan a A 5 ms, D 600 ms, S 60 %, R 350 ms.
  - **Golpe**: si pones un ataque más rápido que el del sample, crea el golpe que le falta con sus propios agudos.
  - **Candado**: cerrado, cargar otro sample no cambia tu ADSR. **Detectado** vuelve a los valores del sample.
- **TIMBRE**: *Fuerza → brillo* (tocar suave oscurece), *Arco/aliento* (un ataque lento empieza oscuro), *Apagado*
  (los agudos se apagan antes que los graves) y *Mecanismo*: lo que suena al pulsar y al soltar la tecla, del
  instrumento que elijas junto al título (Piano, Púa, Arco, Tecla, Soplo o el propio ataque del sample), con un nivel
  relativo al del sample. Piano, Púa, Arco, Tecla y Soplo son grabaciones reales (461 golpes, CC0: ver
  `Resources/Mechanism/CREDITOS.txt`): cada nota toma la grabación hecha en su misma nota (en cualquier octava), movida como mucho semitono y medio; con Piano suena también el pedal derecho (CC64).
  El menú dice cuánto encaja cada gesto con el sample y con tu ataque (» el recomendado) y la línea de abajo avisa
  cuando el elegido encaja poco y por qué; **On/Off** lo apaga para comparar. **Entrada** y **Salida** son el volumen
  del golpe al pulsar y al soltar (dB sobre Mecanismo). **Adaptar** funde el gesto
  con el sample: su golpe cae en el del sample y con su polaridad, toma el color del ataque, no baja de la nota y
  resuena en ella; Arco y Soplo siguen sonando, bajo la nota, mientras ésta suena.
- **EXPRESIÓN MIDI** (pestaña Filtro · Mod): aftertouch y soplo (CC2) abren el sonido, lo suben y añaden vibrato; la
  rueda de modulación hace vibrato; *Bend* fija el rango de la rueda de pitch; CC11 actúa como pedal de expresión.
- **VOZ**: *Voces* (Unison, de 1 a 16) y *Detune* en %, con Blend y Ancho, como en Serum.
- **Snap a C**: apagado, Do3 toca el sample tal cual. Encendido, el sample se transporta al Do más cercano a su
  nota (un La2 sube a Do3) y todo el teclado queda afinado.
- **Forma de onda**: la rueda hace zoom alrededor del ratón; con Mayús o arrastrando el fondo te desplazas. La barra
  INICIO tiene una pestaña para agarrarla, sombrea lo que se salta y se pega al inicio detectado (Mayús: ajuste fino;
  doble clic: vuelve al detectado).
- **EDICIÓN**: edición del propio sample sin salir del plugin. Arrastra para seleccionar y un clic marca dónde se
  pega: Recortar, Borrar, Silencio, Copiar, Cortar, Pegar (también entre samples), Invertir, Fundidos, Normalizar,
  Ganancia, Bucle sin costura, Deshacer y Rehacer (Ctrl+C, X, V, Z, Y y Supr). *Cruces por cero* ajusta los cortes
  para que no hagan clic. El archivo original no se toca: el audio editado se guarda dentro del proyecto. Debajo:
  Fin, fundidos, Ganancia, Normalizar, bucle con fundido cruzado (motor A) y envolvente de pitch (caídas tipo 808).
- **Salida**: vúmetro estéreo, Mono, Pan, Volumen y Exportar, siempre visibles. Debajo, el **tamaño de la ventana**
  (60 a 200 %, o ajustada a la pantalla); también se arrastra la esquina. Se recuerda. La primera vez se ajusta a la
  pantalla: con un monitor 4K al 300 % abre al 75 %.
- **Mandos**: doble clic devuelve un mando a su valor por defecto, salvo los que salen del sample (ADSR e Inicio).
  Un mando atenuado no hace nada con los ajustes actuales. Medio segundo con el ratón encima explica qué hace.
- **FX** (estilo Serum 2): cadena *Main* + *Bus 1* y *Bus 2* en paralelo (con *Envío* y *→ Main*). 12 efectos y
  3 splitters; arrastra las ranuras desde ⋮⋮ para reordenarlas o moverlas entre cadenas.
- **Exportar**: arrastra el sonido actual (una nota en Do3, con efectos) a una pista de Ableton o al explorador.

Notas en nomenclatura de Ableton (C3 = nota MIDI 60).

## Rendimiento

- Latencia: cero. La primera muestra sale en el instante exacto de la nota, en cualquier tamaño de bloque.
- Frecuencias de muestreo: cualquiera; se verifica a 44,1, 48, 88,2 y 96 kHz, y el VST3 también a 176,4 y 192 kHz.
- Cargar un sample tarda menos de medio segundo y no bloquea la interfaz. Los modelos de los motores B, C y D se
  calculan sólo al cambiar a ellos.
- Motor F: cada tramo se prepara por etapas repartidas entre bloques, la nota más urgente primero, así que una FFT
  larga nunca cae entera en un bloque (8 notas a 1,4 s: el peor bloque usa un tercio de su tiempo). Hasta 16 notas
  de drone a la vez; sus búferes (unos 30 MB) se crean la primera vez que se elige el motor.
- El rack de efectos duerme tras 7 s de silencio.

## Pruebas

Se ejecutan desde `build\<nombre>_artefacts\Release\`:

- `ReanimaTests.exe`: análisis, motores y efectos con samples sintéticos.
- `ReanimaAudit.exe [fast]`: auditoría a las cuatro frecuencias. Ninguna reserva de memoria en el hilo de audio,
  latencia, fidelidad, transposición, aliasing, estéreo, envolventes, inicio, archivos raros, clics, cada efecto,
  cada parámetro y prueba de caos.
- `ReanimaSnapshot.exe <carpeta>`: capturas de cada pestaña y pruebas con procesador y editor reales: ratón sobre la
  forma de onda, tooltips, guardar y abrir, carga en segundo plano.
- `ReanimaHostCheck.exe <Reanima.vst3>`: abre el VST3 compilado como lo hace un host, a 6 frecuencias y 11 tamaños
  de bloque, con 8 instancias en paralelo.
- `ReanimaBatch.exe <carpeta> <csv>`: pasa el motor A por una carpeta entera de samples.
- `ReanimaBatch.exe info <carpeta|lista.txt> <csv>`: lo que el análisis encuentra en cada archivo (formato, nivel, clip,
  DC, estéreo, ruido de fondo, nota, envolvente, brillo). Marca los ilegibles.
- `ReanimaBatch.exe motores <carpeta|lista.txt> <csv> [cada N]`: los 6 motores en cada archivo (nivel, picos, DC,
  estabilidad, clics al soltar, cola). `render <wav> <motor> <salida.wav> [id=valor…] [held=s]` guarda esa nota.
- `ReanimaHostCheck.exe <Reanima.vst3> resistencia <minutos> <lista.txt>`: sesión larga por la interfaz VST3 (samples,
  motores, mandos, notas, controladores, bloques y frecuencias cambiando, guardar y reabrir, ventana): valores no
  válidos, bloque más lento por tamaño, memoria por vuelta a la lista.
- `ReanimaRelease.exe <carpeta> <csv> [cada N]`: mide qué pasa al soltar la nota, con la envolvente cambiada de varias
  maneras, y si las notas repetidas suenan igual.
- `ReanimaAudit.exe mapa <wav>`: tabla de lo que cambia cada control del motor A en ataque, pico, cuerpo, cola, ancho y
  brillo, para ver efectos secundarios.
- `ReanimaDemos.exe <carpeta> <salida>`: renders para escuchar el motor A.

Las herramientas de la investigación de los lechos están en `research/`, fuera de la compilación.
