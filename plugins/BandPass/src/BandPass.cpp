#include "BandPass.h"
#include "IPlug_include_in_plug_src.h"
#include "IControls.h"
#include "ICornerResizerControl.h"
#include "Theme.h"
#include "controls/FilterNodePad.h"
#include "controls/BandRangeSlider.h"
#include "controls/PresetSlotControl.h"
#include "PresetFileIO.h"

#include <cstring>
#include <cstdio>
#include <functional>
#include <chrono>
#include <cmath>
#include <algorithm>
#include <string>

#if defined(OS_MAC)
  #include <CoreFoundation/CoreFoundation.h>
#elif defined(OS_WIN)
  #include <windows.h>
#endif

int orm::DetectSystemLanguage()
{
#if defined(OS_MAC)
  bool zh = false;
  CFArrayRef langs = CFLocaleCopyPreferredLanguages();
  if (langs)
  {
    const CFIndex n = CFArrayGetCount(langs);
    for (CFIndex i = 0; i < n; ++i)
    {
      CFStringRef lang = (CFStringRef) CFArrayGetValueAtIndex(langs, i);
      char buf[64] = { 0 };
      if (lang && CFStringGetCString(lang, buf, sizeof(buf), kCFStringEncodingUTF8) &&
          std::strncmp(buf, "zh", 2) == 0)
      {
        zh = true;
        break;
      }
    }
    CFRelease(langs);
  }
  return zh ? orm::kLangZH : orm::kLangEN;
#elif defined(OS_WIN)
  const LANGID lid = GetUserDefaultUILanguage();
  if (PRIMARYLANGID(lid) == LANG_CHINESE)
    return orm::kLangZH;
  return orm::kLangEN;
#else
  return orm::kLangEN;
#endif
}

static IVStyle MakeORMStyle()
{
  IVColorSpec colors = { COL_100(), COL_100(), COL_900(), COL_900(),
                         COL_500(), COL_300(), COL_300(), COL_900(), COL_900() };
  const IText labelText(20, COL_700(), kFontRegular, EAlign::Center, EVAlign::Bottom);
  const IText valueText(20, COL_900(), kFontSemiBold, EAlign::Center, EVAlign::Top);
  return IVStyle(true, true, colors, labelText, valueText,
                 true, true, false, false, 0.2f, 1.5f, 0.f, 1.f, 0.f);
}

static IVStyle MakeButtonStyle()
{
  IVColorSpec colors = { COL_100(), COL_100(), COL_900(), COL_900(),
                         COL_500(), COL_300(), COL_300(), COL_900(), COL_900() };
  const IText labelText(20, COL_900(), kFontSemiBold, EAlign::Center, EVAlign::Middle);
  const IText valueText(20, COL_900(), kFontSemiBold, EAlign::Center, EVAlign::Middle);
  return IVStyle(true, true, colors, labelText, valueText, true, true, false, false,
                 0.f, 2.f, 0.f, 1.f, 0.f);
}

class ThemeCornerResizer : public ICornerResizerControl
{
public:
  ThemeCornerResizer(const IRECT& graphicsBounds)
  : ICornerResizerControl(graphicsBounds, 20.f) {}

  void Draw(IGraphics& g) override
  {
    const IColor col = mDragging        ? COL_700()
                     : GetMouseIsOver() ? COL_900()
                                        : COL_500();
    g.FillTriangle(col, mRECT.L, mRECT.B, mRECT.R, mRECT.T, mRECT.R, mRECT.B);
  }

  void OnMouseDown(float x, float y, const IMouseMod& mod) override
  {
    mDragging = true;
    ICornerResizerControl::OnMouseDown(x, y, mod);
  }

  void OnMouseUp(float x, float y, const IMouseMod& mod) override
  {
    mDragging = false;
    IControl::OnMouseUp(x, y, mod);
  }

private:
  bool mDragging = false;
};

class FlatActionButton : public IVButtonControl
{
public:
  FlatActionButton(const IRECT& bounds, IActionFunction aF, const char* label,
                   const IVStyle& style)
  : IVButtonControl(bounds, aF, label, style) {}

  void Draw(IGraphics& g) override
  {
    const IRECT b = GetWidgetBounds();
    const bool pressed = GetValue() > 0.5;
    const IColor fill = pressed ? COL_900()
                     : GetMouseIsOver() ? COL_500() : COL_300();
    g.FillRect(fill, b.GetPadded(-BLOCK_GAP));
    IText t = mStyle.valueText;
    t.mFGColor = pressed ? COL_100() : COL_900();
    strcpy(t.mFont, kFontSemiBold);
    g.DrawText(t, mLabelStr.Get(), b);
  }
};

static IVButtonControl* MakeMomentary(const IRECT& r,
                                       std::function<void(IControl*)> fn,
                                       const char* label, const IVStyle& st)
{
  return new FlatActionButton(r, [fn](IControl* p) {
    fn(p);
    p->SetValue(0.0);
    p->SetDirty(false);
  }, label, st);
}

static double BwMultToOct(double m) { return 2. * std::log2(m); }

class PresetFadeSlider : public IVSliderControl
{
public:
  PresetFadeSlider(const IRECT& bounds, IActionFunction aF, const IVStyle& style)
  : IVSliderControl(bounds, aF, "", style, false, EDirection::Horizontal)
  {
  }

  void Draw(IGraphics& g) override
  {
    g.FillRect(COL_100(), mRECT);
    DrawWidget(g);
  }

  void DrawTrack(IGraphics& g, const IRECT& filledArea) override
  {
    const float cr = GetRoundedCornerRadius(mTrackBounds);
    const IRECT tb = mTrackBounds.GetHPadded(mHandleSize);
    g.FillRoundRect(COL_300(), tb, cr, &mBlend);
    const IRECT fill(tb.L, filledArea.T, std::max(filledArea.R, mTrackBounds.L), filledArea.B);
    g.FillRoundRect(COL_500(), fill, cr, &mBlend);

    const float x0 = mTrackBounds.L, w = mTrackBounds.W();
    for (int i = 0; i < kNumQuick; ++i)
    {
      const float x = x0 + w * i / (kNumQuick - 1.f);
      g.FillRect(COL_500(), IRECT(x - 1.f, mTrackBounds.T - 3.f, x + 1.f, mTrackBounds.T));
      g.FillRect(COL_500(), IRECT(x - 1.f, mTrackBounds.B, x + 1.f, mTrackBounds.B + 3.f));
    }
  }
  void DrawHandle(IGraphics& g, const IRECT& bounds) override
  {
    const float cx = bounds.MW(), cy = bounds.MH();
    g.FillCircle(COL_100(), cx, cy, HANDLE_R + HANDLE_RING);
    g.FillCircle(COL_900(), cx, cy, HANDLE_R);
  }
};

class ORMSlider : public IVSliderControl
{
public:
  ORMSlider(const IRECT& bounds, int paramIdx, const char* label, const IVStyle& style,
            EDirection dir)
  : IVSliderControl(bounds, paramIdx, label, style, false, dir)
  , mHeaderLabel(label ? label : "")
  {
    mStyle.showLabel = false;
    mStyle.showValue = false;
  }

  ORMSlider(const IRECT& bounds, IActionFunction aF, const char* label, const IVStyle& style)
  : IVSliderControl(bounds, aF, label, style, false, EDirection::Horizontal)
  , mHeaderLabel(label ? label : "")
  {
    mStyle.showLabel = false;
    mStyle.showValue = false;
  }

  void SetValueFormatter(std::function<void(WDL_String&)> f) { mValueFormatter = std::move(f); }
  void SetHeaderLabel(const char* s) { mHeaderLabel.Set(s); SetDirty(false); }

  // Mono-output mode: only a washed-out slider track remains, no handle/text/swatch
  void SetGhost(bool ghost)
  {
    if (mGhost == ghost) return;
    mGhost = ghost;
    SetDirty(false);
  }

  struct AgHooks
  {
    std::function<void()> agToggle;
    std::function<void(int colorIdx)> agSetColor;
  };

  void SetHeaderSwatchColor(int colorIdx) { mHeaderSwatchColor = colorIdx; SetDirty(false); }
  void SetHeaderFont(const char* font) { mHeaderFont = font; SetDirty(false); }
  void SetAgMapHooks(const AgHooks& h) { mAgHooks = h; }
  void SetAgMapState(bool on, int colorIdx)
  {
    mAgMapOn = on;
    mAgMapColor = colorIdx;
    UpdateAgGhost();
    SetDirty(false);
  }
  void SetAgDeltaMix(float d)
  {
    if (std::fabs(d - mAgMixDelta) < 1e-4f) return;
    mAgMixDelta = d;
    UpdateAgGhost();
    SetDirty(false);
  }

  void OnResize() override
  {

    if (mDirection == EDirection::Horizontal)
    {
      mWidgetBounds = mRECT.GetReducedFromTop(kHeaderH);
      mTrackBounds  = mWidgetBounds.GetPadded(-mHandleSize)
                                   .GetMidVPadded(mTrackSize);
    }
    else
    {
      mWidgetBounds = mRECT.GetReducedFromLeft(kHeaderW);
      mTrackBounds  = mWidgetBounds.GetPadded(-mHandleSize)
                                   .GetMidHPadded(mTrackSize);
    }
    SetTargetRECT(mRECT);
    mValueBounds = IRECT();
    SetDirty(false);
  }

  bool IsHit(float x, float y) const override
  {
    return mRECT.Contains(x, y);
  }

  void Draw(IGraphics& g) override
  {
    g.FillRect(COL_100(), mRECT);
    DrawWidget(g);
    if (mGhost)
    {
      const IColor base = COL_100();
      g.FillRect(IColor(150, base.R, base.G, base.B), mRECT);
      return;
    }
    DrawHeader(g, mDirection == EDirection::Vertical ? -90.f : 0.f);
  }

  void OnMouseDown(float x, float y, const IMouseMod& mod) override
  {
    if (mGhost) return;
    if (mAgHooks.agToggle && mAgSwatchRect.Contains(x, y))
    {
      if (mod.R) OpenAgColorMenu();
      else mAgHooks.agToggle();
      return;
    }
    if (mod.R)
      return;
    if (mod.L && !mod.A && ValueRect().Contains(x, y))
    {
      if (GetParam())
        PromptUserInput(ValueRect());
      return;
    }
    IVSliderControl::OnMouseDown(x, y, mod);
  }

  void OnMouseDblClick(float x, float y, const IMouseMod& mod) override
  {
    if (mGhost) return;
    IVSliderControl::OnMouseDblClick(x, y, mod);
  }

  void DrawTrack(IGraphics& g, const IRECT& filledArea) override
  {
    const bool horiz = (mDirection == EDirection::Horizontal);
    const float cr = GetRoundedCornerRadius(mTrackBounds);
    const IRECT tb = horiz ? mTrackBounds.GetHPadded(mHandleSize)
                           : mTrackBounds.GetVPadded(mHandleSize);
    g.FillRoundRect(COL_300(), tb, cr, &mBlend);
    const IRECT fill = horiz
      ? IRECT(tb.L, filledArea.T, std::max(filledArea.R, mTrackBounds.L), filledArea.B)
      : IRECT(filledArea.L, filledArea.T, filledArea.R, tb.B);
    g.FillRoundRect(COL_500(), fill, cr, &mBlend);
  }

  void DrawHandle(IGraphics& g, const IRECT& bounds) override
  {
    if (mGhost) return;
    if (mAgMapOn && mAgGhostNorm >= 0.f)
    {
      const IRECT tb = mTrackBounds;
      if (mDirection == EDirection::Horizontal)
      {
        const float x = std::clamp(tb.L + mAgGhostNorm * tb.W(), tb.L, tb.R);
        g.FillCircle(COL_100(), x, tb.MH(), HANDLE_R + HANDLE_RING);
        g.FillCircle(AgColorGhost(mAgMapColor), x, tb.MH(), HANDLE_R);
      }
      else
      {
        const float y = std::clamp(tb.B - mAgGhostNorm * tb.H(), tb.T, tb.B);
        g.FillCircle(COL_100(), tb.MW(), y, HANDLE_R + HANDLE_RING);
        g.FillCircle(AgColorGhost(mAgMapColor), tb.MW(), y, HANDLE_R);
      }
    }
    const float cx = bounds.MW(), cy = bounds.MH();
    g.FillCircle(COL_100(), cx, cy, HANDLE_R + HANDLE_RING);
    g.FillCircle(COL_900(), cx, cy, HANDLE_R);
  }

protected:
  static constexpr float kHeaderH = 26.f;
  static constexpr float kHeaderW = 26.f;

  // Normalized ghost-handle position for the active random mapping (-1 = hidden).
  void UpdateAgGhost()
  {
    const IParam* p = GetParam();
    if (!mAgMapOn || !p) { mAgGhostNorm = -1.f; return; }
    const double v = std::clamp(p->Value() + GhostDelta(), p->GetMin(), p->GetMax());
    mAgGhostNorm = (float) p->ToNormalized(v);
  }
  virtual double GhostDelta() const { return (double) mAgMixDelta; }

