#pragma once

// 系列共享的 UI 工具函数

#include "IControls.h"
#include "../Theme.h"

BEGIN_IPLUG_NAMESPACE
BEGIN_IGRAPHICS_NAMESPACE

// 标准旋钮手柄: 白环 + 深色核心
inline void DrawKnob(IGraphics &g, float cx, float cy) {
  g.FillCircle(COL_100(), cx, cy, HANDLE_R + HANDLE_RING);
  g.FillCircle(COL_900(), cx, cy, HANDLE_R);
}

// 禁用态半透明覆盖层
inline void DrawGhostOverlay(IGraphics &g, const IRECT &r) {
  const IColor base = COL_100();
  g.FillRect(IColor(150, base.R, base.G, base.B), r);
}

END_IGRAPHICS_NAMESPACE
END_IPLUG_NAMESPACE
