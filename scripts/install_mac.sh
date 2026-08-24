#!/bin/bash
# ============================================================================
# ORM BandPass - macOS 安装 / 卸载脚本
#
# 功能:
#   1. 把 build/out 里的独立 App / VST3 / AU 自动复制到 macOS 对应目录
#   2. 自动清除 com.apple.quarantine 属性 (下载产物无需再手动 xattr)
#   3. 检测目标目录已存在的同产品, 按版本号决定: 更新 / 覆盖 / 跳过
#   4. 可卸载 (并可选清理偏好设置文件)
#
# 产物要求: 先构建 (cmake --build build --target ORMBandPass-app ORMBandPass-vst3 ORMBandPass-au)
# 用法:
#   ./scripts/install_mac.sh           交互式菜单
#   ./scripts/install_mac.sh --install 直接进入安装
#   ./scripts/install_mac.sh --uninstall 直接进入卸载
# ============================================================================
set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# 支持两种放置布局:
#   仓库内: scripts/install_mac.sh, 产物在仓库根 build/out
#   zip 解压: 脚本与产物同目录 (扁平), 由 CI 打包
if [ -d "$SCRIPT_DIR/../build/out" ]; then
  ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
  OUT="$ROOT/build/out"
  CFG="$ROOT/plugins/BandPass/config.h"
else
  ROOT="$SCRIPT_DIR"
  OUT="$SCRIPT_DIR"
  CFG=""
fi

NAME="ORMBandPass"
VERSION=""
if [ -n "$CFG" ]; then
  VERSION="$(grep -o 'PLUG_VERSION_STR "[0-9.]*"' "$CFG" 2>/dev/null | grep -o '[0-9.]*')"
fi
if [ -z "$VERSION" ]; then
  # 扁平布局: 从产物 Info.plist 读版本
  VERSION="$(/usr/libexec/PlistBuddy -c "Print :CFBundleShortVersionString" "$OUT/$NAME.app/Contents/Info.plist" 2>/dev/null)"
fi
VERSION="${VERSION:-unknown}"

# ---- 颜色 (用户友好) ----
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'
BOLD='\033[1m'; DIM='\033[2m'; NC='\033[0m'

ok()   { printf "${GREEN}  ✓${NC} %s\n" "$*"; }
warn() { printf "${YELLOW}  ⚠${NC} %s\n" "$*"; }
err()  { printf "${RED}  ✗${NC} %s\n" "$*"; }
info() { printf "${CYAN}  ▶${NC} %s\n" "$*"; }
hr()   { printf "${DIM}──────────────────────────────────────────────${NC}\n"; }
title(){ printf "${BOLD}%s${NC}\n" "$*"; }

# ---- 版本比较: ver_cmp A B → 0(A=B) 1(A<B) 2(A>B) ----
ver_cmp() {
  local a="$1" b="$2" pa pb na nb i n
  IFS='.' read -r -a pa <<< "$a"
  IFS='.' read -r -a pb <<< "$b"
  n="${#pa[@]}"; [ "${#pb[@]}" -gt "$n" ] && n="${#pb[@]}"
  for i in $(seq 0 $((n - 1))); do
    na="${pa[$i]:-0}"; nb="${pb[$i]:-0}"
    [ "$na" -gt "$nb" ] && { echo 2; return; }
    [ "$na" -lt "$nb" ] && { echo 1; return; }
  done
  echo 0
}

# ---- 读取已安装 bundle 的版本 (Info.plist → CFBundleShortVersionString) ----
get_installed_version() { # $1 = bundle 路径
  [ -d "$1" ] || return 1
  /usr/libexec/PlistBuddy -c "Print :CFBundleShortVersionString" "$1/Contents/Info.plist" 2>/dev/null
}