  virtual IRECT ValueRect() const
  {
    if (mDirection == EDirection::Horizontal)
      return IRECT(mRECT.L, mRECT.T, mRECT.R, mRECT.T + kHeaderH);
    return IRECT(mRECT.L, mRECT.T, mRECT.L + kHeaderW + 4.f, mRECT.T + 44.f);
  }

  virtual void FormatValue(WDL_String& ds) const
  {
    ds.Set("");
    if (mValueFormatter)
    {
      mValueFormatter(ds);
      return;
    }
    const IParam* p = GetParam();
    if (!p) return;
    char buf[32];
    switch (GetParamIdx())
    {
      case kAgAmount:
      case kAgAmountY:
      case kAgAmountB:
      case kAgAmountG:
        std::snprintf(buf, sizeof(buf), "%.0f%%", p->Value() * 100.);
        ds.Set(buf);
        break;
      case kMix:
      {
        double v = p->Value();
        if (mAgMapOn) v = std::clamp(v + (double) mAgMixDelta, 0., 1.);
        std::snprintf(buf, sizeof(buf), "%.0f%%", v * 100.);
        ds.Set(buf);
        break;
      }
      case kAgRate:
      case kAgRateY:
      case kAgRateB:
      case kAgRateG:
        std::snprintf(buf, sizeof(buf), p->Value() < 10. ? "%.2fs" : "%.1fs", p->Value());
        ds.Set(buf);
        break;
      case kGainL:
      case kGainR:
        std::snprintf(buf, sizeof(buf), "%.1fdB", p->Value());
        ds.Set(buf);
        break;
      default:
        p->GetDisplay(ds, false);
        break;
    }
  }

  virtual void DrawHeader(IGraphics& g, float rot)
  {
    WDL_String ds;
    FormatValue(ds);

    if (rot == 0.f)
    {
      const IRECT hdr(mRECT.L, mRECT.T, mRECT.R, mRECT.T + kHeaderH);
      float labelL = hdr.L;
      if (mHeaderSwatchColor >= 0)
      {
        g.FillRect(AgColor(mHeaderSwatchColor),
                   IRECT(hdr.L + 1.f, hdr.MH() - AG_SWATCH * 0.5f,
                         hdr.L + 1.f + AG_SWATCH, hdr.MH() + AG_SWATCH * 0.5f));
        labelL = hdr.L + AG_SWATCH + AG_SWATCH_GAP;
      }
      float valueR = hdr.R;
      if (mAgHooks.agToggle)
      {
        mAgSwatchRect = IRECT(hdr.R - 1.f - AG_SWATCH, hdr.MH() - AG_SWATCH * 0.5f,
                              hdr.R - 1.f, hdr.MH() + AG_SWATCH * 0.5f);
        g.FillRect(mAgMapOn ? AgColor(mAgMapColor) : AgColorDim(mAgMapColor),
                   mAgSwatchRect.GetPadded(-1.f));
        valueR = mAgSwatchRect.L - 6.f;
      }
      g.DrawText(IText(20, COL_900(), mHeaderFont, EAlign::Near, EVAlign::Middle),
                 mHeaderLabel.Get(), IRECT(labelL, hdr.T, hdr.MW(), hdr.B));
      g.DrawText(IText(20, COL_700(), kFontRegular, EAlign::Far, EVAlign::Middle),
                 ds.Get(), IRECT(hdr.MW(), hdr.T, valueR, hdr.B));
    }
    else
    {
      const IRECT hdr(mRECT.L, mRECT.T, mRECT.L + kHeaderW, mRECT.B);
      g.DrawText(IText(20, COL_900(), kFontSemiBold, EAlign::Near, EVAlign::Bottom, rot),
                 mHeaderLabel.Get(), hdr);
      g.DrawText(IText(20, COL_700(), kFontRegular, EAlign::Far, EVAlign::Top, rot),
                 ds.Get(), hdr);
    }
  }

  void OpenAgColorMenu()
  {
    if (!GetUI()) return;
    mAgMenu.Clear();
    mAgMenu.SetFunction([this](IPopupMenu* menu) {
      const int idx = menu ? menu->GetChosenItemIdx() : -1;
      if (idx < 0 || idx >= kNumAgColors) return;
      if (mAgHooks.agSetColor) mAgHooks.agSetColor(idx);
    });
    static const int kNameIds[kNumAgColors] = { orm::kTxtRed, orm::kTxtYellow, orm::kTxtBlue, orm::kTxtGreen };
    for (int c = 0; c < kNumAgColors; ++c)
      mAgMenu.AddItem(orm::Tr(kNameIds[c], orm::UILang()));
    mAgMenu.CheckItemAlone(std::clamp(mAgMapColor, 0, kNumAgColors - 1));
    GetUI()->CreatePopupMenu(*this, mAgMenu, mAgSwatchRect, kNoValIdx);
  }

  WDL_String mHeaderLabel;
  const char* mHeaderFont = kFontSemiBold;
  std::function<void(WDL_String&)> mValueFormatter;
  bool mGhost = false;
  int mHeaderSwatchColor = -1;
  bool mAgMapOn = false;
  int mAgMapColor = 0;
  IRECT mAgSwatchRect;
  IPopupMenu mAgMenu;
  AgHooks mAgHooks;
  float mAgGhostNorm = -1.f;
  float mAgMixDelta = 0.f;
};

class GainSlider : public ORMSlider
{
public:
  GainSlider(const IRECT& bounds, int paramIdx, const char* label, const IVStyle& style)
  : ORMSlider(bounds, paramIdx, label, style, EDirection::Vertical) {}

  void SetAgDeltaDb(float db)
  {
    if (std::fabs(db - mAgGainDb) < 1e-3f) return;
    mAgGainDb = db;
    UpdateAgGhost();
    SetDirty(false);
  }

protected:
  static constexpr float kTextW = 16.f;
  static constexpr float kTextGap = 6.f;
  static constexpr float kPlotTopInset = 30.f;

  double GhostDelta() const override { return (double) mAgGainDb; }

  void FormatValue(WDL_String& ds) const override
  {
    const IParam* p = GetParam();
    if (!p) { ds.Set(""); return; }
    double v = p->Value();
    if (mAgMapOn) v = std::clamp(v + (double) mAgGainDb, p->GetMin(), p->GetMax());
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1fdB", v);
    ds.Set(buf);
  }

  IRECT TextRect() const
  {
    return IRECT(mRECT.R - kTextW - kTextGap, mRECT.T, mRECT.R - kTextGap, mRECT.B);
  }

  float TrackVisTop() const { return mRECT.T + kPlotTopInset; }

  void OnResize() override
  {
    mWidgetBounds = mRECT.GetReducedFromRight(kTextW + kTextGap);
    mTrackBounds  = mWidgetBounds.GetReducedFromTop(kPlotTopInset + mHandleSize)
                                 .GetReducedFromBottom(mHandleSize)
                                 .GetMidHPadded(mTrackSize);
    SetTargetRECT(mRECT);
    mValueBounds = IRECT();
    SetDirty(false);
  }

  IRECT ValueRect() const override
  {
    const IRECT hdr = TextRect();
    return IRECT(hdr.L - 4.f, TrackVisTop(), hdr.R, hdr.MH());
  }

  void DrawHeader(IGraphics& g, float) override
  {
    WDL_String ds;
    FormatValue(ds);

    const IRECT hdr = TextRect();
    mAgSwatchRect = IRECT(hdr.MW() - AG_SWATCH * 0.5f, mRECT.T + kPlotTopInset * 0.5f - AG_SWATCH * 0.5f,
                          hdr.MW() + AG_SWATCH * 0.5f, mRECT.T + kPlotTopInset * 0.5f + AG_SWATCH * 0.5f);
    g.FillRect(mAgMapOn ? AgColor(mAgMapColor) : AgColorDim(mAgMapColor),
               mAgSwatchRect.GetPadded(-1.f));
    g.DrawText(IText(20, COL_700(), kFontRegular, EAlign::Center, EVAlign::Top, 90.f),
               ds.Get(), IRECT(hdr.L, TrackVisTop(), hdr.R, hdr.B));
    g.DrawText(IText(20, COL_900(), kFontSemiBold, EAlign::Center, EVAlign::Bottom, 90.f),
               mHeaderLabel.Get(), hdr);
  }

  float mAgGainDb = 0.f;
};

class InvertToggleControl : public IVToggleControl
{
public:
  InvertToggleControl(const IRECT& bounds, int paramIdx, const char* label,
                      const IVStyle& style, const char* offText, const char* onText)
  : IVToggleControl(bounds, paramIdx, label, style, offText, onText)
  {
    SetActionFunction(EmptyClickActionFunc);
  }

  void SetOnText(const char* s)  { mOnText.Set(s); SetDirty(false); }
  void SetOffText(const char* s) { mOffText.Set(s); SetDirty(false); }

  void DrawValue(IGraphics& g, bool) override
  {
    const bool on = GetValue() > 0.5;
    IText t = mStyle.valueText;
    t.mFGColor = on ? COL_100() : COL_900();
    strcpy(t.mFont, kFontSemiBold);
    g.DrawText(t, on ? mOnText.Get() : mOffText.Get(), mWidgetBounds, &mBlend);
  }
};

class FlatToggleControl : public InvertToggleControl
{
public:
  using InvertToggleControl::InvertToggleControl;

  void Draw(IGraphics& g) override
  {
    const IRECT b = GetWidgetBounds();
    const bool on = GetValue() > 0.5;
    const IColor fill = on ? COL_900()
                     : GetMouseIsOver() ? COL_500() : COL_300();
    g.FillRect(fill, b.GetPadded(-BLOCK_GAP));
    DrawValue(g, false);
  }
};

class SectionTitleControl : public ITextControl
{
public:
  SectionTitleControl(const IRECT& bounds, const char* str, const IText& text, int colorStep = 0, int fontStep = 2)
  : ITextControl(bounds, str, text), mColorStep(colorStep), mFontStep(fontStep) {}

  void Draw(IGraphics& g) override
  {
    IText t = mText;
    t.mFGColor = mColorStep == 0 ? COL_900() : COL_500();
    strcpy(t.mFont, mFontStep == 0 ? kFontRegular : mFontStep == 1 ? kFontSemiBold : kFontBold);
    g.DrawText(t, mStr.Get(), mRECT, &mBlend);
  }

private:
  int mColorStep = 0;
  int mFontStep = 2;
};

class AgColorPickerControl : public IControl
{
public:
  AgColorPickerControl(const IRECT& bounds, std::function<void(int)> onPick)
  : IControl(bounds), mOnPick(std::move(onPick)) {}

  void SetColor(int idx) { mColorIdx = std::clamp(idx, 0, kNumAgColors - 1); SetDirty(false); }

  void Draw(IGraphics& g) override
  {
    g.FillRect(AgColor(mColorIdx), mRECT.GetPadded(-BLOCK_GAP));
  }

  void OnMouseDown(float x, float y, const IMouseMod& mod) override
  {
    if (!GetUI() || !mOnPick) return;
    mMenu.Clear();
    mMenu.SetFunction([this](IPopupMenu* menu) {
      const int idx = menu ? menu->GetChosenItemIdx() : -1;
      if (idx < 0 || idx >= kNumAgColors) return;
      mOnPick(idx);
    });
    static const int kNameIds[kNumAgColors] = { orm::kTxtRed, orm::kTxtYellow, orm::kTxtBlue, orm::kTxtGreen };
    for (int c = 0; c < kNumAgColors; ++c)
      mMenu.AddItem(orm::Tr(kNameIds[c], orm::UILang()));
    mMenu.CheckItemAlone(mColorIdx);
    GetUI()->CreatePopupMenu(*this, mMenu, mRECT, kNoValIdx);
  }

private:
  std::function<void(int)> mOnPick;
  IPopupMenu mMenu;
  int mColorIdx = 0;
};

class SettingsMenuButton : public IControl
{
public:
  SettingsMenuButton(const IRECT& bounds, std::function<void()> onToggle)
  : IControl(bounds), mOnToggle(std::move(onToggle)) {}

  void Draw(IGraphics& g) override
  {
    const float cx = mRECT.MW(), cy = mRECT.MH();
    const float r = (mRECT.W() * 0.5f - 2.f) * 0.8f;
    const IColor col = GetMouseIsOver() ? COL_900() : COL_700();

    g.PathClear();
    g.PathTransformReset();
    g.PathTransformTranslate(cx, cy);
    g.PathCircle(0.f, 0.f, r * 0.72f);
    for (int i = 0; i < 8; ++i)
    {
      g.PathTransformReset();
      g.PathTransformTranslate(cx, cy);
      g.PathTransformRotate(i * 45.f);
      g.PathRect(IRECT(-r * 0.17f, -r, r * 0.17f, -r * 0.70f));
    }
    g.PathFill(col);

    g.PathClear();
    g.PathTransformReset();
    g.PathTransformTranslate(cx, cy);
    g.PathCircle(0.f, 0.f, r * 0.30f);
    g.PathFill(COL_100());
  }

  void OnMouseDown(float x, float y, const IMouseMod& mod) override
  {
    if (mOnToggle) mOnToggle();
  }

private:
  std::function<void()> mOnToggle;
};

