#!/bin/sh
# 制作 macOS 安装镜像 (DMG): 打包 VST3 + AU + 独立 App 三个产物。
# 依赖: create-dmg (brew install create-dmg); 产物来自 build/out/。
# 用法: ./scripts/make_dmg.sh [版本号]   (缺省从 plugins/BandPass/config.h 读取)
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"

# ---- 版本单一来源: config.h ----
if [ -n "$1" ]; then
  VERSION="$1"
else
  VERSION=$(grep -o 'PLUG_VERSION_STR "[0-9.]*"' "$ROOT/plugins/BandPass/config.h" | grep -o '[0-9.]*')
fi

NAME="ORMBandPass"
OUT="$ROOT/build/out"
STAGE="/tmp/${NAME}-dmg-stage"
DMG="$ROOT/build/${NAME}-${VERSION}-mac.dmg"

[ -d "$OUT/$NAME.vst3" ]       || { echo "缺少 $OUT/$NAME.vst3, 请先构建 (cmake --build build --target $NAME-vst3)"; exit 1; }
[ -d "$OUT/$NAME.component" ]  || { echo "缺少 $OUT/$NAME.component, 请先构建 AU 目标"; exit 1; }
[ -d "$OUT/$NAME.app" ]        || { echo "缺少 $OUT/$NAME.app, 请先构建 APP 目标"; exit 1; }
command -v create-dmg >/dev/null || { echo "缺少 create-dmg: 请先执行 brew install create-dmg"; exit 1; }

# ---- 组装暂存目录 ----
rm -rf "$STAGE" && mkdir -p "$STAGE"
cp -R "$OUT/$NAME.vst3" "$OUT/$NAME.component" "$OUT/$NAME.app" "$STAGE/"
cat > "$STAGE/README.txt" <<EOF
$NAME $VERSION

拖入 Applications 安装独立 App。
VST3 / AU 请复制到对应插件目录:
  VST3 -> ~/Library/Audio/Plug-Ins/VST3
  AU   -> ~/Library/Audio/Plug-Ins/Components

macOS 15+ 首次打开若提示"无法验证开发者/已损坏":
  右键点 app -> 打开, 或执行:
  sudo xattr -cr /Applications/ORMBandPass.app
EOF

# ---- 生成 DMG ----
create-dmg \
  --volname "$NAME $VERSION" \
  --window-size 620 420 \
  --icon-size 96 \
  --icon "$NAME.app" 170 180 \
  --hide-extension "$NAME.vst3" \
  --hide-extension "$NAME.component" \
  --app-drop-link 440 180 \
  "$DMG" "$STAGE"

echo "完成: $DMG"
