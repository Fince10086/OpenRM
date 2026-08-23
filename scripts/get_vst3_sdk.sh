#!/bin/sh
# 下载 Steinberg VST3 SDK 到 iPlug2 的 Dependencies 目录 (iPlug2 上游不追踪此目录)。
# 默认钉在 v3.8.1_build_84 —— 即本项目已验证可用的版本, 可用环境变量 VST3SDK_REF 覆盖。
set -e

SDK_DIR="$(dirname "$0")/../third_party/iPlug2/Dependencies/IPlug/VST3_SDK"
REF="${VST3SDK_REF:-v3.8.1_build_84}"

if [ -d "$SDK_DIR/.git" ] || [ -d "$SDK_DIR/public.sdk" ]; then
  echo "VST3 SDK 已存在于 $SDK_DIR，跳过。"
  exit 0
fi

echo "正在克隆 Steinberg VST3 SDK ($REF，含子模块约 500MB)……"
git clone --recursive --branch "$REF" --shallow-submodules \
  https://github.com/steinbergmedia/vst3sdk.git "$SDK_DIR"
echo "完成。"