class SettingsPanelControl : public IControl
{
public:
  struct Hooks
  {
    std::function<void(int lang)> onLanguage;
    std::function<void(int themeMode)> onTheme;
    std::function<void(int hue)> onHue;
    std::function<void(int satMax)> onSat;
    // Audio device selection (standalone app only; empty in plug-in builds)
    std::function<std::vector<std::string>(bool input)> listAudioDevices;
    std::function<const char*(bool input)> currentAudioDevice;
    std::function<void(bool input, const char* name)> onAudioDevice;
  };

  SettingsPanelControl(const IRECT& bounds, Hooks hooks)
  : IControl(bounds)
  , mHooks(std::move(hooks))
  {
    mHasAudio = (bool) (mHooks.listAudioDevices && mHooks.currentAudioDevice && mHooks.onAudioDevice);
    const float cardH = mHasAudio ? kCardHAudio : kCardH;
    mCard = IRECT(bounds.MW() - kCardW * 0.5f, bounds.MH() - cardH * 0.5f,
                  bounds.MW() + kCardW * 0.5f, bounds.MH() + cardH * 0.5f);
    const float bw = (kCardW - 2.f * kPad - kBtnGap) * 0.5f;
    mLangBtns[0]  = IRECT(mCard.L + kPad, mCard.T + kLangBtnY, mCard.L + kPad + bw, mCard.T + kLangBtnY + kBtnH);
    mLangBtns[1]  = IRECT(mCard.L + kPad + bw + kBtnGap, mCard.T + kLangBtnY, mCard.L + kPad + 2.f * bw + kBtnGap, mCard.T + kLangBtnY + kBtnH);
    mThemeBtns[0] = IRECT(mCard.L + kPad, mCard.T + kThemeBtnY, mCard.L + kPad + bw, mCard.T + kThemeBtnY + kBtnH);
    mThemeBtns[1] = IRECT(mCard.L + kPad + bw + kBtnGap, mCard.T + kThemeBtnY, mCard.L + kPad + 2.f * bw + kBtnGap, mCard.T + kThemeBtnY + kBtnH);
    const float rowW = mCard.W() - 2.f * kPad;
    for (int i = 0; i < 2; ++i)
    {
      const float headerY = mCard.T + (i == 0 ? kHueTitleY : kSatTitleY);
      mSliderHeader[i] = IRECT(mCard.L + kPad, headerY, mCard.R - kPad, headerY + kHeaderH);
      const float trackY = mCard.T + (i == 0 ? kHueY : kSatY);
      mSliderTrack[i] = IRECT(mCard.L + kPad, trackY, mCard.L + kPad + rowW, trackY + kTrackH);
      const float devY = mCard.T + (i == 0 ? kAudioRow1Y : kAudioRow2Y);
      mAudioRow[i] = IRECT(mCard.L + kPad, devY, mCard.R - kPad, devY + kAudioRowH);
    }
    mHover = kHoverNone;
  }

  void Draw(IGraphics& g) override
  {
    g.FillRect(IColor(70, 26, 25, 22), mRECT);

    g.FillRect(COL_100(), mCard);
    g.DrawRect(COL_300(), mCard, &mBlend, 1.f);

    const int lang = orm::UILang();
    const int theme = ThemeMode();
    const float L = mCard.L + kPad;

    g.DrawText(IText(kTitleSize, COL_900(), kFontBold, EAlign::Near, EVAlign::Middle),
               orm::Tr(orm::kTxtLanguage, lang), IRECT(L, mCard.T + kLangTitleY, mCard.R - kPad, mCard.T + kLangTitleY + kTitleSize));
    g.DrawText(IText(kTitleSize, COL_900(), kFontBold, EAlign::Near, EVAlign::Middle),
               orm::Tr(orm::kTxtTheme, lang), IRECT(L, mCard.T + kThemeTitleY, mCard.R - kPad, mCard.T + kThemeTitleY + kTitleSize));

    DrawButton(g, mLangBtns[0],  orm::Tr(orm::kTxtChinese, lang), lang == orm::kLangZH, mHover == kHoverLangZh, kFontSemiBold);
    DrawButton(g, mLangBtns[1],  "ENGLISH", lang == orm::kLangEN, mHover == kHoverLangEn, kFontSemiBold);
    DrawButton(g, mThemeBtns[0], orm::Tr(orm::kTxtDark,  lang), theme == 1, mHover == kHoverThemeDark);
    DrawButton(g, mThemeBtns[1], orm::Tr(orm::kTxtLight, lang), theme == 0, mHover == kHoverThemeLight);

    DrawSliderHeader(g, mSliderHeader[0], orm::Tr(orm::kTxtHue, lang),        HueLabel(lang));
    DrawSlider(g, mSliderTrack[0], HueNorm());
    DrawSliderHeader(g, mSliderHeader[1], orm::Tr(orm::kTxtSaturation, lang), SatLabel(lang));
    DrawSlider(g, mSliderTrack[1], SatNorm());

    if (mHasAudio)
    {
      g.DrawText(IText(kTitleSize, COL_900(), kFontBold, EAlign::Near, EVAlign::Middle),
                 orm::Tr(orm::kTxtAudio, lang), IRECT(L, mCard.T + kAudioTitleY, mCard.R - kPad, mCard.T + kAudioTitleY + kTitleSize));
      for (int i = 0; i < 2; ++i)
        DrawDeviceRow(g, mAudioRow[i], orm::Tr(i == 0 ? orm::kTxtAudioInput : orm::kTxtAudioOutput, lang),
                      mHooks.currentAudioDevice(i == 0), mHover == (i == 0 ? kHoverAudioIn : kHoverAudioOut));
    }
  }

  void OnMouseDown(float x, float y, const IMouseMod& mod) override
  {
    if (!mCard.Contains(x, y))
    {
      SetVisible(false);
      return;
    }
    for (int i = 0; i < 2; ++i)
    {
      if (mLangBtns[i].Contains(x, y) && mHooks.onLanguage)
      {
        mHooks.onLanguage(i == 0 ? orm::kLangZH : orm::kLangEN);
        return;
      }
      if (mThemeBtns[i].Contains(x, y) && mHooks.onTheme)
      {
        mHooks.onTheme(i == 0 ? 1 : 0);
        return;
      }
    }
    if (mSliderTrack[0].Contains(x, y)) { mDrag = kDragHue; DragTo(x, y); return; }
    if (mSliderTrack[1].Contains(x, y)) { mDrag = kDragSat; DragTo(x, y); return; }
    if (mHasAudio)
    {
      if (mAudioRow[0].Contains(x, y)) { OpenDeviceMenu(true); return; }
      if (mAudioRow[1].Contains(x, y)) { OpenDeviceMenu(false); return; }
    }
  }

  void OnMouseDrag(float x, float y, float dX, float dY, const IMouseMod& mod) override
  {
    if (mDrag != kDragNone) DragTo(x, y);
  }

  void OnMouseUp(float x, float y, const IMouseMod& mod) override
  {
    mDrag = kDragNone;
  }

  void OnMouseOver(float x, float y, const IMouseMod& mod) override
  {
    const EHover h = HitHover(x, y);
    if (h != mHover) { mHover = h; SetDirty(false); }
    IControl::OnMouseOver(x, y, mod);
  }

  void OnMouseOut() override
  {
    if (mHover != kHoverNone) { mHover = kHoverNone; SetDirty(false); }
    IControl::OnMouseOut();
  }

  void SetVisible(bool show)
  {
    Hide(!show);
    SetDirty(false);
  }

private:
  enum EHover { kHoverNone, kHoverLangZh, kHoverLangEn, kHoverThemeDark, kHoverThemeLight, kHoverAudioIn, kHoverAudioOut };
  enum EDrag  { kDragNone, kDragHue, kDragSat };

  float HueNorm() const { const int h = ThemeHue(); return (h - kHueMin) / (float) (kHueMax - kHueMin); }
  float SatNorm() const
  {
    const int s = ThemeSatMax();
    int idx = 0;
    for (int i = 0; i < kNumSat; ++i) if (kSatVals[i] == s) { idx = i; break; }
    return idx / (float) (kNumSat - 1);
  }

  void DragTo(float x, float y)
  {
    const IRECT* s = (mDrag == kDragHue) ? &mSliderTrack[0] : &mSliderTrack[1];
    const float n = std::clamp((x - s->L) / s->W(), 0.f, 1.f);
    if (mDrag == kDragHue)
    {
      const int hue = kHueMin + (int) std::lround(n * (kHueMax - kHueMin) / kHueStep) * kHueStep;
      if (hue != ThemeHue()) { ThemeHue() = hue; if (mHooks.onHue) mHooks.onHue(hue); }
    }
    else
    {
      const int idx = (int) std::lround(n * (kNumSat - 1));
      const int sat = kSatVals[idx];
      if (sat != ThemeSatMax()) { ThemeSatMax() = sat; if (mHooks.onSat) mHooks.onSat(sat); }
    }
    SetDirty(false);
  }

  EHover HitHover(float x, float y) const
  {
    if (mLangBtns[0].Contains(x, y)) return kHoverLangZh;
    if (mLangBtns[1].Contains(x, y)) return kHoverLangEn;
    if (mThemeBtns[0].Contains(x, y)) return kHoverThemeDark;
    if (mThemeBtns[1].Contains(x, y)) return kHoverThemeLight;
    if (mHasAudio && mAudioRow[0].Contains(x, y)) return kHoverAudioIn;
    if (mHasAudio && mAudioRow[1].Contains(x, y)) return kHoverAudioOut;
    return kHoverNone;
  }

  void DrawButton(IGraphics& g, const IRECT& b, const char* label, bool active, bool hover,
                  const char* font = nullptr)
  {
    const IColor fill = active ? COL_900()
                       : hover   ? COL_500()
                                 : COL_300();
    g.FillRect(fill, b);
    const IColor fg = active ? COL_100() : COL_900();
    g.DrawText(IText(16, fg, font ? font : kFontSemiBold, EAlign::Center, EVAlign::Middle), label, b);
  }

  void DrawSliderHeader(IGraphics& g, const IRECT& hdr, const char* title, const char* value)
  {
    g.DrawText(IText(kHeaderFontSize, COL_900(), kFontSemiBold, EAlign::Near, EVAlign::Middle),
               title, IRECT(hdr.L, hdr.T, hdr.MW(), hdr.B));
    g.DrawText(IText(kHeaderFontSize, COL_700(), kFontRegular, EAlign::Far, EVAlign::Middle),
               value, IRECT(hdr.MW(), hdr.T, hdr.R, hdr.B));
  }

  void DrawSlider(IGraphics& g, const IRECT& s, float norm)
  {
    const float y = s.MH();
    const float x = s.L + norm * s.W();
    g.FillRect(COL_300(), IRECT(s.L, y - 2.f, s.R, y + 2.f));
    g.FillRect(COL_500(), IRECT(s.L, y - 2.f, x, y + 2.f));
    g.FillCircle(COL_100(), x, y, HANDLE_R + HANDLE_RING);
    g.FillCircle(COL_900(), x, y, HANDLE_R);
  }

  const char* HueLabel(int lang) const
  {
    static char buf[16];
    std::snprintf(buf, sizeof(buf), "%d\xC2\xB0", ThemeHue());
    (void) lang;
    return buf;
  }

  const char* SatLabel(int lang) const
  {
    static const int kIds[kNumSat] = { orm::kTxtSatNone, orm::kTxtSatLow, orm::kTxtSatMed, orm::kTxtSatHigh };
    int idx = 0;
    for (int i = 0; i < kNumSat; ++i) if (kSatVals[i] == ThemeSatMax()) { idx = i; break; }
    return orm::Tr(kIds[idx], lang);
  }

  void OpenDeviceMenu(bool isInput)
  {
    if (!GetUI() || !mHooks.onAudioDevice) return;
    const std::vector<std::string> names = mHooks.listAudioDevices(isInput);
    const char* current = mHooks.currentAudioDevice(isInput);
    mMenu.Clear();
    mMenu.SetFunction([this, isInput, names](IPopupMenu* menu) {
      const int idx = menu ? menu->GetChosenItemIdx() : -1;
      if (idx < 0 || idx >= (int) names.size()) return;
      mHooks.onAudioDevice(isInput, names[idx].c_str());
      SetDirty(false);
    });
    for (const std::string& n : names)
      mMenu.AddItem(n.c_str());
    for (int i = 0; i < (int) names.size(); ++i)
      if (names[i] == current) { mMenu.CheckItemAlone(i); break; }
    GetUI()->CreatePopupMenu(*this, mMenu, isInput ? mAudioRow[0] : mAudioRow[1], kNoValIdx);
  }

  void DrawDeviceRow(IGraphics& g, const IRECT& r, const char* label, const char* device, bool hover)
  {
    if (hover)
      g.FillRect(COL_300(), r);
    const IText labelTxt(16, COL_900(), kFontSemiBold, EAlign::Near, EVAlign::Middle);
    g.DrawText(labelTxt, label, r);
    IRECT labelBox = r;
    g.MeasureText(labelTxt, label, labelBox);
    const IText valTxt(16, COL_700(), kFontRegular, EAlign::Far, EVAlign::Middle);
    const IRECT valRect(labelBox.R + 12.f, r.T, r.R, r.B);
    WDL_String fitted;
    FitText(g, valTxt, device, valRect.W(), fitted);
    g.DrawText(valTxt, fitted.Get(), valRect);
  }

