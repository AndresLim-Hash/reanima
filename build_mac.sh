#!/bin/sh
# Builds Reanima on macOS as one universal binary (Apple Silicon and Intel) and installs it for the current user:
#   VST3 -> ~/Library/Audio/Plug-Ins/VST3/Blackpanda/Reanima.vst3   (Blackpanda's folder, as on Windows)
#   AU   -> ~/Library/Audio/Plug-Ins/Components/Reanima.component  (macOS only looks for Audio Units there)
# Needs: Xcode (from the App Store, opened once) and CMake (https://cmake.org/download or `brew install cmake`).
# Usage: open Terminal in this folder and run   sh build_mac.sh
set -e
cd "$(dirname "$0")"

command -v cmake >/dev/null 2>&1 || { echo "Falta CMake: instálalo desde https://cmake.org/download (o: brew install cmake)"; exit 1; }
xcode-select -p >/dev/null 2>&1 || { echo "Falta Xcode: instálalo desde la App Store, ábrelo una vez y vuelve a ejecutar esto."; exit 1; }

cmake -S . -B build-mac -G Xcode -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0
cmake --build build-mac --config Release --target Reanima_VST3 Reanima_AU -- -quiet

VST3_DIR="$HOME/Library/Audio/Plug-Ins/VST3/Blackpanda"
AU_DIR="$HOME/Library/Audio/Plug-Ins/Components"
mkdir -p "$VST3_DIR" "$AU_DIR"
rm -rf "$VST3_DIR/Reanima.vst3" "$AU_DIR/Reanima.component"
cp -R build-mac/Reanima_artefacts/Release/VST3/Reanima.vst3 "$VST3_DIR/"
cp -R build-mac/Reanima_artefacts/Release/AU/Reanima.component "$AU_DIR/"

# Built here, not signed by an Apple developer account: signed for this Mac only, quarantine lifted.
codesign --force --deep --sign - "$VST3_DIR/Reanima.vst3" "$AU_DIR/Reanima.component"
xattr -dr com.apple.quarantine "$VST3_DIR/Reanima.vst3" "$AU_DIR/Reanima.component" 2>/dev/null || true

# Tell macOS a new Audio Unit is there, and check it (Logic and GarageBand use this check too).
killall -9 AudioComponentRegistrar 2>/dev/null || true
if command -v auval >/dev/null 2>&1; then
    auval -v aumu Rnm1 Bpnd >/tmp/reanima_auval.txt 2>&1 && echo "Audio Unit validado (auval OK)." || echo "auval avisó de algo: mira /tmp/reanima_auval.txt"
fi
lipo -info "$VST3_DIR/Reanima.vst3/Contents/MacOS/Reanima" || true
echo "Instalado: VST3 en $VST3_DIR y AU en $AU_DIR. Abre Ableton y reescanea los plugins."
