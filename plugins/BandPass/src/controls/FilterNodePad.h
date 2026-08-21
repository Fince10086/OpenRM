// ============================================================================
// FilterNodePad.h — 黑白极简风格滤波节点可视化控件 (模仿 nono.feizao.org / GRM)
//   白底 + 2px 纯黑边框 + #ccc 细网格 + 实心黑球节点
//   左侧 LEFT/RIGHT 竖排标签 (逆时针 90°)
//   四角可点击编辑的参数 (外侧): 左上 CENTER / 右上 BANDWIDTH / 左下 LOWCUT / 右下 HIGHCUT
//   网格下方一条双控制点范围滑块 (对应 low/high 截止频率, 两点间涂黑)
//   继承 IVXYPadControl: 内部 valIdx0 = 中心频率 (对数), valIdx1 = 带宽 (oct)
//
//   角标点击 → CreateTextEntry 弹出行内编辑. valIdx 4..7 越界 (本控件只有 2 个 vals)
//   会触发 IControl::GetParamIdx 的 assert. 因此用 mEditingCorner 跟踪当前角标,
//   CreateTextEntry 传 kNoValIdx (-1) 跳过参数映射, OnTextEntryCompletion 据此分发.
// ============================================================================
#pragma once

#include "IControls.h"
#include "../Theme.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cctype>

BEGIN_IPLUG_NAMESPACE
BEGIN_IGRAPHICS_NAMESPACE

// 角标 id 约定 (valIdx 4..7, 0/1 已被 freq/bw 占用)
enum EPadCorner : int
{
  kCornerCenter = 4,
  kCornerBw     = 5,
  kCornerLow    = 6,
  kCornerHigh   = 7
};

class FilterNodePad : public IVXYPadControl
{
public:
  // 回调: 把用户交互交回插件层换算并写参 (插件持有 param 范围/钳位逻辑)
  struct Hooks
  {
    std::function<void()> gestureBegin;                                  // 滑块手势起点 → MaybePushGestureUndo
    std::function<void(int cornerId, double value)> editCorner;          // 角标文本提交 (value: Hz 或 oct)
    std::function<void(double lowNorm, double highNorm)> editBand;        // 滑块拖点 (归一化 20..20k)
  };

  FilterNodePad(const IRECT& bounds, const std::initializer_list<int>& params,
                const char* label, const IVStyle& style, const Hooks& hooks,
                float handleRadius = 9.f)
  : IVXYPadControl(bounds, params, /*label*/ "", style, handleRadius, true, true)
  , mHooks(hooks)
  , mSideLabel(label)  // 左侧竖排标签 (LEFT/RIGHT); 留空字符串则不画
  {
    SetTextEntryLength(20);
  }

  void Draw(IGraphics& g) override
  {
    IVXYPadControl::Draw(g); // 背景 + 边框 + 网格 + 节点 (label 留空故不画)
    DrawSlider(g);
    DrawCorner(g, kCornerCenter);
    DrawCorner(g, kCornerBw);
    DrawCorner(g, kCornerLow);
    DrawCorner(g, kCornerHigh);
    DrawSideLabel(g);  // pad 框左侧外侧竖排 (LEFT/RIGHT)
  }

  void OnMouseDown(float x, float y, const IMouseMod& mod) override
  {
    mOverCorner = -1;
    // 1) 角标 → 行内文本编辑. 用 mEditingCorner 记录角 id (避免 valIdx 越界触发 assert)
    for (int id : { kCornerCenter, kCornerBw, kCornerLow, kCornerHigh })
    {
      if (CornerRect(id).Contains(x, y))
      {
        WDL_String init; GetCornerLabel(id, init);
        EAlign align = (id == kCornerBw || id == kCornerHigh) ? EAlign::Far : EAlign::Near;
        IText t(11, COL_BLACK, "Outfit-SemiBold", align, EVAlign::Middle);
        mEditingCorner = id;
        GetUI()->CreateTextEntry(*this, t, CornerRect(id), init.Get(), kNoValIdx);
        return;
      }
    }
    // 2) 滑块条 → 拖控制点. 调基类 OnMouseDown 初始化 mMouseDown 等内部状态
    if (SliderRect().Contains(x, y))
    {
      const float lx = NormToX(LowNorm()), hx = NormToX(HighNorm());
      const float dL = std::fabs(x - lx), dR = std::fabs(x - hx);
      if (x > lx + 7.f && x < hx - 7.f) { mActiveHandle = 2; mStartX = x; mStartLow = LowNorm(); mStartHigh = HighNorm(); }
      else if (dL <= dR) mActiveHandle = 0;
      else mActiveHandle = 1;
      if (mHooks.gestureBegin) mHooks.gestureBegin();
      OnMouseDrag(x, y, 0.f, 0.f, mod);
      return;
    }
    // 3) 其余 → XY 节点拖拽 (限于绘图区). 调基类初始化 mMouseDown/hide-cursor
    //    (基类 OnMouseDown 会自动调 OnMouseDrag 完成首帧位置写入, 不需再手动 NodeDrag)
    if (PlotRect().Contains(x, y))
    {
      IVXYPadControl::OnMouseDown(x, y, mod);
    }
  }