  static void FitText(IGraphics& g, const IText& t, const char* str, float maxW, WDL_String& out)
  {
    out.Set(str);
    IRECT m;
    g.MeasureText(t, out.Get(), m);
    if (m.W() <= maxW) return;
    int len = out.GetLength();
    while (len > 0)
    {
      // drop the trailing UTF-8 code point, then retry with an ellipsis
      do { --len; } while (len > 0 && ((unsigned char) out.Get()[len] & 0xC0) == 0x80);
      out.SetLen(len);
      out.Append("\xE2\x80\xA6");
      g.MeasureText(t, out.Get(), m);
      if (m.W() <= maxW) return;
      out.SetLen(len);
    }
  }

  static constexpr float kCardW = 380.f;
  static constexpr float kCardH = 270.f;
  static constexpr float kCardHAudio = 360.f;
  static constexpr float kPad = 20.f;
  static constexpr float kBtnH = 30.f;
  static constexpr float kBtnGap = 0.f;
  static constexpr float kTitleSize = 20.f;
  static constexpr float kHeaderFontSize = 20.f;
  static constexpr float kHeaderH = 26.f;
  static constexpr float kTrackH = 26.f;
  static constexpr float kLangTitleY = 14.f;
  static constexpr float kLangBtnY = 38.f;
  static constexpr float kThemeTitleY = 80.f;
  static constexpr float kThemeBtnY = 104.f;
  static constexpr float kHueTitleY = 148.f;
  static constexpr float kHueY = 174.f;
  static constexpr float kSatTitleY = 206.f;
  static constexpr float kSatY = 232.f;
  static constexpr float kAudioTitleY = 262.f;
  static constexpr float kAudioRow1Y = 288.f;
  static constexpr float kAudioRow2Y = 320.f;
  static constexpr float kAudioRowH = 28.f;

  static constexpr int kHueMin = 15;
  static constexpr int kHueMax = 360;
  static constexpr int kHueStep = 15;
  static constexpr int kSatMin = 0;
  static constexpr int kSatMax = 50;
  static constexpr int kNumSat = 4;
  static constexpr int kSatVals[kNumSat] = { 0, 15, 30, 50 };

  Hooks mHooks;
  IRECT mCard;
  IRECT mLangBtns[2];
  IRECT mThemeBtns[2];
  IRECT mSliderHeader[2];
  IRECT mSliderTrack[2];
  IRECT mAudioRow[2];
  IPopupMenu mMenu;
  bool mHasAudio = false;
  EHover mHover = kHoverNone;
  EDrag mDrag = kDragNone;
};

