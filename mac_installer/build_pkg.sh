#!/bin/sh
# Builds the macOS installer (Reanima-0.9.5.pkg): the standard Installer window with welcome, licence,
# a choice of VST3 / Audio Unit, an admin password prompt and a progress bar.
# Installs for every user of the Mac, where commercial plugins go:
#   /Library/Audio/Plug-Ins/VST3/Blackpanda/Reanima.vst3   and   /Library/Audio/Plug-Ins/Components/Reanima.component
# Usage (from the repository root, after building):  sh mac_installer/build_pkg.sh <folder with Reanima.vst3 and Reanima.component> <output.pkg>
set -e
SRC="$1"; OUT="$2"; VER="0.9.5"
HERE="$(cd "$(dirname "$0")" && pwd)"
W="$(mktemp -d)"

mkdir -p "$W/vst3/Library/Audio/Plug-Ins/VST3/Blackpanda" "$W/au/Library/Audio/Plug-Ins/Components" "$W/res"
cp -R "$SRC/Reanima.vst3" "$W/vst3/Library/Audio/Plug-Ins/VST3/Blackpanda/"
cp -R "$SRC/Reanima.component" "$W/au/Library/Audio/Plug-Ins/Components/"

# Without this, Installer "relocates" a bundle to wherever an older copy with the same ID lives instead of the folder above.
for k in vst3 au; do
    pkgbuild --analyze --root "$W/$k" "$W/$k.plist"
    i=0
    while plutil -replace $i.BundleIsRelocatable -bool NO "$W/$k.plist" 2>/dev/null; do i=$((i+1)); done
done

chmod +x "$HERE/scripts/postinstall"
pkgbuild --root "$W/vst3" --component-plist "$W/vst3.plist" --identifier com.blackpanda.reanima.vst3 --version "$VER" --install-location / "$W/ReanimaVST3.pkg"
pkgbuild --root "$W/au" --component-plist "$W/au.plist" --identifier com.blackpanda.reanima.au --version "$VER" --install-location / --scripts "$HERE/scripts" "$W/ReanimaAU.pkg"

cp "$HERE/welcome.html" "$HERE/conclusion.html" "$W/res/"
cp LICENSE.md "$W/res/license.txt"
productbuild --distribution "$HERE/distribution.xml" --resources "$W/res" --package-path "$W" "$OUT"
rm -rf "$W"
echo "Instalador creado: $OUT"
