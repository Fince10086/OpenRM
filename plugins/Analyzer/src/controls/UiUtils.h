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

// 频谱右侧布局 (Analyzer.cpp 图例与 SpectrumPad 共用):
// - kDbTickW: dB 刻度文字区宽度
// - kGainBarW: Gain 竖条横向宽度 = 右侧滑块 track 纵向宽度 (IVSliderControl 默认 2.f) 的 4 倍
constexpr float kDbTickW = 52.f;
constexpr float kGainBarW = 4.f * 2.f;

// 标准旋钮手柄: 白环 + 深色核心
inline void DrawKnob(IGraphics &g, float cx, float cy) {
  g.FillCircle(COL_100(), cx, cy, HANDLE_R + HANDLE_RING);
  g.FillCircle(COL_900(), cx, cy, HANDLE_R);
}

END_IGRAPHICS_NAMESPACE
END_IPLUG_NAMESPACE