ORMBandPass::ORMBandPass(const InstanceInfo& info)
: Plugin(info, MakeConfig(kNumParams, 1))
{
  GetParam(kFreqL)->InitDouble("FreqL", std::sqrt(300. * 4000.), 20., 20000., 0.01, "Hz", 0, "", IParam::ShapeExp());
  GetParam(kBwL)  ->InitDouble("BW L", 3.65, 1., 31., 0.01, "x", 0, "", IParam::ShapeExp());
  GetParam(kGainL)->InitDouble("Gain L", 0., -96., 12., 0.01, "");
  GetParam(kFreqR)->InitDouble("FreqR", std::sqrt(300. * 4000.), 20., 20000., 0.01, "Hz", 0, "", IParam::ShapeExp());
  GetParam(kBwR)  ->InitDouble("BW R", 3.65, 1., 31., 0.01, "x", 0, "", IParam::ShapeExp());
  GetParam(kGainR)->InitDouble("Gain R", 0., -96., 12., 0.01, "");
  GetParam(kLink) ->InitBool("Link", false);
  GetParam(kMix)  ->InitDouble("Mix", 1., 0., 1., 0.01, "");
  GetParam(kAgOn) ->InitBool("Agitation", false);
  GetParam(kAgAmount)->InitDouble("Ag Amount", 0.5, 0., 1., 0.01, "");
  GetParam(kAgRate)->InitDouble("Ag Speed", 0.5, 0.01, 60., 0.01, "", 0, "", IParam::ShapeExp());
  GetParam(kSlopeL)->InitEnum("Slope L", kSlopeDefaultIdx,
    { "12 dB/oct", "24 dB/oct", "48 dB/oct", "96 dB/oct" });
  GetParam(kSlopeR)->InitEnum("Slope R", kSlopeDefaultIdx,
    { "12 dB/oct", "24 dB/oct", "48 dB/oct", "96 dB/oct" });
  {
    struct MapDef { int colorIdx; int enableIdx; const char* colorName; const char* enableName; };
    const MapDef maps[7] = {
      { kAgColorFreqL, kAgEnableFreqL, "Rand Color Freq L", "Rand Freq L" },
      { kAgColorBwL,   kAgEnableBwL,   "Rand Color BW L",   "Rand BW L" },
      { kAgColorGainL, kAgEnableGainL, "Rand Color Gain L", "Rand Gain L" },
      { kAgColorFreqR, kAgEnableFreqR, "Rand Color Freq R", "Rand Freq R" },
      { kAgColorBwR,   kAgEnableBwR,   "Rand Color BW R",   "Rand BW R" },
      { kAgColorGainR, kAgEnableGainR, "Rand Color Gain R", "Rand Gain R" },
      { kAgColorMix,   kAgEnableMix,   "Rand Color Mix",    "Rand Mix" },
    };
    for (const MapDef& m : maps)
    {
      GetParam(m.colorIdx)->InitEnum(m.colorName, 0, { "Red", "Yellow", "Blue", "Green" });
      GetParam(m.enableIdx)->InitBool(m.enableName, false);
    }
  }
  {
    struct RateDef { int amountIdx; int rateIdx; const char* amountName; const char* rateName; };
    const RateDef rates[3] = {
      { kAgAmountY, kAgRateY, "Ag Amount Yellow", "Ag Speed Yellow" },
      { kAgAmountB, kAgRateB, "Ag Amount Blue",   "Ag Speed Blue" },
      { kAgAmountG, kAgRateG, "Ag Amount Green",  "Ag Speed Green" },
    };
    const double kDefaultAmount[3] = { 0.3, 0.6, 0.4 };  // Yellow 30%, Blue 60%, Green 40%
    const double kDefaultRate[3]   = { 1.0, 0.75, 2.0 }; // Yellow 1.0s, Blue 0.75s, Green 2.0s
    for (int i = 0; i < 3; ++i)
    {
      const RateDef& r = rates[i];
      GetParam(r.amountIdx)->InitDouble(r.amountName, kDefaultAmount[i], 0., 1., 0.01, "");
      GetParam(r.rateIdx)->InitDouble(r.rateName, kDefaultRate[i], 0.01, 60., 0.01, "", 0, "", IParam::ShapeExp());
    }
  }

  for (int i = 0; i < kNumPresets; ++i)
  {
    mPresets[i] = Snapshot();
    mSlotNumber[i] = i;
  }

  mDefaultSnapshot = Snapshot();
  mStableSnapshot  = Snapshot();

  auto setBand = [](ParamSnapshot& s, double lowL, double highL,
                    double lowR, double highR)
  {
    s[kFreqL] = std::sqrt(lowL * highL);
    s[kBwL]   = std::sqrt(highL / lowL);
    s[kFreqR] = std::sqrt(lowR * highR);
    s[kBwR]   = std::sqrt(highR / lowR);
  };

  {
    ParamSnapshot s = Snapshot();
    setBand(s, 300., 4000., 300., 4000.);            // 1
    mPresets[0] = s;
  }
  {
    ParamSnapshot s = Snapshot();
    setBand(s, 23., 200., 23., 200.);                // 2
    mPresets[1] = s;
  }
  {
    ParamSnapshot s = Snapshot();
    setBand(s, 23., 1000., 1000., 22050.);           // 3
    mPresets[2] = s;
  }
  {
    ParamSnapshot s = Snapshot();
    setBand(s, 1000., 22050., 23., 1000.);           // 4
    mPresets[3] = s;
  }
  {
    ParamSnapshot s = Snapshot();
    setBand(s, 5000., 22050., 5000., 22050.);        // 5
    mPresets[4] = s;
  }
  {
    ParamSnapshot s = Snapshot();
    setBand(s, 400., 4000., 400., 4000.);            // 6
    mPresets[5] = s;
  }
  {
    ParamSnapshot s = Snapshot();
    setBand(s, 300., 300., 300., 300.);              // 7
    mPresets[6] = s;
  }
  {
    ParamSnapshot s = Snapshot();
    setBand(s, 6000., 6000., 6000., 6000.);          // 8
    mPresets[7] = s;
  }
  {
    ParamSnapshot s = Snapshot();
    setBand(s, 6102., 22050., 6102., 22050.);        // 9
    mPresets[8] = s;
  }
  {
    ParamSnapshot s = Snapshot();
    setBand(s, 467., 557., 467., 557.);              // 10
    mPresets[9] = s;
  }
  {
    ParamSnapshot s = Snapshot();
    setBand(s, 77., 5626., 77., 5626.);              // 11
    mPresets[10] = s;
  }
  {
    ParamSnapshot s = Snapshot();
    setBand(s, 7784., 9155., 7784., 9155.);          // 12
    mPresets[11] = s;
  }
  {
    ParamSnapshot s = Snapshot();
    setBand(s, 4982., 12327., 4982., 12327.);        // 13
    mPresets[12] = s;
  }
  {
    ParamSnapshot s = Snapshot();
    setBand(s, 254., 329., 254., 329.);              // 14
    mPresets[13] = s;
  }

  // Agitation and Link are always off in the factory presets.
  for (int i = 0; i < kNumPresets; ++i)
  {
    mPresets[i][kAgOn] = 0.;
    mPresets[i][kLink] = 0.;
  }

#if IPLUG_EDITOR
  mMakeGraphicsFunc = [&]()
  {
    return MakeGraphics(*this, PLUG_WIDTH, PLUG_HEIGHT, PLUG_FPS,
                        GetScaleForScreen(PLUG_WIDTH, PLUG_HEIGHT));
  };

  mLayoutFunc = [&](IGraphics* pGraphics)
  {
    pGraphics->AttachCornerResizer(new ThemeCornerResizer(pGraphics->GetBounds()), EUIResizerMode::Scale, false);
    pGraphics->AttachPanelBackground(COL_100());
    pGraphics->LoadFont(kFontRegular, MIXED_FN);
    pGraphics->LoadFont(kFontSemiBold, MIXED_SB_FN);
    pGraphics->LoadFont(kFontBold, MIXED_BD_FN);

    const IVStyle style   = MakeORMStyle();
    const IVStyle btnStyle= MakeButtonStyle();
    IVStyle toggleStyle = btnStyle;
    toggleStyle.showLabel = false;
    toggleStyle.showValue = false;

    mTextBindings.clear();
    mTooltipBindings.clear();
    auto bindText = [this](int id, std::function<void(const char*)> apply) {
      mTextBindings.push_back({ id, std::move(apply) });
    };
    auto bindTip = [this](IControl* c, int id) {
      mTooltipBindings.push_back({ c, id });
    };

    constexpr float kCol1X     = 740.f;
    constexpr float kCol2X     = 824.f;
    constexpr float kBtnW      = 72.f;
    constexpr float kBtnH      = 30.f;
    constexpr float kSlotPitch = 34.f;
    constexpr float kSliderH   = 42.f;
    constexpr float kPanelR    = kCol2X + kBtnW;

    auto padHooks = [&](int kF, int kB, int kSlope,
                        int kEnFreq, int kColFreq, int kEnBw, int kColBw) -> FilterNodePad::Hooks {
      return FilterNodePad::Hooks{
        [this, kF, kB](int id, double v) { EditCorner(kF, kB, id, v); },
        [this, kSlope](int slopeDb) { SetSlopeFromMenu(kSlope, slopeDb); },
        [this, kEnFreq, kEnBw](int id) { ToggleAgMap(id == kCornerCenter ? kEnFreq : kEnBw); },
        [this, kColFreq, kColBw](int id, int c) { SetAgMapColor(id == kCornerCenter ? kColFreq : kColBw, c); },
      };
    };

    auto bandHooks = [&](int kF, int kB) -> BandRangeSlider::Hooks {
      return BandRangeSlider::Hooks{
        [this] { MaybePushGestureUndo(); },
        [this, kF, kB](int id, double v) { EditCorner(kF, kB, id, v); },
        [this, kF, kB](double lN, double hN) { EditBand(kF, kB, lN, hN); },
      };
    };

    mPadL = new FilterNodePad(IRECT(20, 30, 668, 210), { kFreqL, kBwL }, "LEFT", style,
                              padHooks(kFreqL, kBwL, kSlopeL, kAgEnableFreqL, kAgColorFreqL, kAgEnableBwL, kAgColorBwL));
    pGraphics->AttachControl(mPadL, kCtrlTagPadL);
    bindText(orm::kTxtLeft, [this](const char* s) {
      mPadL->SetSideLabel(mMonoDisplay ? orm::Tr(orm::kTxtMono, orm::UILang()) : s);
    });
    bindText(orm::kTxtCenter, [this](const char* s) { mPadL->SetCenterPrefix(s); });
    bindText(orm::kTxtBandwidth, [this](const char* s) { mPadL->SetBwPrefix(s); });
    bindText(orm::kTxtSlope, [this](const char* s) { mPadL->SetSlopePrefix(s); });
    bindTip(mPadL, orm::kTxtTipPad);
    mBandL = new BandRangeSlider(IRECT(20, 216, 668, 262), { kFreqL, kBwL }, bandHooks(kFreqL, kBwL));
    pGraphics->AttachControl(mBandL);
    bindText(orm::kTxtLowCut, [this](const char* s) { mBandL->SetLowPrefix(s); });
    bindText(orm::kTxtHighCut, [this](const char* s) { mBandL->SetHighPrefix(s); });
    bindTip(mBandL, orm::kTxtTipBand);

    mPadR = new FilterNodePad(IRECT(20, 288, 668, 468), { kFreqR, kBwR }, "RIGHT", style,
                              padHooks(kFreqR, kBwR, kSlopeR, kAgEnableFreqR, kAgColorFreqR, kAgEnableBwR, kAgColorBwR));
    pGraphics->AttachControl(mPadR, kCtrlTagPadR);
    bindText(orm::kTxtRight, [this](const char* s) { mPadR->SetSideLabel(s); });
    bindText(orm::kTxtCenter, [this](const char* s) { mPadR->SetCenterPrefix(s); });
    bindText(orm::kTxtBandwidth, [this](const char* s) { mPadR->SetBwPrefix(s); });
    bindText(orm::kTxtSlope, [this](const char* s) { mPadR->SetSlopePrefix(s); });
    bindTip(mPadR, orm::kTxtTipPad);
    mBandR = new BandRangeSlider(IRECT(20, 474, 668, 520), { kFreqR, kBwR }, bandHooks(kFreqR, kBwR));
    pGraphics->AttachControl(mBandR);
    bindText(orm::kTxtLowCut, [this](const char* s) { mBandR->SetLowPrefix(s); });
    bindText(orm::kTxtHighCut, [this](const char* s) { mBandR->SetHighPrefix(s); });
    bindTip(mBandR, orm::kTxtTipBand);

    auto gainHooks = [this](int kEnable, int kColor) -> ORMSlider::AgHooks
    {
      return ORMSlider::AgHooks{
        [this, kEnable]() { ToggleAgMap(kEnable); },
        [this, kColor](int c) { SetAgMapColor(kColor, c); },
      };
    };
    mGainSliderL = new GainSlider(IRECT(672, 30, 730, 210), kGainL, "GAIN L", style);
    mGainSliderL->SetAgMapHooks(gainHooks(kAgEnableGainL, kAgColorGainL));
    pGraphics->AttachControl(mGainSliderL);
    bindText(orm::kTxtGainL, [this](const char* s) { mGainSliderL->SetHeaderLabel(s); });
    bindTip(mGainSliderL, orm::kTxtTipGain);
    mGainSliderR = new GainSlider(IRECT(672, 288, 730, 468), kGainR, "GAIN R", style);
    mGainSliderR->SetAgMapHooks(gainHooks(kAgEnableGainR, kAgColorGainR));
    pGraphics->AttachControl(mGainSliderR);
    bindText(orm::kTxtGainR, [this](const char* s) { mGainSliderR->SetHeaderLabel(s); });
    bindTip(mGainSliderR, orm::kTxtTipGain);

    SectionTitleControl* presetsTitle = new SectionTitleControl(IRECT(kCol1X, 30, 1050, 52), "PRESETS",
      IText(20, COL_900(), kFontBold, EAlign::Near, EVAlign::Middle), 0, 1);
    pGraphics->AttachControl(presetsTitle);
    presetsTitle->SetTargetRECT(IRECT(kCol1X, 30, kCol1X + 130, 52));
    bindText(orm::kTxtPresets, [presetsTitle](const char* s) { presetsTitle->SetStr(s); presetsTitle->SetDirty(false); });
    bindTip(presetsTitle, orm::kTxtTipPresets);

    auto makeSlotHooks = [this](int pos) -> PresetSlotControl::Hooks
    {
      return PresetSlotControl::Hooks{
        [this, pos]() {
          LoadSlot(mSlotNumber[pos]);
          if (pos < kNumQuick)
          {
            mFadePos = pos;
            if (mFadeSlider)
            {
              mFadeSlider->SetValue((float) (pos / (kNumQuick - 1.0)));
              mFadeSlider->SetDirty(false);
            }
          }
        },
        [this, pos]() { SaveToSlot(mSlotNumber[pos]); },
        [this, pos]() { RestoreDefault(mSlotNumber[pos]); },
        [this, pos]() { OnDragBegin(pos); },
        [this, pos](float x, float y) { OnDragMove(x, y); },
        [this, pos](float x, float y) { OnDragDrop(pos, x, y); },
        [this, pos]() -> std::string {
          char buf[32];
          snprintf(buf, sizeof(buf), orm::Tr(orm::kTxtPreset, orm::UILang()), mSlotNumber[pos] + 1);
          std::string s = buf;
          s += "\n";
          s += orm::Tr(orm::kTxtTipDrag, orm::UILang());
          s += "\n";
          s += orm::Tr(orm::kTxtTipSaveHere, orm::UILang());
          s += "\n";
          s += orm::Tr(orm::kTxtTipMenu, orm::UILang());
          return s;
        },
      };
    };

    for (int r = 0; r < 4; ++r)
    {
      for (int c = 0; c < 4; ++c)
      {
        const int pos = kNumBottom + r * 4 + c;
        char label[8];
        snprintf(label, 8, "%d", mSlotNumber[pos] + 1);
        PresetSlotControl* btn = new PresetSlotControl(
          IRECT(kCol1X + c * 39, 56 + r * 32,
                kCol1X + c * 39 + 39, 56 + r * 32 + 32),
          makeSlotHooks(pos), label, btnStyle);
        mSlotButtons[pos] = btn;
        pGraphics->AttachControl(btn);
      }
    }

    ORMSlider* morphSlider = new ORMSlider(IRECT(kCol1X, 188, kPanelR, 230),
      [this](IControl* pCtrl) {
        MaybePushGestureUndo();
        const double n = pCtrl->GetValue(0);
        mFadeTime = (n <= 0.) ? 0. : 0.01 * std::pow(6000., n);
      }, "MORPH", style);
    morphSlider->SetValueFormatter([this](WDL_String& ds) {
      char buf[32];
      snprintf(buf, sizeof(buf), "%.2fs", mFadeTime);
      ds.Set(buf);
    });
    pGraphics->AttachControl(morphSlider);
    morphSlider->SetHeaderFont(kFontRegular);
    morphSlider->SetValue(std::log(0.25 / 0.01) / std::log(6000.));
    morphSlider->SetDirty(false);
    bindText(orm::kTxtMorph, [morphSlider](const char* s) { morphSlider->SetHeaderLabel(s); });
    bindTip(morphSlider, orm::kTxtTipMorph);

    SectionTitleControl* randomTitle = new SectionTitleControl(IRECT(kCol1X, 234, 1050, 260), "RANDOM",
      IText(20, COL_900(), kFontBold, EAlign::Near, EVAlign::Middle), 0, 1);
    pGraphics->AttachControl(randomTitle);
    randomTitle->SetTargetRECT(IRECT(kCol1X, 234, kCol1X + 130, 260));
    bindText(orm::kTxtRandom, [randomTitle](const char* s) { randomTitle->SetStr(s); randomTitle->SetDirty(false); });
    bindTip(randomTitle, orm::kTxtTipRandom);
    mAgPicker = new AgColorPickerControl(IRECT(kPanelR - AG_SWATCH - 1.f, 240.f, kPanelR - 1.f, 254.f),
                                         [this](int idx) { SetAgSelectedColor(idx); });
    pGraphics->AttachControl(mAgPicker);
    bindTip(mAgPicker, orm::kTxtTipAgPicker);
    {
      const int kAmountParams[4] = { kAgAmount, kAgAmountY, kAgAmountB, kAgAmountG };
      const int kRateParams[4]   = { kAgRate, kAgRateY, kAgRateB, kAgRateG };
      for (int c = 0; c < 4; ++c)
      {
        mAgRangeSlider[c] = new ORMSlider(IRECT(kCol1X, 264, kPanelR, 306), kAmountParams[c],
                                          "RANGE", style, EDirection::Horizontal);
        pGraphics->AttachControl(mAgRangeSlider[c]);
        mAgRangeSlider[c]->SetHeaderSwatchColor(c);
        mAgRangeSlider[c]->SetHeaderFont(kFontRegular);
        mAgRangeSlider[c]->Hide(c != mAgSelColor);
        bindText(orm::kTxtRange, [this, c](const char* s) { mAgRangeSlider[c]->SetHeaderLabel(s); });
        bindTip(mAgRangeSlider[c], orm::kTxtTipRange);
        mAgSpeedSlider[c] = new ORMSlider(IRECT(kCol1X, 310, kPanelR, 352), kRateParams[c],
                                          "SPEED", style, EDirection::Horizontal);
        pGraphics->AttachControl(mAgSpeedSlider[c]);
        mAgSpeedSlider[c]->SetHeaderSwatchColor(c);
        mAgSpeedSlider[c]->SetHeaderFont(kFontRegular);
        mAgSpeedSlider[c]->Hide(c != mAgSelColor);
        bindText(orm::kTxtSpeed, [this, c](const char* s) { mAgSpeedSlider[c]->SetHeaderLabel(s); });
        bindTip(mAgSpeedSlider[c], orm::kTxtTipSpeed);
      }
    }

    IVButtonControl* copyLRBtn = MakeMomentary(IRECT(kCol1X, 356, kCol1X + 78, 386), [this](IControl*) { CopyLtoR(); }, "L->R", btnStyle);
    pGraphics->AttachControl(copyLRBtn);
    bindText(orm::kTxtCopyLR, [copyLRBtn](const char* s) { copyLRBtn->SetLabelStr(s); copyLRBtn->SetDirty(false); });
    IVButtonControl* copyRLBtn = MakeMomentary(IRECT(kCol1X + 78, 356, kPanelR, 386), [this](IControl*) { CopyRtoL(); }, "R->L", btnStyle);
    pGraphics->AttachControl(copyRLBtn);
    bindText(orm::kTxtCopyRL, [copyRLBtn](const char* s) { copyRLBtn->SetLabelStr(s); copyRLBtn->SetDirty(false); });
    FlatToggleControl* linkToggle = new FlatToggleControl(IRECT(kCol1X, 386, kCol1X + 78, 416), kLink, " ", toggleStyle, "LINK", "LINK");
    pGraphics->AttachControl(linkToggle);
    bindText(orm::kTxtLink, [linkToggle](const char* s) { linkToggle->SetOnText(s); linkToggle->SetOffText(s); });
    IVButtonControl* flipBtn = MakeMomentary(IRECT(kCol1X + 78, 386, kPanelR, 416), [this](IControl*) { FlipLR(); }, "FLIP", btnStyle);
    pGraphics->AttachControl(flipBtn);
    bindText(orm::kTxtFlip, [flipBtn](const char* s) { flipBtn->SetLabelStr(s); flipBtn->SetDirty(false); });
    mMixSlider = new ORMSlider(IRECT(kCol1X, 420, kPanelR, 462), kMix, "MIX", style, EDirection::Horizontal);
    mMixSlider->SetAgMapHooks(gainHooks(kAgEnableMix, kAgColorMix));
    pGraphics->AttachControl(mMixSlider);
    bindText(orm::kTxtMix, [this](const char* s) { mMixSlider->SetHeaderLabel(s); });
    bindTip(mMixSlider, orm::kTxtTipMix);

    IVButtonControl* undoBtn = MakeMomentary(IRECT(kCol1X, 466, kCol1X + 78, 496), [this](IControl*) { Undo(); }, "UNDO", btnStyle);
    pGraphics->AttachControl(undoBtn);
    bindText(orm::kTxtUndo, [undoBtn](const char* s) { undoBtn->SetLabelStr(s); undoBtn->SetDirty(false); });
    IVButtonControl* redoBtn = MakeMomentary(IRECT(kCol1X + 78, 466, kPanelR, 496), [this](IControl*) { Redo(); }, "REDO", btnStyle);
    pGraphics->AttachControl(redoBtn);
    bindText(orm::kTxtRedo, [redoBtn](const char* s) { redoBtn->SetLabelStr(s); redoBtn->SetDirty(false); });
    IVButtonControl* saveBtn = MakeMomentary(IRECT(kCol1X, 496, kCol1X + 78, 526), [this](IControl*) { SaveFile(); }, "SAVE", btnStyle);
    pGraphics->AttachControl(saveBtn);
    bindText(orm::kTxtSave, [saveBtn](const char* s) { saveBtn->SetLabelStr(s); saveBtn->SetDirty(false); });
    IVButtonControl* loadBtn = MakeMomentary(IRECT(kCol1X + 78, 496, kPanelR, 526), [this](IControl*) { LoadFile(); }, "LOAD", btnStyle);
    pGraphics->AttachControl(loadBtn);
    bindText(orm::kTxtLoad, [loadBtn](const char* s) { loadBtn->SetLabelStr(s); loadBtn->SetDirty(false); });

    for (int i = 0; i < kNumBottom; ++i)
    {
      char label[8];
      snprintf(label, 8, "%d", mSlotNumber[i] + 1);
      const float tick = 39.5f + 609.f * i / (kNumQuick - 1.f);
      const float l = tick - 19.5f;
      PresetSlotControl* btn = new PresetSlotControl(
        IRECT(l, 552, l + 39.f, 582),
        makeSlotHooks(i), label, btnStyle);
      mSlotButtons[i] = btn;
      pGraphics->AttachControl(btn);
    }

    mFadeSlider = new PresetFadeSlider(IRECT(31.5f, 592, 656.5f, 616),
      [this](IControl* pCtrl) {
        MaybePushGestureUndo();
        mFadePos = pCtrl->GetValue(0) * (kNumQuick - 1.0);
        OnFadeDrag(pCtrl->GetValue(0));
      }, btnStyle);
    pGraphics->AttachControl(mFadeSlider);
    bindTip(mFadeSlider, orm::kTxtTipFade);

    IText ormText(32, COL_900(), kFontBold, EAlign::Near, EVAlign::Bottom);
    pGraphics->AttachControl(new SectionTitleControl(IRECT(kCol1X, 544, kCol1X + 120, 578), "ORM", ormText, 0));
    IRECT ormInk(kCol1X, 544, kCol1X + 120, 578);
    pGraphics->MeasureText(ormText, "ORM", ormInk);
    const float gearL = ormInk.R + 8.f;
    const float gearR = gearL + (ormInk.B - ormInk.T);
    pGraphics->AttachControl(new SettingsMenuButton(IRECT(gearL, ormInk.T, gearR, ormInk.B),
      [this]() { ToggleSettingsPanel(); }));
    pGraphics->AttachControl(new SectionTitleControl(IRECT(kCol1X, 576, 1060, 610), "BandPass",
      IText(32, COL_900(), kFontBold, EAlign::Near, EVAlign::Middle), 0));
    pGraphics->AttachControl(new SectionTitleControl(IRECT(gearR + 8.f, 541, 1060, 575), "v" PLUG_VERSION_STR,
      IText(20, COL_500(), kFontRegular, EAlign::Near, EVAlign::Bottom), 1, 0));

    SettingsPanelControl::Hooks settingsHooks;
    settingsHooks.onLanguage = [this](int lang) {
      if (lang != orm::UILang())
      {
        orm::UILang() = lang;
        ApplyLanguage();
      }
    };
    settingsHooks.onTheme = [this](int themeMode) {
      mThemeMode = themeMode;
      ApplyTheme();
    };
    settingsHooks.onHue = [this](int hue) {
      ThemeHue() = hue;
      RefreshThemeColors();
    };
    settingsHooks.onSat = [this](int satMax) {
      ThemeSatMax() = satMax;
      RefreshThemeColors();
    };
#ifdef APP_API
    settingsHooks.listAudioDevices = [this](bool input) {
      std::vector<std::string> names;
      GetAPPAudioDeviceNames(input ? ERoute::kInput : ERoute::kOutput, names);
      return names;
    };
    settingsHooks.currentAudioDevice = [this](bool input) {
      return GetAPPCurrentAudioDeviceName(input ? ERoute::kInput : ERoute::kOutput);
    };
    settingsHooks.onAudioDevice = [this](bool input, const char* name) {
      if (input)
        SetAPPAudioDevices(name, nullptr);
      else
        SetAPPAudioDevices(nullptr, name);
    };
#endif
    mSettingsPanel = new SettingsPanelControl(IRECT(0.f, 0.f, (float) PLUG_WIDTH, (float) PLUG_HEIGHT),
      settingsHooks);
    mSettingsPanel->SetVisible(false);
    pGraphics->AttachControl(mSettingsPanel);

    pGraphics->EnableTooltips(true);
    ApplyLanguage();
    UpdatePads();
    UpdateAgMaps();
  };
#endif
}

