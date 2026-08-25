#pragma once

// 系列共享的 UI 工具函数 (Analyzer 版, 从 BandPass 裁剪):
// 只保留设置面板旋钮绘制用的 DrawKnob; 频率格式化/解析、Ghost 覆盖、随机颜色菜单
// 随 BandPass 的随机/滤波功能一并移除。

#include "IControls.h"
#include "../Theme.h"

#include <algorithm>
#include <cmath>

BEGIN_IPLUG_NAMESPACE
BEGIN_IGRAPHICS_NAMESPACE

// 频谱左侧 dB 刻度区宽度 (px)。Analyzer.cpp 图例左移对齐 + SpectrumPad 内部图形区右移共用。
constexpr float kDbAxisW = 56.f;

// 标准旋钮手柄: 白环 + 深色核心
inline void DrawKnob(IGraphics &g, float cx, float cy) {
  g.FillCircle(COL_100(), cx, cy, HANDLE_R + HANDLE_RING);
  g.FillCircle(COL_900(), cx, cy, HANDLE_R);
}

END_IGRAPHICS_NAMESPACE
END_IPLUG_NAMESPACE
