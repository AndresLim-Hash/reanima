# Compilar Reanima en un Mac

Necesitas un Mac con macOS 11 o posterior. Sale un único plugin que funciona en Apple Silicon (M1, M2…) y en Intel.

1. **Instala Xcode** desde la App Store (gratis, unos 10 GB). Ábrelo una vez y acepta la licencia.
2. **Instala CMake**: descárgalo de https://cmake.org/download (archivo .dmg «macOS Universal»), arrastra CMake a Aplicaciones y, en Terminal, escribe una vez:
   `sudo "/Applications/CMake.app/Contents/bin/cmake-gui" --install`
   Si usas Homebrew, basta con `brew install cmake`.
3. **Descomprime** `Reanima_codigo_fuente_mac.zip` (por ejemplo en el Escritorio).
4. **Abre Terminal**, escribe `cd ` (con un espacio), arrastra la carpeta `Reanima` a la ventana y pulsa Intro.
5. **Compila e instala:**
   `sh build_mac.sh`
   La primera vez tarda entre 5 y 15 minutos.

Al terminar quedan instalados:
- **VST3**: `~/Library/Audio/Plug-Ins/VST3/Blackpanda/Reanima.vst3`
- **AU** (Logic, GarageBand y también Ableton): `~/Library/Audio/Plug-Ins/Components/Reanima.component`

Abre Ableton → Preferencias → Plug-ins → «Reescanear».

Si algo falla, copia lo último que aparece en Terminal y pásamelo.

Como el plugin se compila en tu propio Mac, se firma solo para ese Mac. Para usarlo en otro Mac, repite estos pasos allí; no copies el plugin ya compilado.