#if IPLUG_DSP
void ORMBandPass::ProcessBlock(sample** inputs, sample** outputs, int nFrames)
{
  orm::BandPassCore::Params p;
  if (mParamMailbox.consume(p))
    mCore.setParams(p);

  mCore.updateSmoothing(nFrames);

  const int nOuts = NOutChansConnected();
  const int nIns = NInChansConnected();
  mObservedNOuts.store(nOuts, std::memory_order_relaxed);

  const int nSpec = std::min(nFrames, kMaxBlock);
  if (nIns >= 2)
  {
    std::memcpy(mSpecInL.data(), inputs[0], nSpec * sizeof(sample));
    std::memcpy(mSpecInR.data(), inputs[1], nSpec * sizeof(sample));
  }
  else
  {
    std::memcpy(mSpecInL.data(), inputs[0], nSpec * sizeof(sample));
  }

  if (nOuts >= 2 && nIns >= 2)
  {
    mCore.process(inputs[0], inputs[1], outputs[0], outputs[1], nFrames, mWetL.data(), mWetR.data());
    for (int c = 2; c < nOuts; ++c)
      std::memcpy(outputs[c], inputs[c], nFrames * sizeof(sample));
  }
  else if (nOuts >= 2)
  {
    // Mono input, stereo output: duplicate the input so both channels are filtered independently
    std::memcpy(mMonoIn.data(), inputs[0], nFrames * sizeof(sample));
    mCore.process(inputs[0], mMonoIn.data(), outputs[0], outputs[1], nFrames, mWetL.data(), mWetR.data());
    for (int c = 2; c < nOuts; ++c)
      std::memcpy(outputs[c], outputs[0], nFrames * sizeof(sample));
  }
  else
  {
    mCore.process(inputs[0], outputs[0], nFrames, mWetL.data());
    for (int c = 1; c < nOuts; ++c)
      std::memcpy(outputs[c], outputs[0], nFrames * sizeof(sample));
  }

  if (nIns >= 2)
  {
    sample* specL[2] = { mSpecInL.data(), mWetL.data() };
    sample* specR[2] = { mSpecInR.data(), mWetR.data() };
    mSpectrumL.ProcessBlock(specL, nSpec, kCtrlTagPadL, 2);
    mSpectrumR.ProcessBlock(specR, nSpec, kCtrlTagPadR, 2);
  }
  else if (nOuts >= 2)
  {
    // Duplicated mono input: both pads show the same input spectrum but their own wet signal
    sample* specL[2] = { mSpecInL.data(), mWetL.data() };
    sample* specR[2] = { mSpecInL.data(), mWetR.data() };
    mSpectrumL.ProcessBlock(specL, nSpec, kCtrlTagPadL, 2);
    mSpectrumR.ProcessBlock(specR, nSpec, kCtrlTagPadR, 2);
  }
  else
  {
    sample* specM[2] = { mSpecInL.data(), mWetL.data() };
    mSpectrumL.ProcessBlock(specM, nSpec, kCtrlTagPadL, 2);
  }

  mAgDeltaMailbox.publish(mCore.agDeltas());
}

void ORMBandPass::OnReset()
{
  mCore.prepare(GetSampleRate());
  mCore.setParams(CollectParams());

  mSpectrumL.SetFFTSizeAndOverlap(kSpectrumFFTSize, kSpectrumOverlap);
  mSpectrumR.SetFFTSizeAndOverlap(kSpectrumFFTSize, kSpectrumOverlap);

  SendSpectrumConfig();
}

void ORMBandPass::SendSpectrumConfig()
{
  const double sr = GetSampleRate();
  const int fftSize = kSpectrumFFTSize;
  SendControlMsgFromDelegate(kCtrlTagPadL, FilterNodePad::kMsgTagSampleRate, sizeof(double), &sr);
  SendControlMsgFromDelegate(kCtrlTagPadL, FilterNodePad::kMsgTagFFTSize, sizeof(int), &fftSize);
  SendControlMsgFromDelegate(kCtrlTagPadR, FilterNodePad::kMsgTagSampleRate, sizeof(double), &sr);
  SendControlMsgFromDelegate(kCtrlTagPadR, FilterNodePad::kMsgTagFFTSize, sizeof(int), &fftSize);
}

void ORMBandPass::OnParamChange(int paramIdx, EParamSource source, int sampleOffset)
{
  PublishParamsToCore();
}

void ORMBandPass::OnParamChangeUI(int paramIdx, EParamSource source)
{
  PublishParamsToCore();
  if (source == EParamSource::kUI)
  {
    if (mFading && !mInFadeApply)
      mFading = false;
    MaybePushGestureUndo();
    MirrorLinkedParams(paramIdx);
  }
#if IPLUG_EDITOR
  // May fire from host automation while the editor is closed
  if (GetUI())
  {
    UpdatePads();
    UpdateAgMaps();
  }
#endif
}
#endif

orm::BandPassCore::Params ORMBandPass::CollectParams() const
{
  orm::BandPassCore::Params p;
  p.freqL  = GetParam(kFreqL)->Value();
  p.bwL    = BwMultToOct(GetParam(kBwL)->Value());
  p.gainL  = static_cast<float>(std::pow(10., GetParam(kGainL)->Value() / 20.));
  p.freqR  = GetParam(kFreqR)->Value();
  p.bwR    = BwMultToOct(GetParam(kBwR)->Value());
  p.gainR  = static_cast<float>(std::pow(10., GetParam(kGainR)->Value() / 20.));
  p.linked = GetParam(kLink)->Value() > 0.5;
  p.mix    = static_cast<float>(GetParam(kMix)->Value());
  {
    const int kAmountParams[4] = { kAgAmount, kAgAmountY, kAgAmountB, kAgAmountG };
    const int kRateParams[4]   = { kAgRate, kAgRateY, kAgRateB, kAgRateG };
    for (int c = 0; c < 4; ++c)
    {
      p.agAmount[c]    = static_cast<float>(GetParam(kAmountParams[c])->Value());
      p.agPeriodSec[c] = GetParam(kRateParams[c])->Value();
    }
  }
  auto mapEnable = [&](int idx) { return GetParam(idx)->Value() > 0.5; };
  auto mapColor  = [&](int idx) { return (std::uint8_t) std::clamp(GetParam(idx)->Int(), 0, 3); };
  p.agEnableFreqL = mapEnable(kAgEnableFreqL); p.agColorFreqL = mapColor(kAgColorFreqL);
  p.agEnableBwL   = mapEnable(kAgEnableBwL);   p.agColorBwL   = mapColor(kAgColorBwL);
  p.agEnableGainL = mapEnable(kAgEnableGainL); p.agColorGainL = mapColor(kAgColorGainL);
  p.agEnableFreqR = mapEnable(kAgEnableFreqR); p.agColorFreqR = mapColor(kAgColorFreqR);
  p.agEnableBwR   = mapEnable(kAgEnableBwR);   p.agColorBwR   = mapColor(kAgColorBwR);
  p.agEnableGainR = mapEnable(kAgEnableGainR); p.agColorGainR = mapColor(kAgColorGainR);
  p.agEnableMix   = mapEnable(kAgEnableMix);   p.agColorMix   = mapColor(kAgColorMix);
  p.slopeDbL = kSlopeDb[std::clamp(GetParam(kSlopeL)->Int(), 0, 3)];
  p.slopeDbR = kSlopeDb[std::clamp(GetParam(kSlopeR)->Int(), 0, 3)];
  return p;
}

void ORMBandPass::PublishParamsToCore()
{
  mParamMailbox.publish(CollectParams());
}

void ORMBandPass::SetParamFromEditor(int idx, double value)
{
  GetParam(idx)->Set(value);
  InformHostOfParamChange(idx, GetParam(idx)->GetNormalized());
  PublishParamsToCore();
}

void ORMBandPass::RefreshAfterEdit()
{
#if IPLUG_EDITOR
  if (GetUI())
  {
    SendCurrentParamValuesFromDelegate();
    GetUI()->SetAllControlsDirty();
  }
#endif
  UpdatePads();
  UpdateAgMaps();
  MarkStateStable();
}

namespace {
const std::pair<int, int> kLRParamPairs[] = {
  { kFreqL, kFreqR }, { kBwL, kBwR }, { kGainL, kGainR }, { kSlopeL, kSlopeR },
  { kAgEnableFreqL, kAgEnableFreqR }, { kAgColorFreqL, kAgColorFreqR },
  { kAgEnableBwL,   kAgEnableBwR },   { kAgColorBwL,   kAgColorBwR },
  { kAgEnableGainL, kAgEnableGainR }, { kAgColorGainL, kAgColorGainR },
};

int LeftMirrorOf(int idx)
{
  for (const auto& pr : kLRParamPairs)
    if (pr.second == idx) return pr.first;
  return -1;
}

int RightMirrorOf(int idx)
{
  for (const auto& pr : kLRParamPairs)
    if (pr.first == idx) return pr.second;
  return -1;
}
} // namespace

