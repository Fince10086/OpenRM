// ============================================================================
// FilterNodePad.h — 黑白极简风格滤波节点可视化控件 (模仿 nono.feizao.org)
//   白底 + 2px 纯黑边框 + #ccc 细网格 + 浅灰频带 + 实心黑球节点
//   继承 IVXYPadControl: X = 中心频率 (对数), Y = 带宽 (octave)
// ============================================================================
#pragma once

#include "IControls.h"

BEGIN_IPLUG_NAMESPACE
BEGIN_IGRAPHICS_NAMESPACE

class FilterNodePad : public IVXYPadControl
{
public:
  FilterNodePad(const IRECT& bounds, const std::initializer_list<int>& params,
                const char* label = "", const IVStyle& style = DEFAULT_STYLE,
                float handleRadius = 9.f)
  : IVXYPadControl(bounds, params, label, style, handleRadius, true, true)
  {
  }

  // 背景轨道: 白底黑框 + #ccc 网格 + 浅灰频带
  void DrawTrack(IGraphics& g) override
  {
    const IRECT tb = mWidgetBounds;

    // 白底 + 2px 纯黑边框 (nono 容器语言: border:2px solid #000)
    g.FillRect(IColor(255, 255, 255, 255), tb);
    g.DrawRect(IColor(255, 0, 0, 0), tb, nullptr, 2.f);

    // #ccc 细网格 (3 横线 + 3 竖线, 对应 border:1px solid #ccc)
    const IColor gridCol(255, 204, 204, 204);
    for (int i = 1; i < 4; ++i)
    {
      const float y = tb.T + tb.H() * i / 4.f;
      g.DrawLine(gridCol, tb.L, y, tb.R, y, nullptr, 1.f);
      const float x = tb.L + tb.W() * i / 4.f;
      g.DrawLine(gridCol, x, tb.T, x, tb.B, nullptr, 1.f);
    }

    // ---- 水平频带: 以节点频率为中心的浅灰块, 宽度随带宽 ----
    const float xc = static_cast<float>(GetValue(0)); // 归一化中心频率
    const float yv = static_cast<float>(GetValue(1)); // 归一化带宽
    const float cx = tb.L + xc * tb.W();
    const float cy = tb.B - yv * tb.H();              // 带宽越大节点越高

    const float bandFrac = 0.10f + 0.30f * yv;        // 光带半宽随带宽
    const float xl = cx - bandFrac * tb.W();
    const float xr = cx + bandFrac * tb.W();
    const float topH = (tb.B - cy) * 0.16f;           // 顶部收窄段高度

    // 主体宽段 (#f0f0f0) + 顶部窄段 (#e5e5e5) + 底部黑色边线 2px
    g.FillRect(IColor(255, 240, 240, 240), IRECT(xl, cy + topH, xr, tb.B));
    g.FillRect(IColor(255, 229, 229, 229),
               IRECT(cx - bandFrac * tb.W() * 0.30f, cy,
                     cx + bandFrac * tb.W() * 0.30f, cy + topH));
    g.FillRect(IColor(255, 0, 0, 0), IRECT(xl, tb.B - 2.f, xr, tb.B));
  }

  // 节点手柄: 黑色引导线 + 实心黑球 (nono 填充格语言) + 白芯
  void DrawHandle(IGraphics& g, const IRECT& trackBounds, const IRECT& handleBounds) override
  {
    const float cx = handleBounds.MW();
    const float cy = handleBounds.MH();
    const float r  = handleBounds.W() * 0.5f;

    g.DrawLine(IColor(255, 0, 0, 0), cx, handleBounds.T, cx, cy - r, nullptr, 1.f);

    g.FillCircle(IColor(255, 0, 0, 0), cx, cy, r);
    g.FillCircle(IColor(255, 255, 255, 255), cx, cy, r * 0.25f);
  }
};

END_IGRAPHICS_NAMESPACE
END_IPLUG_NAMESPACE