# ---- 清除 quarantine 属性 ----
clear_quarantine() { # $1 = 路径, $2 = 可选 "sudo"
  local sudo_cmd=""
  [ "${2:-}" = "sudo" ] && sudo_cmd="sudo"
  if $sudo_cmd xattr -dr com.apple.quarantine "$1" 2>/dev/null; then
    ok "已清除 quarantine: $(basename "$1")"
  else
    warn "quarantine 已不存在或无需处理: $(basename "$1")"
  fi
}

# ---- 确认提示: confirm "文案" 默认值(y/n) ----
confirm() { # $1 文案, $2 默认 y/n
  local ans
  while true; do
    printf "${BOLD}%s${NC} [%s/%s] " "$1" "$([ "$2" = "y" ] && echo Y || echo y)" "$([ "$2" = "n" ] && echo N || echo n)"
    read -r ans || return 1
    ans="${ans:-$2}"
    case "$ans" in y|Y) return 0;; n|N) return 1;; *) warn "请输入 y 或 n";; esac
  done
}

# ---- 检查构建产物 ----
check_products() {
  local missing=0
  for p in "$NAME.app" "$NAME.vst3" "$NAME.component"; do
    [ -d "$OUT/$p" ] || { err "缺少 $OUT/$p — 请先构建:"; printf "${DIM}    cmake --build build --target $NAME-app $NAME-vst3 $NAME-au${NC}\n"; missing=1; }
  done
  [ "$missing" = "1" ] && return 1
  return 0
}

# ---- 安装单个组件 (含版本检测) ----
install_one() { # $1 显示名, $2 源 bundle, $3 目标目录, $4 sudo(空或sudo)
  local label="$1" src="$2" dir="$3" sudo_cmd="${4:-}"
  local dst="$dir/$(basename "$src")"
  local new_v inst_v cmp ans

  new_v="$(get_installed_version "$src")"; new_v="${new_v:-$VERSION}"
  mkdir -p "$dir" 2>/dev/null || $sudo_cmd mkdir -p "$dir" 2>/dev/null || { err "无法创建目录 $dir"; return 1; }

  if [ -d "$dst" ]; then
    inst_v="$(get_installed_version "$dst")"
    cmp="$(ver_cmp "${inst_v:-0.0.0}" "$new_v")"
    printf "\n  ${BOLD}%s${NC}: 已安装 v${inst_v:-?} → 新版本 v${new_v}\n" "$label"
    case "$cmp" in
      1) info "检测到更新版本, 建议更新" ;;
      2) warn "已安装版本更新 ($inst_v > $new_v), 覆盖将降级" ;;
      0) info "版本相同" ;;
    esac
    if ! confirm "  是否覆盖安装?" "y"; then
      warn "跳过 $label"
      return 0
    fi
  else
    info "$label: 全新安装 (v$new_v)"
  fi

  $sudo_cmd rm -rf "$dst"
  if $sudo_cmd cp -R "$src" "$dst"; then
    ok "已安装: $dst"
    clear_quarantine "$dst" "$sudo_cmd"
    return 0
  fi
  err "安装失败: $dst"
  return 1
}

# ---- 安装流程 ----
do_install() {
  check_products || return 1
  local scope sudo_cmd APP_DIR VST3_DIR AU_DIR ans

  title "安装范围"
  echo "  1) 仅当前用户  (推荐, 无需密码)"
  echo "  2) 所有用户    (需要管理员密码)"
  printf "请选择 [1/2] (默认 1): "; read -r ans
  case "${ans:-1}" in
    2) scope="system"; sudo_cmd="sudo"; APP_DIR="/Applications"; VST3_DIR="/Library/Audio/Plug-Ins/VST3"; AU_DIR="/Library/Audio/Plug-Ins/Components" ;;
    *) scope="user";  sudo_cmd="";  APP_DIR="$HOME/Applications"; VST3_DIR="$HOME/Library/Audio/Plug-Ins/VST3"; AU_DIR="$HOME/Library/Audio/Plug-Ins/Components" ;;
  esac

  hr; info "安装目标 ($scope):"; echo "    $APP_DIR"; echo "    $VST3_DIR"; echo "    $AU_DIR"; hr

  install_one "独立 App" "$OUT/$NAME.app"     "$APP_DIR" "$sudo_cmd"
  install_one "VST3 插件" "$OUT/$NAME.vst3"   "$VST3_DIR" "$sudo_cmd"
  install_one "AU 插件"   "$OUT/$NAME.component" "$AU_DIR" "$sudo_cmd"

  printf "\n"
  ok "安装完成!"
  warn "提示: 若 DAW 中看不到新装的插件, 请重启 DAW (AU 插件可能需先退出再重新扫描)。"
  [ "$scope" = "user" ] && warn "当前为「仅当前用户」安装, 其他系统账户无法使用; 需要全系统请重新运行并选 2。"
}

