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
CloseOreUI — 关闭 Minecraft 基岩版 OreUI 的 mcpelauncher 模组
=============================================================

安装
----
把 libcloseoreui.so 放到 mcpelauncher 的 mods 目录：

  原生:    ~/.local/share/mcpelauncher/mods/
  Flatpak: ~/.var/app/io.mrarm.mcpelauncher/data/mcpelauncher/mods/

启动游戏后会自动在同级 CloseOreUI/config.json 生成配置。
想确认是否生效，可设 CLOSEOREUI_DEBUG=1 查看日志中的
"OreUI config function at ..." 与 "screens patched" 两行。

本模组不绑定特定游戏版本：目标函数在启动时按结构定位。
但行为层依赖一定的内存布局，若某版本失效，日志会给出明确原因。

本包只包含模组本身，不含任何 Minecraft 文件。

许可
----
GNU Affero General Public License v3.0，全文见同目录 LICENSE。
对应源码可从发布该版本的仓库获取。
EOF

( cd "$OUTDIR" && sha256sum libcloseoreui.so > SHA256SUMS.txt )

tar -czf "$OUTDIR/${NAME}.tar.gz" -C "$OUTDIR" \
    libcloseoreui.so README.txt LICENSE SHA256SUMS.txt

echo "packaged: $OUTDIR/${NAME}.tar.gz"
echo "NAME=$NAME"