void ORMBandPass::EditCorner(int kFreq, int kBw, int cornerId, double value)
{
  mFading = false;
  PushUndo();
  const IParam* pf = GetParam(kFreq);
  const double center = pf->FromNormalized(GetParam(kFreq)->GetNormalized());
  const double bw = GetParam(kBw)->Value();
  double lowHz = center / bw;
  double highHz = center * bw;
  double nc = center, nb = bw;
  switch (cornerId)
  {
    case kCornerCenter: nc = value;     break;
    case kCornerBw:     nb = value;     break;
    case kCornerLow:    lowHz = value;  nc = std::sqrt(lowHz * highHz); nb = std::sqrt(highHz / lowHz); break;
    case kCornerHigh:   highHz = value; nc = std::sqrt(lowHz * highHz); nb = std::sqrt(highHz / lowHz); break;
  }
  ClampAndSet(kFreq, kBw, nc, nb);
}

void ORMBandPass::EditBand(int kFreq, int kBw, double lowNorm, double highNorm)
{
  mFading = false;
  const IParam* pf = GetParam(kFreq);
  const double lowHz = pf->FromNormalized(lowNorm);
  const double highHz = pf->FromNormalized(highNorm);
  const double center = std::sqrt(lowHz * highHz);
  const double bw = std::sqrt(highHz / lowHz);
  ClampAndSet(kFreq, kBw, center, bw);
}

void ORMBandPass::ClampAndSet(int kFreq, int kBw, double centerHz, double bw)
{
  centerHz = std::clamp(centerHz, 20., 20000.);
  bw       = std::clamp(bw, 1., 31.);
  double lowHz = centerHz / bw;
  double highHz = centerHz * bw;
  if (lowHz < 20.)     { lowHz = 20.;   centerHz = highHz / bw; }
  if (highHz > 20000.) { highHz = 20000.; centerHz = lowHz * bw; }
  SetParamFromEditor(kFreq, centerHz);
  SetParamFromEditor(kBw, bw);
  MirrorLinkedParams(kFreq);
  MirrorLinkedParams(kBw);
  RefreshAfterEdit();
}

void ORMBandPass::SetSlopeFromMenu(int slopeParamIdx, int slopeDb)
{
  int idx = kSlopeDefaultIdx;
  for (int i = 0; i < 4; ++i)
    if (kSlopeDb[i] == slopeDb) { idx = i; break; }
  const bool changed = GetParam(slopeParamIdx)->Int() != idx;
  const int mirrorIdx = LeftMirrorOf(slopeParamIdx);
  const bool mirrorNeeds = mirrorIdx >= 0 && GetParam(mirrorIdx)->Int() != idx;
  if (!changed && !mirrorNeeds) return;
  mFading = false;
  MaybePushGestureUndo();
  if (changed)
    SetParamFromEditor(slopeParamIdx, (double) idx);
  MirrorLinkedParams(slopeParamIdx);
  RefreshAfterEdit();
}

void ORMBandPass::ToggleAgMap(int enableParamIdx)
{
  mFading = false;
  MaybePushGestureUndo();
  SetParamFromEditor(enableParamIdx, GetParam(enableParamIdx)->Value() > 0.5 ? 0. : 1.);
  MirrorLinkedParams(enableParamIdx);
  RefreshAfterEdit();
}

void ORMBandPass::SetAgMapColor(int colorParamIdx, int colorIdx)
{
  const bool changed = GetParam(colorParamIdx)->Int() != colorIdx;
  // While linked, the left channel is the source of truth: even if this
  // (right-channel) color already matches, its left counterpart may not.
  const int mirrorIdx = LeftMirrorOf(colorParamIdx);
  const bool mirrorNeeds = mirrorIdx >= 0 && GetParam(mirrorIdx)->Int() != colorIdx;
  if (!changed && !mirrorNeeds) return;
  mFading = false;
  MaybePushGestureUndo();
  if (changed)
    SetParamFromEditor(colorParamIdx, (double) colorIdx);
  MirrorLinkedParams(colorParamIdx);
  RefreshAfterEdit();
}

void ORMBandPass::SetAgSelectedColor(int colorIdx)
{
  if (colorIdx == mAgSelColor) return;
  mAgSelColor = std::clamp(colorIdx, 0, 3);
  if (mAgPicker)
    mAgPicker->SetColor(mAgSelColor);
  for (int c = 0; c < 4; ++c)
  {
    if (mAgRangeSlider[c]) mAgRangeSlider[c]->Hide(c != mAgSelColor);
    if (mAgSpeedSlider[c]) mAgSpeedSlider[c]->Hide(c != mAgSelColor);
  }
}

void ORMBandPass::UpdateAgMaps()
{
#if IPLUG_EDITOR
  auto mapOn = [this](int idx) { return GetParam(idx)->Value() > 0.5; };
  auto color = [this](int idx) { return std::clamp(GetParam(idx)->Int(), 0, 3); };
  if (mPadL)
    mPadL->SetAgMap(mapOn(kAgEnableFreqL), color(kAgColorFreqL), mapOn(kAgEnableBwL), color(kAgColorBwL));
  if (mPadR)
    mPadR->SetAgMap(mapOn(kAgEnableFreqR), color(kAgColorFreqR), mapOn(kAgEnableBwR), color(kAgColorBwR));
  if (mGainSliderL) mGainSliderL->SetAgMapState(mapOn(kAgEnableGainL), color(kAgColorGainL));
  if (mGainSliderR) mGainSliderR->SetAgMapState(mapOn(kAgEnableGainR), color(kAgColorGainR));
  if (mMixSlider)   mMixSlider->SetAgMapState(mapOn(kAgEnableMix), color(kAgColorMix));
#endif
}

void ORMBandPass::AgDisplayPush()
{
#if IPLUG_EDITOR
  // OnIdle keeps firing after the editor closes; every stored control pointer
  // is dangling once IGraphics is destroyed (see OnUIClose)
  if (!GetUI()) return;
  if (mPadL) mPadL->SetAgDeltas(mAgDeltas.freqOct[0], mAgDeltas.bwOct[0]);
  if (mPadR) mPadR->SetAgDeltas(mAgDeltas.freqOct[1], mAgDeltas.bwOct[1]);
  if (mGainSliderL) mGainSliderL->SetAgDeltaDb(mAgDeltas.gainDb[0]);
  if (mGainSliderR) mGainSliderR->SetAgDeltaDb(mAgDeltas.gainDb[1]);
  if (mMixSlider)   mMixSlider->SetAgDeltaMix(mAgDeltas.mix);
#endif
}

void ORMBandPass::ApplyMonoDisplay(bool mono)
{
  mMonoDisplay = mono;
#if IPLUG_EDITOR
  if (mPadR) mPadR->SetGhost(mono);
  if (mGainSliderR) mGainSliderR->SetGhost(mono);
  if (mBandR) mBandR->SetGhost(mono);
  if (mPadL)
    mPadL->SetSideLabel(orm::Tr(mono ? orm::kTxtMono : orm::kTxtLeft, orm::UILang()));
#endif
}

void ORMBandPass::MigrateLegacySnapshot(ParamSnapshot& s)
{
  if (s[kAgOn] <= 0.5) return;
  static const int kEnableParams[7] = {
    kAgEnableFreqL, kAgEnableBwL, kAgEnableGainL,
    kAgEnableFreqR, kAgEnableBwR, kAgEnableGainR, kAgEnableMix,
  };
  for (int i = 0; i < 7; ++i)
    if (s[kEnableParams[i]] > 0.5) return; // already uses the new mapping system
  s[kAgEnableFreqL] = 1.;
  s[kAgEnableFreqR] = 1.;
  s[kAgColorFreqL] = 0.; // red
  s[kAgColorFreqR] = 0.;
}

int ORMBandPass::UnserializeState(const IByteChunk& chunk, int startPos)
{
  const int pos = Plugin::UnserializeState(chunk, startPos);
  if (pos <= 0) return pos;
  ParamSnapshot s = Snapshot();
  const ParamSnapshot before = s;
  MigrateLegacySnapshot(s);
  if (!(s == before))
  {
    for (int i = 0; i < kNumParams; ++i)
      if (s[i] != before[i]) SetParamFromEditor(i, s[i]);
  }
  return pos;
}

void ORMBandPass::UpdatePads()
{
  if (mPadL)
  {
    mPadL->SetValueFromDelegate(GetParam(kFreqL)->GetNormalized(), 0);
    mPadL->SetValueFromDelegate(GetParam(kBwL)->GetNormalized(), 1);
    mPadL->SetSlopeIndex(GetParam(kSlopeL)->Int());
    mPadL->SetDirty(false);
  }
  if (mBandL)
  {
    mBandL->SetValueFromDelegate(GetParam(kFreqL)->GetNormalized(), 0);
    mBandL->SetValueFromDelegate(GetParam(kBwL)->GetNormalized(), 1);
    mBandL->SetDirty(false);
  }
  if (mPadR)
  {
    mPadR->SetValueFromDelegate(GetParam(kFreqR)->GetNormalized(), 0);
    mPadR->SetValueFromDelegate(GetParam(kBwR)->GetNormalized(), 1);
    mPadR->SetSlopeIndex(GetParam(kSlopeR)->Int());
    mPadR->SetDirty(false);
  }
  if (mBandR)
  {
    mBandR->SetValueFromDelegate(GetParam(kFreqR)->GetNormalized(), 0);
    mBandR->SetValueFromDelegate(GetParam(kBwR)->GetNormalized(), 1);
    mBandR->SetDirty(false);
  }
}

ParamSnapshot ORMBandPass::Snapshot() const
{
  ParamSnapshot s;
  for (int i = 0; i < kNumParams; ++i)
    s[i] = GetParam(i)->Value();
  return s;
}

void ORMBandPass::ApplySnapshot(const ParamSnapshot& s)
{
  for (int i = 0; i < kNumParams; ++i)
    SetParamFromEditor(i, s[i]);
  RefreshAfterEdit();
}

void ORMBandPass::PushUndo()
{
  PushUndoSnapshot(Snapshot());
}

void ORMBandPass::PushUndoSnapshot(const ParamSnapshot& s)
{
  if (!mUndoStack.empty() && mUndoStack.back() == s) return;
  mUndoStack.push_back(s);
  if (mUndoStack.size() > 100) mUndoStack.pop_front();
  mRedoStack.clear();
}

static constexpr double kGestureGapSec = 0.4;

void ORMBandPass::MaybePushGestureUndo()
{
  using namespace std::chrono;
  const double now = duration<double>(steady_clock::now().time_since_epoch()).count();
  if (now - mLastUIChangeTime > kGestureGapSec)
    PushUndoSnapshot(mStableSnapshot);
  mLastUIChangeTime = now;
  mGesturePending = true;
}

void ORMBandPass::OnIdle()
{
  mSpectrumL.TransmitData(*this);
  mSpectrumR.TransmitData(*this);

  SendSpectrumConfig();

#if IPLUG_EDITOR
  if (mAgDeltaMailbox.consume(mAgDeltas))
    AgDisplayPush();

  const int nOuts = mObservedNOuts.load(std::memory_order_relaxed);
  if ((nOuts < 2) != mMonoDisplay)
    ApplyMonoDisplay(nOuts < 2);
#endif

  using namespace std::chrono;
  const double now = duration<double>(steady_clock::now().time_since_epoch()).count();
  if (mGesturePending && now - mLastUIChangeTime > kGestureGapSec)
  {
    mStableSnapshot = Snapshot();
    mGesturePending = false;
  }
  if (mFading)
  {
    const double t = (now - mFadeStartTime) / std::max(mFadeTime, 0.001);
    if (t >= 1.0)
    {
      mFading = false;
      ApplySnapshot(mFadeTo);
    }
    else
    {
      mInFadeApply = true;
      ApplySnapshot(MixSnapshots(mFadeFrom, mFadeTo, t));
      mInFadeApply = false;
    }
  }
}

void ORMBandPass::OnUIClose()
{
  // The delegate destroys IGraphics and every attached control when the editor
  // closes, while OnIdle/automation callbacks keep running: drop all references.
  mPadL = mPadR = nullptr;
  mBandL = mBandR = nullptr;
  mMixSlider = nullptr;
  mGainSliderL = mGainSliderR = nullptr;
  for (ORMSlider*& s : mAgRangeSlider) s = nullptr;
  for (ORMSlider*& s : mAgSpeedSlider) s = nullptr;
  mAgPicker = nullptr;
  mFadeSlider = nullptr;
  for (PresetSlotControl*& b : mSlotButtons) b = nullptr;
  mSettingsPanel = nullptr;
  mTextBindings.clear();
  mTooltipBindings.clear();
  // let OnIdle re-apply mono display to the freshly created controls on reopen
  mMonoDisplay = false;
}

void ORMBandPass::OnParentWindowResize(int width, int height)
{
  if (auto* pGraphics = GetUI())
  {
    const float platformScale = pGraphics->GetPlatformWindowScale();
    const float sx = static_cast<float>(width) / platformScale / static_cast<float>(pGraphics->Width());
    const float sy = static_cast<float>(height) / platformScale / static_cast<float>(pGraphics->Height());
    pGraphics->Resize(pGraphics->Width(), pGraphics->Height(), std::min(sx, sy), false);
  }
}

bool ORMBandPass::ConstrainEditorResize(int& w, int& h) const
{
  constexpr double kMinScale = DEFAULT_MIN_DRAW_SCALE;
  w = std::max(w, static_cast<int>(PLUG_WIDTH * kMinScale));

  const int wantH = static_cast<int>(std::lround(w * static_cast<double>(PLUG_HEIGHT) / PLUG_WIDTH));
  const bool ok = (h == wantH);
  h = wantH;
  return ok;
}