  void OnMouseDrag(float x, float y, float dX, float dY, const IMouseMod& mod) override
  {
    if (mActiveHandle == 0 || mActiveHandle == 1)
    {
      float n = XToNorm(x);
      if (mActiveHandle == 0)
      {
        const float hN = HighNorm();
        n = std::clamp(n, 0.f, hN - kMinGap);
        mHooks.editBand((double) n, (double) hN);
      }
      else
      {
        const float lN = LowNorm();
        n = std::clamp(n, lN + kMinGap, 1.f);
        mHooks.editBand((double) lN, (double) n);
      }
    }
    else if (mActiveHandle == 2)
    {
      const float srx = SliderRect().W();
      const float d = (x - mStartX) / srx;
      float lN = std::clamp(mStartLow + d, 0.f, 1.f - kMinGap);
      float hN = std::clamp(mStartHigh + d, lN + kMinGap, 1.f);
      if (hN - lN < kMinGap) { hN = lN + kMinGap; if (hN > 1.f) { hN = 1.f; lN = 1.f - kMinGap; } }
      mHooks.editBand((double) lN, (double) hN);
    }
    else
    {
      NodeDrag(x, y);
    }
  }

  void OnMouseUp(float x, float y, const IMouseMod& mod) override
  {
    mActiveHandle = -1;
    IVXYPadControl::OnMouseUp(x, y, mod);
  }

  void OnMouseOver(float x, float y, const IMouseMod& mod) override
  {
    int hit = -1;
    for (int id : { kCornerCenter, kCornerBw, kCornerLow, kCornerHigh })
      if (CornerRect(id).Contains(x, y)) { hit = id; break; }
    if (hit != mOverCorner) { mOverCorner = hit; SetDirty(false); }
    IVXYPadControl::OnMouseOver(x, y, mod);
  }

  void OnMouseOut() override
  {
    if (mOverCorner != -1) { mOverCorner = -1; SetDirty(false); }
    IVXYPadControl::OnMouseOut();
  }

  void OnTextEntryCompletion(const char* str, int valIdx) override
  {
    // CreateTextEntry 用 kNoValIdx (-1) 调入; valIdx 实际就是 -1.
    // 用 mEditingCorner 决定分发到哪个角 (本控件只有 2 个 vals, 不能用 valIdx 4..7).
    const int id = mEditingCorner;
    mEditingCorner = -1;
    if (id < 0) return;
    double v;
    if (id == kCornerBw) { char* end = nullptr; v = std::strtod(str, &end); if (end == str) return; }
    else if (!ParseFreq(str, v)) return;
    if (mHooks.editCorner) mHooks.editCorner(id, v);
  }

  // 节点区域限定在绘图区 (基类用 mWidgetBounds 会越过滑块条)
  void DrawWidget(IGraphics& g) override
  {
    DrawTrack(g);
    const IRECT tb = PlotRect();
    const float xpos = (float) GetValue(0) * tb.W();
    const float ypos = (float) GetValue(1) * tb.H();
    const IRECT hb(tb.L + xpos - mHandleRadius, tb.B - ypos - mHandleRadius,
                   tb.L + xpos + mHandleRadius, tb.B - ypos + mHandleRadius);
    DrawHandle(g, tb, hb);
  }

  // 网格限定在绘图区 (滑块/角标之上)
  void DrawTrack(IGraphics& g) override
  {
    const IRECT tb = PlotRect();
    g.FillRect(COLOR_WHITE, tb);
    for (int i = 1; i < 4; ++i)
    {
      const float y = tb.T + tb.H() * i / 4.f;
      g.DrawLine(COL_GRID, tb.L, y, tb.R, y, nullptr, 1.f);
      const float x = tb.L + tb.W() * i / 4.f;
      g.DrawLine(COL_GRID, x, tb.T, x, tb.B, nullptr, 1.f);
    }
  }