# ---- 卸载流程 ----
do_uninstall() {
  local found=0
  local paths=(
    "$HOME/Applications/$NAME.app"
    "/Applications/$NAME.app"
    "$HOME/Library/Audio/Plug-Ins/VST3/$NAME.vst3"
    "/Library/Audio/Plug-Ins/VST3/$NAME.vst3"
    "$HOME/Library/Audio/Plug-Ins/Components/$NAME.component"
    "/Library/Audio/Plug-Ins/Components/$NAME.component"
  )
  local p v i=0

  hr; title "检测已安装的组件:"; hr
  for p in "${paths[@]}"; do
    if [ -d "$p" ]; then
      v="$(get_installed_version "$p")"
      printf "  ${BOLD}[%d]${NC} %s  (v%s)\n" "$((i+1))" "$p" "${v:-?}"
      found=1
    fi
    i=$((i+1))
  done
  [ "$found" = "0" ] && { warn "未检测到已安装的 $NAME 组件"; return 0; }

  printf "\n"
  if ! confirm "确定要卸载以上全部组件吗?" "n"; then
    warn "已取消卸载"
    return 0
  fi

  for p in "${paths[@]}"; do
    if [ -d "$p" ]; then
      case "$p" in
        /Applications/*|/Library/*) sudo rm -rf "$p" ;;
        *) rm -rf "$p" ;;
      esac
      ok "已删除: $p"
    fi
  done

  # 可选清理偏好设置
  local settings="$HOME/Library/Application Support/OpenRM/$NAME.settings"
  if [ -f "$settings" ]; then
    printf "\n"
    if confirm "是否同时删除偏好设置文件?\n    $settings" "n"; then
      rm -f "$settings" && ok "已删除偏好设置"
    else
      info "保留偏好设置"
    fi
  fi

  printf "\n"; ok "卸载完成!"
  warn "提示: DAW 可能缓存插件信息, 重启 DAW 后消失; AU 缓存异常时可执行: killall -SIGKILL AudioComponentRegistrar"
}

# ---- 主菜单 ----
main() {
  printf "\n${BOLD}┌──────────────────────────────────────────────┐${NC}\n"
  printf "${BOLD}│  ORM BandPass  macOS 安装助手  v${VERSION}          │${NC}\n"
  printf "${BOLD}└──────────────────────────────────────────────┘${NC}\n\n"

  case "${1:-}" in
    --install)   do_install; return $? ;;
    --uninstall) do_uninstall; return $? ;;
    -h|--help)   head -30 "$0" | sed -n '1,12p'; echo "... 见脚本头部注释"; return 0 ;;
    "") ;;
    *) err "未知参数: $1 (支持 --install / --uninstall / -h)"; return 1 ;;
  esac

  local ans
  while true; do
    printf "\n  ${BOLD}1${NC} 安装 (App + VST3 + AU)\n"
    printf "  ${BOLD}2${NC} 卸载\n"
    printf "  ${BOLD}3${NC} 退出\n\n"
    printf "请选择 [1/2/3]: "; read -r ans || break
    case "$ans" in
      1) do_install ;;
      2) do_uninstall ;;
      3|q|Q) echo; ok "已退出"; break ;;
      *) warn "无效选项, 请重新输入" ;;
    esac
  done
}

main "${1:-}"
