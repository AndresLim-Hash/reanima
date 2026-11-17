#!/bin/sh
# Installs Reanima (downloaded from GitHub) on this Mac: double click it.
#   VST3 -> ~/Library/Audio/Plug-Ins/VST3/Blackpanda   AU -> ~/Library/Audio/Plug-Ins/Components
cd "$(dirname "$0")"
VST3_DIR="$HOME/Library/Audio/Plug-Ins/VST3/Blackpanda"
AU_DIR="$HOME/Library/Audio/Plug-Ins/Components"
mkdir -p "$VST3_DIR" "$AU_DIR"
rm -rf "$VST3_DIR/Reanima.vst3" "$AU_DIR/Reanima.component"
cp -R Reanima.vst3 "$VST3_DIR/"
cp -R Reanima.component "$AU_DIR/"
# Downloaded from the internet and not signed by an Apple developer: lift the quarantine and sign it for this Mac.
xattr -dr com.apple.quarantine "$VST3_DIR/Reanima.vst3" "$AU_DIR/Reanima.component" 2>/dev/null || true
codesign --force --deep --sign - "$VST3_DIR/Reanima.vst3" "$AU_DIR/Reanima.component" 2>/dev/null || true
killall -9 AudioComponentRegistrar 2>/dev/null || true
echo "Reanima instalado. Abre Ableton y reescanea los plugins."
