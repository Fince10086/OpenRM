#pragma once
// ============================================================================
// Theme.h — 黑白极简配色统一入口
//   单一定义源: 供 BandPass.cpp / FilterNodePad.h 等所有 UI 代码引用,
//   避免各文件各自定义重复颜色 (历史遗留的 COL_* / FP_* 双份已合并至此)。
//   注意: IColor 构造非 constexpr, 故用 static const (每 TU 内链, 无 ODR 问题)。
// ============================================================================
#include "IGraphics.h"

namespace iplug { namespace igraphics {

static const IColor COL_BG    (255, 255, 255, 255);  // 面板/控件底白
static const IColor COL_BLACK (255,   0,   0,   0);  // 主黑: 边框/文字/手柄/填充
static const IColor COL_DIM   (255, 102, 102, 102);  // #666 次要文字 (滑块数值)
static const IColor COL_FAINT (255, 153, 153, 153);  // #999 弱化文字 (版本号)
static const IColor COL_TRACK (255, 236, 236, 236);  // 滑轨底 #ececec
static const IColor COL_HOVER (255, 240, 240, 240);  // #f0f0f0 hover 填充
static const IColor COL_GRID  (255, 204, 204, 204);  // #ccc 细网格线 (pad)

} } // namespace iplug::igraphics