  // 节点限定在绘图区
  void DrawHandle(IGraphics& g, const IRECT& trackBounds, const IRECT& handleBounds) override
  {
    const float cx = handleBounds.MW();
    const float cy = handleBounds.MH();
    const float r  = handleBounds.W() * 0.5f;
    g.DrawLine(COL_BLACK, cx, handleBounds.T, cx, cy - r, nullptr, 1.f);
    g.FillCircle(COL_BLACK, cx, cy, r);
    g.FillCircle(COLOR_WHITE, cx, cy, r * 0.25f);
  }

  // 重写命中判定: 角标/左侧标签在 widget 边界外, 也算本控件命中
  bool IsHit(float x, float y) const override
  {
    if (mTargetRECT.Contains(x, y)) return true;
    for (int id : { kCornerCenter, kCornerBw, kCornerLow, kCornerHigh })
      if (CornerRect(id).Contains(x, y)) return true;
    if (SideLabelRect().Contains(x, y)) return true;
    return false;
  }

  void DrawSideLabel(IGraphics& g)
  {
    if (mSideLabel.GetLength() == 0) return;
    const IRECT r = SideLabelRect();
    // 逆时针 90° (ccwise) → 文字自下而上阅读. mAngle 是 degrees ccwise
    IText t(11, COL_BLACK, "Outfit-SemiBold", EAlign::Center, EVAlign::Middle, -90.f);
    g.DrawText(t, mSideLabel.Get(), r);
  }

private:
  IRECT PlotRect() const
  {
    const IRECT& w = mWidgetBounds;
    const float top = w.T + kTopPad;
    // 滑块条紧贴 pad 框底边: 绘图区下沿 = 滑块条上沿
    const float srTop = w.B - kBottomInset - kSliderH;
    return IRECT(w.L, top, w.R, srTop);
  }

  IRECT SliderRect() const
  {
    const IRECT& w = mWidgetBounds;
    // 滑块条紧贴 pad 框底边 (仅留 kBottomInset 避免压到 2px 黑框)
    const float top = w.B - kBottomInset - kSliderH;
    return IRECT(w.L + 2.f, top, w.R - 2.f, top + kSliderH);
  }

  IRECT CornerRect(int id) const
  {
    const IRECT& w = mWidgetBounds;
    switch (id)
    {
      case kCornerCenter: return IRECT(w.L , w.T - kSideH, w.L + kCornerW, w.T + kCornerTextH);
      case kCornerBw:     return IRECT(w.R - kCornerW, w.T - kSideH, w.R, w.T + kCornerTextH);
      case kCornerLow:    return IRECT(w.L , w.B - kCornerTextH, w.L + kCornerW, w.B + kSideH);
      case kCornerHigh:   return IRECT(w.R - kCornerW, w.B - kCornerTextH, w.R, w.B + kSideH);
    }
    return IRECT();
  }

  // 左侧竖排 (LEFT/RIGHT) 标签区域: 位于 pad 框左外侧, 高度等同 pad
  IRECT SideLabelRect() const
  {
    const IRECT& w = mWidgetBounds;
    return IRECT(w.L - kSideW, w.T, w.L, w.B);
  }

  float NormToX(float norm) const { const IRECT s = SliderRect(); return s.L + norm * s.W(); }
  float XToNorm(float x) const { const IRECT s = SliderRect(); return std::clamp((x - s.L) / s.W(), 0.f, 1.f); }

  float LowNorm() const
  {
    const IParam* pf = GetParam(0);
    const double centerHz = pf->FromNormalized(GetValue(0));
    const double bw = GetParam(1)->FromNormalized(GetValue(1));
    return (float) pf->ToNormalized(centerHz * std::pow(2., -bw / 2.));
  }

  float HighNorm() const
  {
    const IParam* pf = GetParam(0);
    const double centerHz = pf->FromNormalized(GetValue(0));
    const double bw = GetParam(1)->FromNormalized(GetValue(1));
    return (float) pf->ToNormalized(centerHz * std::pow(2., bw / 2.));
  }

  void NodeDrag(float x, float y)
  {
    const IRECT p = PlotRect();
    x = std::clamp(x, p.L, p.R);
    y = std::clamp(y, p.T, p.B);
    const float xn = (x - p.L) / p.W();
    const float yn = 1.f - (y - p.T) / p.H();
    SetValue((double) xn, 0);
    SetValue((double) yn, 1);
    SetDirty(true);
  }

