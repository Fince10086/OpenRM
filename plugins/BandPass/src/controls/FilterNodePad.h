// ============================================================================
// FilterNodePad.h — GRM 风格滤波节点可视化控件
//   深色背景 + 水平频谱光带 (随节点频率/带宽变化) + 可拖动小球
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

  // 背景轨道: 深色底 + 网格 + 频谱光带
  void DrawTrack(IGraphics& g) override
  {
    const IRECT tb = mWidgetBounds;

    // 深色底面
    g.FillRect(IColor(255, 30, 30, 30), tb);
    g.DrawRect(IColor(255, 70, 70, 70), tb, nullptr, 1.f);

    // 网格 (4 水平线 + 垂直中线)
    const IColor gridCol(255, 55, 55, 55);
    for (int i = 1; i < 4; ++i)
    {
      const float y = tb.T + tb.H() * i / 4.f;
      g.DrawLine(gridCol, tb.L, y, tb.R, y, nullptr, 1.f);
    }
    g.DrawLine(gridCol, (tb.L + tb.R) * 0.5f, tb.T, (tb.L + tb.R) * 0.5f, tb.B, nullptr, 1.f);

    // ---- 水平频谱光带: 以节点频率为中心的带, 宽度随带宽 ----
    const float xc = static_cast<float>(GetValue(0)); // 归一化中心频率
    const float yv = static_cast<float>(GetValue(1)); // 归一化带宽
    const float cx = tb.L + xc * tb.W();
    const float cy = tb.B - yv * tb.H();              // 带宽越大节点越高

    const float bandFrac = 0.10f + 0.30f * yv;        // 光带半宽随带宽
    const float xl = cx - bandFrac * tb.W();
    const float xr = cx + bandFrac * tb.W();
    const float topH = (tb.B - cy) * 0.16f;           // 顶部收窄段高度

    // 顶部窄段 (半透明琥珀)
    g.FillRect(IColor(90, 224, 180, 92),
               IRECT(cx - bandFrac * tb.W() * 0.30f, cy,
                     cx + bandFrac * tb.W() * 0.30f, cy + topH));
    // 主体宽段
    g.FillRect(IColor(70, 224, 180, 92),
               IRECT(xl, cy + topH, xr, tb.B));
    // 底部亮线
    g.FillRect(IColor(255, 224, 180, 92),
               IRECT(xl, tb.B - 2.f, xr, tb.B));
  }

  // 小球手柄: 指示线 + 荧光圆
  void DrawHandle(IGraphics& g, const IRECT& trackBounds, const IRECT& handleBounds) override
  {
    const float cx = handleBounds.MW();
    const float cy = handleBounds.MH();
    const float r  = handleBounds.W() * 0.5f;

    g.DrawLine(IColor(255, 224, 180, 92), cx, handleBounds.T, cx, cy - r, nullptr, 1.f);

    g.FillCircle(IColor(255, 224, 180, 92), cx, cy, r);
    g.FillCircle(IColor(255, 244, 214, 140), cx, cy, r * 0.55f);
    g.FillCircle(IColor(255, 255, 255, 255), cx, cy, r * 0.22f);
  }
};

END_IGRAPHICS_NAMESPACE
END_IPLUG_NAMESPACE
