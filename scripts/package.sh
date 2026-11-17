#!/usr/bin/env bash
# Packages the built mod into a distributable archive.
#
# Kept out of the workflow file on purpose: embedding a multi-line text file via
# a heredoc inside a YAML block scalar requires re-indenting every line, which is
# fragile and was the cause of a workflow that GitHub refused to parse.
#
# Usage: scripts/package.sh <version> [outdir]
set -euo pipefail

cd "$(dirname "$0")/.."

VERSION="${1:?usage: package.sh <version> [outdir]}"
OUTDIR="${2:-dist}"

NAME="closeoreui-${VERSION}-linux-x86_64"
SO=build/x86_64/libcloseoreui.so

[[ -f "$SO" ]] || { echo "missing $SO; run scripts/build.sh first" >&2; exit 1; }
[[ -f LICENSE ]] || { echo "missing LICENSE" >&2; exit 1; }

rm -rf "$OUTDIR"
mkdir -p "$OUTDIR"
cp "$SO" "$OUTDIR/"
# AGPL section 6: conveying the binary requires conveying the licence with it.
cp LICENSE "$OUTDIR/"

cat > "$OUTDIR/README.txt" <<'EOF'
CloseOreUI — disable OreUI in Minecraft Bedrock for mcpelauncher
===============================================================

Install
-------
Put libcloseoreui.so into mcpelauncher's mods directory:

  native:  ~/.local/share/mcpelauncher/mods/
  flatpak: ~/.var/app/io.mrarm.mcpelauncher/data/mcpelauncher/mods/

A config file is created next to the mod at CloseOreUI/config.json on first run.
To confirm it worked, set CLOSEOREUI_DEBUG=1 and look for these log lines:

  "OreUI config function at ..."
  "screens patched"

This mod is not tied to a specific game version: the target function is located
structurally at load time. The behavioural layer does assume a certain memory
layout, so if a version ever breaks, the log will say why.

If the launcher was built with the menu API, an entry appears under
Mods > CloseOreUI with a master switch.

This package contains the mod only. No Minecraft files are included.
EOF

( cd "$OUTDIR" && sha256sum libcloseoreui.so > SHA256SUMS.txt )

tar -czf "$OUTDIR/${NAME}.tar.gz" -C "$OUTDIR" \
    libcloseoreui.so README.txt LICENSE SHA256SUMS.txt

echo "packaged: $OUTDIR/${NAME}.tar.gz"
echo "NAME=$NAME"