  void DrawSlider(IGraphics& g)
  {
    const IRECT s = SliderRect();
    const float y = s.MH();
    const float lx = NormToX(LowNorm());
    const float hx = NormToX(HighNorm());
    // 外侧浅灰轨道
    g.FillRect(COL_TRACK, IRECT(s.L, y - 2.f, s.R, y + 2.f));
    // 两控制点之间涂黑
    g.FillRect(COL_BLACK, IRECT(lx, y - 2.f, hx, y + 2.f));
    // 控制点: 白底黑描边圆
    for (float px : { lx, hx })
    {
      g.FillCircle(COLOR_WHITE, px, y, 7.f);
      g.DrawCircle(COL_BLACK, px, y, 7.f, nullptr, 1.5f);
    }
  }

  void DrawCorner(IGraphics& g, int id)
  {
    const IRECT r = CornerRect(id);
    if (mOverCorner == id)
      g.FillRoundRect(COL_HOVER, r.GetPadded(-2.f), 4.f);
    WDL_String label; GetCornerLabel(id, label);
    const EAlign align = (id == kCornerBw || id == kCornerHigh) ? EAlign::Far : EAlign::Near;
    const IText t(11, COL_BLACK, "Outfit-SemiBold", align, EVAlign::Middle);
    g.DrawText(t, label.Get(), r);
  }

  void GetCornerLabel(int id, WDL_String& out) const
  {
    const IParam* pf = GetParam(0);
    const double c = pf->FromNormalized(GetValue(0));
    const double bw = GetParam(1)->FromNormalized(GetValue(1));
    char buf[32];
    switch (id)
    {
      case kCornerCenter: FormatFreq(buf, 32, c);                       out.SetFormatted(64, "CENTER %s", buf); break;
      case kCornerBw:     std::snprintf(buf, 32, "%.2f", bw);            out.SetFormatted(64, "BANDWIDTH %s", buf); break;
      case kCornerLow:    FormatFreq(buf, 32, c * std::pow(2., -bw/2.)); out.SetFormatted(64, "LOWCUT %s", buf); break;
      case kCornerHigh:   FormatFreq(buf, 32, c * std::pow(2.,  bw/2.)); out.SetFormatted(64, "HIGHCUT %s", buf); break;
    }
  }

  static void FormatFreq(char* b, int n, double hz)
  {
    if (hz >= 10000.) std::snprintf(b, n, "%.1fk", hz / 1000.);
    else if (hz >= 1000.) std::snprintf(b, n, "%.2fk", hz / 1000.);
    else std::snprintf(b, n, "%.0f", hz);
  }

  static bool ParseFreq(const char* s, double& hz)
  {
    char* end = nullptr;
    const double v = std::strtod(s, &end);
    if (end == s) return false;
    while (*end && std::isspace((unsigned char)*end)) ++end;
    if (*end == 'k' || *end == 'K') hz = v * 1000.;
    else hz = v;
    return true;
  }

  static constexpr float kCornerW = 100.f;    // 角标文字水平宽度 (从 pad 框外侧向内延伸)
  static constexpr float kCornerTextH = 11.f; // 角标文字行高 (≈字号, 框内向外伸出的量)
  static constexpr float kSideW    = 22.f;    // 角标 / 竖排标签向外伸出量
  static constexpr float kSideH    = 25.f;    // 角标向上/下伸出量 (贴边)
  static constexpr float kTopPad   = 18.f;
  static constexpr float kSliderH  = 20.f;
  static constexpr float kBottomInset = 2.f;  // 滑块条与 pad 框底边的留白 (紧贴)
  static constexpr float kMinGap   = 0.01f; // 两控制点最小归一化间隔 (~2/3 半音)

  Hooks mHooks;
  WDL_String mSideLabel;       // 左侧竖排标签 (LEFT/RIGHT); 留空则不画
  int   mEditingCorner = -1;   // 当前正在行内编辑的角标 id (避免 valIdx 越界)
  int   mOverCorner = -1;
  int   mActiveHandle = -1; // 0=左(low) 1=右(high) 2=中间(整体平移)
  float mStartX = 0.f, mStartLow = 0.f, mStartHigh = 1.f;
};

END_IGRAPHICS_NAMESPACE
END_IPLUG_NAMESPACE