void ORMBandPass::MarkStateStable()
{
  mStableSnapshot = Snapshot();
  mGesturePending = false;
}

void ORMBandPass::Undo()
{
  if (mUndoStack.empty()) return;
  mFading = false;
  mRedoStack.push_back(Snapshot());
  const ParamSnapshot s = mUndoStack.back();
  mUndoStack.pop_back();
  ApplySnapshot(s);
}

void ORMBandPass::Redo()
{
  if (mRedoStack.empty()) return;
  mFading = false;
  mUndoStack.push_back(Snapshot());
  const ParamSnapshot s = mRedoStack.back();
  mRedoStack.pop_back();
  ApplySnapshot(s);
}

void ORMBandPass::SaveToSlot(int idx)
{
  if (idx < 0 || idx >= kNumPresets) return;
  mPresets[idx] = Snapshot();
}

void ORMBandPass::LoadSlot(int idx)
{
  if (idx < 0 || idx >= kNumPresets) return;
  PushUndo();
  mCurrentPreset = idx;
  StartFade(mPresets[idx]);
}

void ORMBandPass::RestoreDefault(int idx)
{
  if (idx < 0 || idx >= kNumPresets) return;
  mPresets[idx] = mDefaultSnapshot;
  if (idx == mCurrentPreset)
  {
    mFading = false;
    PushUndo();
    ApplySnapshot(mDefaultSnapshot);
  }
}

void ORMBandPass::SwapSlots(int posA, int posB)
{
  if (posA == posB) return;
  if (posA < 0 || posA >= kNumPresets || posB < 0 || posB >= kNumPresets) return;
  std::swap(mSlotNumber[posA], mSlotNumber[posB]);
  RefreshSlotLabels();
}

void ORMBandPass::RefreshSlotLabels()
{
  for (int i = 0; i < kNumPresets; ++i)
  {
    if (!mSlotButtons[i]) continue;
    char buf[8];
    snprintf(buf, sizeof(buf), "%d", mSlotNumber[i] + 1);
    mSlotButtons[i]->SetSlotLabel(buf);
  }
}

void ORMBandPass::SaveFile()
{
  if (!GetUI()) return;
  mDialogFileName.Set("ORMBandPass Presets");
  mDialogPath.Set("");
  GetUI()->PromptForFile(mDialogFileName, mDialogPath, EFileAction::Save, "json",
    [this](const WDL_String& fileName, const WDL_String& path) {
      if (fileName.GetLength() == 0) return;
      std::string full = fileName.Get();
      if (full.size() < 5 || full.compare(full.size() - 5, 5, ".json") != 0)
        full += ".json";
      std::string err;
      WritePresetFileTo(full, err);
      if (!err.empty() && GetUI())
        GetUI()->ShowMessageBox(err.c_str(), "Save Failed", kMB_OK);
    });
}

void ORMBandPass::LoadFile()
{
  if (!GetUI()) return;
  mDialogFileName.Set("");
  GetUI()->PromptForFile(mDialogFileName, mDialogPath, EFileAction::Open, "json",
    [this](const WDL_String& fileName, const WDL_String& path) {
      if (fileName.GetLength() == 0) return;
      std::string err;
      ReadPresetFileFrom(fileName.Get(), err);
      if (!err.empty() && GetUI())
        GetUI()->ShowMessageBox(err.c_str(), "Load Failed", kMB_OK);
    });
}

void ORMBandPass::WritePresetFileTo(const std::string& path, std::string& err)
{
  PresetFileData data;
  for (const auto& p : mPresets)
  {
    std::vector<double> vals(p.begin(), p.end());
    data.presets.push_back(std::move(vals));
  }
  const ParamSnapshot cur = Snapshot();
  data.currentValues.assign(cur.begin(), cur.end());
  data.currentPreset = mCurrentPreset;
  data.fadePos = mFadePos;

  if (WritePresetFile(path, data, err))
    err.clear();
}

void ORMBandPass::ReadPresetFileFrom(const std::string& path, std::string& err)
{
  PresetFileData data;
  if (!ReadPresetFile(path, data, err)) return;

  if ((int) data.presets.size() != kNumPresets) { err = "Preset count mismatch (expected 24)"; return; }
  const auto checkLen = [&](const std::vector<double>& e) -> bool {
    return (int) e.size() == kNumParams || (int) e.size() == kNumLegacyParamsV2;
  };
  for (const auto& e : data.presets)
    if (!checkLen(e)) { err = "Preset parameter count mismatch (expected " + std::to_string(kNumParams) + ")"; return; }
  if (!data.currentValues.empty() && !checkLen(data.currentValues))
  { err = "Current values count mismatch (expected " + std::to_string(kNumParams) + ")"; return; }

  const auto toSnapshot = [&](const std::vector<double>& e) -> ParamSnapshot {
    ParamSnapshot s = mDefaultSnapshot;
    const int n = std::min((int) e.size(), (int) kNumParams);
    for (int i = 0; i < n; ++i) s[i] = e[i];
    MigrateLegacySnapshot(s);
    return s;
  };

  PushUndo();
  mFading = false;

  for (int i = 0; i < kNumPresets; ++i)
    mPresets[i] = toSnapshot(data.presets[i]);
  mCurrentPreset = std::clamp(data.currentPreset, 0, kNumPresets - 1);

  ParamSnapshot cur = data.currentValues.empty() ? mDefaultSnapshot : toSnapshot(data.currentValues);
  ApplySnapshot(cur);

  mFadePos = std::clamp(data.fadePos, 0.0, (double) (kNumQuick - 1));
  if (mFadeSlider)
  {
    mFadeSlider->SetValue((float) (mFadePos / (kNumQuick - 1.0)));
    mFadeSlider->SetDirty(false);
  }

  for (int i = 0; i < kNumPresets; ++i)
    mSlotNumber[i] = i;
  RefreshSlotLabels();

  err.clear();
}

void ORMBandPass::OnDragBegin(int src)
{
  mDragSourceSlot = src;
  mDragTargetSlot = -1;
}

int ORMBandPass::HitTestSlot(float x, float y)
{
  for (int i = 0; i < kNumPresets; ++i)
    if (mSlotButtons[i] && mSlotButtons[i]->GetWidgetBounds().Contains(x, y))
      return i;
  return -1;
}

void ORMBandPass::OnDragMove(float x, float y)
{
  if (mDragSourceSlot < 0) return;
  int target = HitTestSlot(x, y);
  if (target == mDragSourceSlot) target = -1;
  if (target == mDragTargetSlot) return;

  if (mDragTargetSlot >= 0 && mSlotButtons[mDragTargetSlot])
    mSlotButtons[mDragTargetSlot]->SetDragTarget(false);
  mDragTargetSlot = target;
  if (mDragTargetSlot >= 0 && mSlotButtons[mDragTargetSlot])
    mSlotButtons[mDragTargetSlot]->SetDragTarget(true);
}

void ORMBandPass::OnDragDrop(int src, float x, float y)
{
  if (mDragTargetSlot >= 0 && mSlotButtons[mDragTargetSlot])
    mSlotButtons[mDragTargetSlot]->SetDragTarget(false);
  mDragTargetSlot = -1;

  const int target = HitTestSlot(x, y);
  mDragSourceSlot = -1;
  if (target >= 0 && target != src)
    SwapSlots(src, target);
}

void ORMBandPass::ApplyLtoRParams()
{
  for (const auto& pr : kLRParamPairs)
  {
    SetParamFromEditor(pr.second, GetParam(pr.first)->Value());
#if IPLUG_EDITOR
    if (GetUI())
      SendParameterValueFromDelegate(pr.second, GetParam(pr.second)->GetNormalized(), true);
#endif
  }
}

void ORMBandPass::ApplyRtoLParams()
{
  for (const auto& pr : kLRParamPairs)
  {
    SetParamFromEditor(pr.first, GetParam(pr.second)->Value());
#if IPLUG_EDITOR
    if (GetUI())
      SendParameterValueFromDelegate(pr.first, GetParam(pr.first)->GetNormalized(), true);
#endif
  }
}

void ORMBandPass::CopyLtoR()
{
  mFading = false;
  PushUndo();
  ApplyLtoRParams();
  RefreshAfterEdit();
}

void ORMBandPass::CopyRtoL()
{
  mFading = false;
  PushUndo();
  ApplyRtoLParams();
  RefreshAfterEdit();
}

void ORMBandPass::FlipLR()
{
  mFading = false;
  PushUndo();
  const double fL = GetParam(kFreqL)->Value(), bL = GetParam(kBwL)->Value(), gL = GetParam(kGainL)->Value(), sL = GetParam(kSlopeL)->Value();
  SetParamFromEditor(kFreqL, GetParam(kFreqR)->Value());
  SetParamFromEditor(kBwL,   GetParam(kBwR)->Value());
  SetParamFromEditor(kGainL, GetParam(kGainR)->Value());
  SetParamFromEditor(kSlopeL, GetParam(kSlopeR)->Value());
  SetParamFromEditor(kFreqR, fL);
  SetParamFromEditor(kBwR,   bL);
  SetParamFromEditor(kGainR, gL);
  SetParamFromEditor(kSlopeR, sL);
  RefreshAfterEdit();
}

void ORMBandPass::MirrorLinkedParams(int paramIdx)
{
  if (GetParam(kLink)->Value() < 0.5) return;

  if (paramIdx == kLink)
  {
    ApplyLtoRParams();
    return;
  }

  const int toLeft = LeftMirrorOf(paramIdx);
  if (toLeft >= 0 && std::fabs(GetParam(toLeft)->Value() - GetParam(paramIdx)->Value()) >= 1e-9)
  {
    SetParamFromEditor(toLeft, GetParam(paramIdx)->Value());
#if IPLUG_EDITOR
    if (GetUI())
      SendParameterValueFromDelegate(toLeft, GetParam(toLeft)->GetNormalized(), true);
#endif
  }

  const int mirror = RightMirrorOf(paramIdx);
  if (mirror >= 0 && std::fabs(GetParam(mirror)->Value() - GetParam(paramIdx)->Value()) >= 1e-9)
  {
    SetParamFromEditor(mirror, GetParam(paramIdx)->Value());
#if IPLUG_EDITOR
    if (GetUI())
      SendParameterValueFromDelegate(mirror, GetParam(mirror)->GetNormalized(), true);
#endif
  }
}

ParamSnapshot ORMBandPass::MixSnapshots(const ParamSnapshot& a, const ParamSnapshot& b, double t) const
{
  ParamSnapshot out;
  for (int i = 0; i < kNumParams; ++i)
  {
    const IParam* p = GetParam(i);
    const double na = p->ToNormalized(a[i]);
    const double nb = p->ToNormalized(b[i]);
    out[i] = p->FromNormalized(na + (nb - na) * t);
  }
  return out;
}

ParamSnapshot ORMBandPass::InterpolatePresets(double pos)
{
  const int i0 = std::clamp(static_cast<int>(std::floor(pos)), 0, kNumQuick - 1);
  const int i1 = std::min(i0 + 1, kNumQuick - 1);
  const double t = std::clamp(pos - i0, 0.0, 1.0);
  return MixSnapshots(mPresets[mSlotNumber[i0]], mPresets[mSlotNumber[i1]], t);
}

void ORMBandPass::OnFadeDrag(double normalizedPos)
{
  mFading = false;
  ApplySnapshot(InterpolatePresets(normalizedPos * (kNumQuick - 1)));
}

void ORMBandPass::StartFade(const ParamSnapshot& to)
{
  if (mFadeTime <= 0.001 || !GetUI())
  {
    ApplySnapshot(to);
    return;
  }
  mFadeFrom = Snapshot();
  mFadeTo = to;
  mFadeStartTime = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
  mFading = true;
}

void ORMBandPass::ApplyLanguage()
{
  for (auto& binding : mTextBindings)
    if (binding.second) binding.second(orm::Tr(binding.first, orm::UILang()));
  ApplyTooltips();
#if IPLUG_EDITOR
  if (GetUI())
  {
    GetUI()->SetAllControlsDirty();
    GetUI()->UpdateTooltips();
  }
#endif
}

void ORMBandPass::ApplyTooltips()
{
  for (auto& binding : mTooltipBindings)
    if (binding.first) binding.first->SetTooltip(orm::Tr(binding.second, orm::UILang()));
}

void ORMBandPass::ApplyTheme()
{
  ThemeMode() = mThemeMode;
  RefreshThemeColors();
}

void ORMBandPass::RefreshThemeColors()
{
#if IPLUG_EDITOR
  if (GetUI())
  {
    if (IControl* pBG = GetUI()->GetBackgroundControl())
    {
      if (IPanelControl* pPanel = dynamic_cast<IPanelControl*>(pBG))
        pPanel->SetPattern(COL_100());
    }
    GetUI()->SetAllControlsDirty();
  }
#endif
}

void ORMBandPass::ToggleSettingsPanel()
{
  if (mSettingsPanel)
    mSettingsPanel->SetVisible(mSettingsPanel->IsHidden());
}
