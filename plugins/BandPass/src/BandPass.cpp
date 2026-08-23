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

static IVStyle MakeORMStyle()
{
  IVColorSpec colors = { COL_100(), COL_100(), COL_900(), COL_900(),
                         COL_500(), COL_300(), COL_300(), COL_900(), COL_900() };
  const IText labelText(20, COL_700(), FontRegular(), EAlign::Center, EVAlign::Bottom);
  const IText valueText(20, COL_900(), FontSemiBold(), EAlign::Center, EVAlign::Top);
  return IVStyle(true, true, colors, labelText, valueText,
                 true, true, false, false, 0.2f, 1.5f, 0.f, 1.f, 0.f);
}

static IVStyle MakeButtonStyle()
{
  IVColorSpec colors = { COL_100(), COL_100(), COL_900(), COL_900(),
                         COL_500(), COL_300(), COL_300(), COL_900(), COL_900() };
  const IText labelText(20, COL_900(), FontSemiBold(), EAlign::Center, EVAlign::Middle);
  const IText valueText(20, COL_900(), FontSemiBold(), EAlign::Center, EVAlign::Middle);
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
    const IColor col = mDragging        ? COL_700()  // dragging
                     : GetMouseIsOver() ? COL_900()  // hover
                                        : COL_500(); // rest
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
    strcpy(t.mFont, FontSemiBold());
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
    DrawHeader(g, mDirection == EDirection::Vertical ? -90.f : 0.f);
  }

  void OnMouseDown(float x, float y, const IMouseMod& mod) override
  {
    if (mod.L && !mod.R && !mod.A && ValueRect().Contains(x, y))
    {
      if (GetParam())
        PromptUserInput(ValueRect());
      return;
    }
    IVSliderControl::OnMouseDown(x, y, mod);
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
    const float cx = bounds.MW(), cy = bounds.MH();
    g.FillCircle(COL_100(), cx, cy, HANDLE_R + HANDLE_RING);
    g.FillCircle(COL_900(), cx, cy, HANDLE_R);
  }

protected:
  static constexpr float kHeaderH = 26.f;
  static constexpr float kHeaderW = 26.f;

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
      case kMix:
        std::snprintf(buf, sizeof(buf), "%.0f%%", p->Value() * 100.);
        ds.Set(buf);
        break;
      case kAgRate:
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
      g.DrawText(IText(20, COL_900(), FontSemiBold(), EAlign::Near, EVAlign::Middle),
                 mHeaderLabel.Get(), IRECT(hdr.L, hdr.T, hdr.MW(), hdr.B));
      g.DrawText(IText(20, COL_700(), FontRegular(), EAlign::Far, EVAlign::Middle),
                 ds.Get(), IRECT(hdr.MW(), hdr.T, hdr.R, hdr.B));
    }
    else
    {
      const IRECT hdr(mRECT.L, mRECT.T, mRECT.L + kHeaderW, mRECT.B);
      g.DrawText(IText(20, COL_900(), FontSemiBold(), EAlign::Near, EVAlign::Bottom, rot),
                 mHeaderLabel.Get(), hdr);
      g.DrawText(IText(20, COL_700(), FontRegular(), EAlign::Far, EVAlign::Top, rot),
                 ds.Get(), hdr);
    }
  }

  WDL_String mHeaderLabel;
  std::function<void(WDL_String&)> mValueFormatter;
};

class GainSlider : public ORMSlider
{
public:
  GainSlider(const IRECT& bounds, int paramIdx, const char* label, const IVStyle& style)
  : ORMSlider(bounds, paramIdx, label, style, EDirection::Vertical) {}

protected:
  static constexpr float kTextW = 16.f;
  static constexpr float kTextGap = 6.f;
  static constexpr float kPlotTopInset = 30.f;

  IRECT TextRect() const
  {
    return IRECT(mRECT.R - kTextW - kTextGap, mRECT.T, mRECT.R - kTextGap, mRECT.B);
  }

  // Top of the visible (extended) track = pad plot top.
  float TrackVisTop() const { return mRECT.T + kPlotTopInset; }

  void OnResize() override
  {
    mWidgetBounds = mRECT.GetReducedFromRight(kTextW + kTextGap);
    // Shrink the handle-travel range so that after the +/-handleSize render
    // extension the visible track ends align with the pad plot top / bottom.
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
    g.DrawText(IText(20, COL_700(), FontRegular(), EAlign::Center, EVAlign::Top, 90.f),
               ds.Get(), IRECT(hdr.L, TrackVisTop(), hdr.R, hdr.B));
    g.DrawText(IText(20, COL_900(), FontSemiBold(), EAlign::Center, EVAlign::Bottom, 90.f),
               mHeaderLabel.Get(), hdr);
  }
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
    strcpy(t.mFont, FontSemiBold());
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
    strcpy(t.mFont, mFontStep == 0 ? FontRegular() : mFontStep == 1 ? FontSemiBold() : FontBold());
    g.DrawText(t, mStr.Get(), mRECT, &mBlend);
  }

private:
  int mColorStep = 0;
  int mFontStep = 2;
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
  };

  SettingsPanelControl(const IRECT& bounds, Hooks hooks)
  : IControl(bounds)
  , mHooks(std::move(hooks))
  {
    mCard = IRECT(bounds.MW() - kCardW * 0.5f, bounds.MH() - kCardH * 0.5f,
                  bounds.MW() + kCardW * 0.5f, bounds.MH() + kCardH * 0.5f);
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

    g.DrawText(IText(kTitleSize, COL_900(), FontBold(), EAlign::Near, EVAlign::Middle),
               orm::Tr(orm::kTxtLanguage, lang), IRECT(L, mCard.T + kLangTitleY, mCard.R - kPad, mCard.T + kLangTitleY + kTitleSize));
    g.DrawText(IText(kTitleSize, COL_900(), FontBold(), EAlign::Near, EVAlign::Middle),
               orm::Tr(orm::kTxtTheme, lang), IRECT(L, mCard.T + kThemeTitleY, mCard.R - kPad, mCard.T + kThemeTitleY + kTitleSize));

    DrawButton(g, mLangBtns[0],  orm::Tr(orm::kTxtChinese, lang), lang == orm::kLangZH, mHover == kHoverLangZh, FontCJKSemiBold());
    DrawButton(g, mLangBtns[1],  "ENGLISH", lang == orm::kLangEN, mHover == kHoverLangEn, "Outfit-SemiBold");
    DrawButton(g, mThemeBtns[0], orm::Tr(orm::kTxtDark,  lang), theme == 1, mHover == kHoverThemeDark);
    DrawButton(g, mThemeBtns[1], orm::Tr(orm::kTxtLight, lang), theme == 0, mHover == kHoverThemeLight);

    DrawSliderHeader(g, mSliderHeader[0], orm::Tr(orm::kTxtHue, lang),        HueLabel(lang));
    DrawSlider(g, mSliderTrack[0], HueNorm());
    DrawSliderHeader(g, mSliderHeader[1], orm::Tr(orm::kTxtSaturation, lang), SatLabel(lang));
    DrawSlider(g, mSliderTrack[1], SatNorm());
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
  enum EHover { kHoverNone, kHoverLangZh, kHoverLangEn, kHoverThemeDark, kHoverThemeLight };
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
    g.DrawText(IText(16, fg, font ? font : FontSemiBold(), EAlign::Center, EVAlign::Middle), label, b);
  }

  void DrawSliderHeader(IGraphics& g, const IRECT& hdr, const char* title, const char* value)
  {
    g.DrawText(IText(kHeaderFontSize, COL_900(), FontSemiBold(), EAlign::Near, EVAlign::Middle),
               title, IRECT(hdr.L, hdr.T, hdr.MW(), hdr.B));
    g.DrawText(IText(kHeaderFontSize, COL_700(), FontRegular(), EAlign::Far, EVAlign::Middle),
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

  static constexpr float kCardW = 380.f;
  static constexpr float kCardH = 270.f;
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
  EHover mHover = kHoverNone;
  EDrag mDrag = kDragNone;
};

ORMBandPass::ORMBandPass(const InstanceInfo& info)
: Plugin(info, MakeConfig(kNumParams, 1))
{
  GetParam(kFreqL)->InitDouble("FreqL", std::sqrt(300. * 4000.), 20., 20000., 0.01, "Hz", 0, "", IParam::ShapeExp());
  GetParam(kBwL)  ->InitDouble("BW L", 1.41, 1., 31., 0.01, "x", 0, "", IParam::ShapeExp());
  GetParam(kGainL)->InitDouble("Gain L", 0., -96., 12., 0.01, "");
  GetParam(kFreqR)->InitDouble("FreqR", std::sqrt(300. * 4000.), 20., 20000., 0.01, "Hz", 0, "", IParam::ShapeExp());
  GetParam(kBwR)  ->InitDouble("BW R", 1.41, 1., 31., 0.01, "x", 0, "", IParam::ShapeExp());
  GetParam(kGainR)->InitDouble("Gain R", 0., -96., 12., 0.01, "");
  GetParam(kLink) ->InitBool("Link", false);
  GetParam(kMix)  ->InitDouble("Mix", 1., 0., 1., 0.01, "");
  GetParam(kAgOn) ->InitBool("Agitation", false);
  GetParam(kAgAmount)->InitDouble("Ag Amount", 0.1, 0., 1., 0.01, "");
  GetParam(kAgRate)->InitDouble("Ag Speed", 1., 0.01, 60., 0.01, "", 0, "", IParam::ShapeExp());
  GetParam(kSlopeL)->InitEnum("Slope L", kSlopeDefaultIdx,
    { "12 dB/oct", "24 dB/oct", "48 dB/oct", "96 dB/oct" });
  GetParam(kSlopeR)->InitEnum("Slope R", kSlopeDefaultIdx,
    { "12 dB/oct", "24 dB/oct", "48 dB/oct", "96 dB/oct" });

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
    pGraphics->LoadFont("Outfit", OUTFIT_FN);
    pGraphics->LoadFont("Outfit-SemiBold", OUTFIT_SB_FN);
    pGraphics->LoadFont("Outfit-Bold", OUTFIT_BD_FN);
    pGraphics->LoadFont("Mixed", MIXED_FN);
    pGraphics->LoadFont("Mixed-SemiBold", MIXED_SB_FN);
    pGraphics->LoadFont("Mixed-Bold", MIXED_BD_FN);

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

    auto padHooks = [&](int kF, int kB, int kSlope) -> FilterNodePad::Hooks {
      return FilterNodePad::Hooks{
        [this, kF, kB](int id, double v) { EditCorner(kF, kB, id, v); },
        [this, kSlope](int slopeDb) { SetSlopeFromMenu(kSlope, slopeDb); },
      };
    };

    auto bandHooks = [&](int kF, int kB) -> BandRangeSlider::Hooks {
      return BandRangeSlider::Hooks{
        [this] { MaybePushGestureUndo(); },
        [this, kF, kB](int id, double v) { EditCorner(kF, kB, id, v); },
        [this, kF, kB](double lN, double hN) { EditBand(kF, kB, lN, hN); },
      };
    };

    mPadL = new FilterNodePad(IRECT(20, 30, 668, 210), { kFreqL, kBwL }, "LEFT", style, padHooks(kFreqL, kBwL, kSlopeL));
    pGraphics->AttachControl(mPadL, kCtrlTagPadL);
    bindText(orm::kTxtLeft, [this](const char* s) { mPadL->SetSideLabel(s); });
    bindText(orm::kTxtCenter, [this](const char* s) { mPadL->SetCenterPrefix(s); });
    bindText(orm::kTxtBandwidth, [this](const char* s) { mPadL->SetBwPrefix(s); });
    bindText(orm::kTxtSlope, [this](const char* s) { mPadL->SetSlopePrefix(s); });
    bindTip(mPadL, orm::kTxtTipPad);
    mBandL = new BandRangeSlider(IRECT(20, 216, 668, 262), { kFreqL, kBwL }, bandHooks(kFreqL, kBwL));
    pGraphics->AttachControl(mBandL);
    bindText(orm::kTxtLowCut, [this](const char* s) { mBandL->SetLowPrefix(s); });
    bindText(orm::kTxtHighCut, [this](const char* s) { mBandL->SetHighPrefix(s); });
    bindTip(mBandL, orm::kTxtTipBand);

    mPadR = new FilterNodePad(IRECT(20, 288, 668, 468), { kFreqR, kBwR }, "RIGHT", style, padHooks(kFreqR, kBwR, kSlopeR));
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

    GainSlider* gainL = new GainSlider(IRECT(672, 30, 730, 210), kGainL, "GAIN L", style);
    pGraphics->AttachControl(gainL);
    bindText(orm::kTxtGainL, [gainL](const char* s) { gainL->SetHeaderLabel(s); });
    bindTip(gainL, orm::kTxtTipGain);
    GainSlider* gainR = new GainSlider(IRECT(672, 288, 730, 468), kGainR, "GAIN R", style);
    pGraphics->AttachControl(gainR);
    bindText(orm::kTxtGainR, [gainR](const char* s) { gainR->SetHeaderLabel(s); });
    bindTip(gainR, orm::kTxtTipGain);

    SectionTitleControl* presetsTitle = new SectionTitleControl(IRECT(kCol1X, 30, 1050, 52), "PRESETS",
      IText(20, COL_900(), FontBold(), EAlign::Near, EVAlign::Middle));
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
    morphSlider->SetValue(std::log(0.25 / 0.01) / std::log(6000.));
    morphSlider->SetDirty(false);
    bindText(orm::kTxtMorph, [morphSlider](const char* s) { morphSlider->SetHeaderLabel(s); });
    bindTip(morphSlider, orm::kTxtTipMorph);

    SectionTitleControl* randomTitle = new SectionTitleControl(IRECT(kCol1X, 234, 1050, 260), "RANDOM",
      IText(20, COL_900(), FontBold(), EAlign::Near, EVAlign::Middle));
    pGraphics->AttachControl(randomTitle);
    randomTitle->SetTargetRECT(IRECT(kCol1X, 234, kCol1X + 130, 260));
    bindText(orm::kTxtRandom, [randomTitle](const char* s) { randomTitle->SetStr(s); randomTitle->SetDirty(false); });
    bindTip(randomTitle, orm::kTxtTipRandom);
    FlatToggleControl* agToggle = new FlatToggleControl(IRECT(kCol1X + 116, 234, kPanelR, 260), kAgOn, " ", toggleStyle, "OFF", "ON");
    pGraphics->AttachControl(agToggle);
    bindText(orm::kTxtOff, [agToggle](const char* s) { agToggle->SetOffText(s); });
    bindText(orm::kTxtOn, [agToggle](const char* s) { agToggle->SetOnText(s); });
    ORMSlider* rangeSlider = new ORMSlider(IRECT(kCol1X, 264, kPanelR, 306), kAgAmount, "RANGE", style, EDirection::Horizontal);
    pGraphics->AttachControl(rangeSlider);
    bindText(orm::kTxtRange, [rangeSlider](const char* s) { rangeSlider->SetHeaderLabel(s); });
    bindTip(rangeSlider, orm::kTxtTipRange);
    ORMSlider* speedSlider = new ORMSlider(IRECT(kCol1X, 310, kPanelR, 352), kAgRate, "SPEED", style, EDirection::Horizontal);
    pGraphics->AttachControl(speedSlider);
    bindText(orm::kTxtSpeed, [speedSlider](const char* s) { speedSlider->SetHeaderLabel(s); });
    bindTip(speedSlider, orm::kTxtTipSpeed);

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
    ORMSlider* mixSlider = new ORMSlider(IRECT(kCol1X, 420, kPanelR, 462), kMix, "MIX", style, EDirection::Horizontal);
    pGraphics->AttachControl(mixSlider);
    bindText(orm::kTxtMix, [mixSlider](const char* s) { mixSlider->SetHeaderLabel(s); });
    bindTip(mixSlider, orm::kTxtTipMix);

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

    IText ormText(32, COL_900(), FontBold(), EAlign::Near, EVAlign::Bottom);
    pGraphics->AttachControl(new SectionTitleControl(IRECT(kCol1X, 544, kCol1X + 120, 578), "ORM", ormText, 0));
    IRECT ormInk(kCol1X, 544, kCol1X + 120, 578);
    pGraphics->MeasureText(ormText, "ORM", ormInk);
    const float gearL = ormInk.R + 8.f;
    const float gearR = gearL + (ormInk.B - ormInk.T);
    pGraphics->AttachControl(new SettingsMenuButton(IRECT(gearL, ormInk.T, gearR, ormInk.B),
      [this]() { ToggleSettingsPanel(); }));
    pGraphics->AttachControl(new SectionTitleControl(IRECT(kCol1X, 576, 1060, 610), "BandPass",
      IText(32, COL_900(), FontBold(), EAlign::Near, EVAlign::Middle), 0));
    pGraphics->AttachControl(new SectionTitleControl(IRECT(gearR + 8.f, 541, 1060, 575), "v" PLUG_VERSION_STR,
      IText(20, COL_500(), FontRegular(), EAlign::Near, EVAlign::Bottom), 1, 0));

    mSettingsPanel = new SettingsPanelControl(IRECT(0.f, 0.f, (float) PLUG_WIDTH, (float) PLUG_HEIGHT),
    {
      [this](int lang) {
        if (lang != orm::UILang())
        {
          orm::UILang() = lang;
          ApplyLanguage();
        }
      },
      [this](int themeMode) {
        mThemeMode = themeMode;
        ApplyTheme();
      },
      [this](int hue) {
        ThemeHue() = hue;
        RefreshThemeColors();
      },
      [this](int satMax) {
        ThemeSatMax() = satMax;
        RefreshThemeColors();
      },
    });
    mSettingsPanel->SetVisible(false);
    pGraphics->AttachControl(mSettingsPanel);

    pGraphics->EnableTooltips(true);
    ApplyLanguage();
    UpdatePads();
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

  const int nSpec = std::min(nFrames, kMaxSpecBlock);
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
    mSpectrumL.ProcessBlock(specL, nFrames, kCtrlTagPadL, 2);
    mSpectrumR.ProcessBlock(specR, nFrames, kCtrlTagPadR, 2);
  }
  else
  {
    sample* specM[2] = { mSpecInL.data(), mWetL.data() };
    mSpectrumL.ProcessBlock(specM, nFrames, kCtrlTagPadL, 2);
  }
}

void ORMBandPass::OnReset()
{
  mCore.setParams(CollectParams());
  mCore.prepare(GetSampleRate(), GetBlockSize());

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
  if (source == EParamSource::kHost)
  {
    mCore.setParams(CollectParams());
  }
  else
  {
    PublishParamsToCore();
  }
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
  UpdatePads();
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
  p.agOn   = GetParam(kAgOn)->Value() > 0.5;
  p.agAmount = static_cast<float>(GetParam(kAgAmount)->Value());
  p.agRate = 1.0 / GetParam(kAgRate)->Value();
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
  MarkStateStable();
}

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
  RefreshAfterEdit();
}

void ORMBandPass::SetSlopeFromMenu(int slopeParamIdx, int slopeDb)
{
  int idx = kSlopeDefaultIdx;
  for (int i = 0; i < 4; ++i)
    if (kSlopeDb[i] == slopeDb) { idx = i; break; }
  if (GetParam(slopeParamIdx)->Int() == idx) return;
  mFading = false;
  MaybePushGestureUndo();
  SetParamFromEditor(slopeParamIdx, (double) idx);
  MirrorLinkedParams(slopeParamIdx);
  RefreshAfterEdit();
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
  for (const auto& e : data.presets)
    if ((int) e.size() != kNumParams)           { err = "Preset parameter count mismatch (expected " + std::to_string(kNumParams) + ")"; return; }
  if ((int) data.currentValues.size() != kNumParams) { err = "Current values count mismatch (expected " + std::to_string(kNumParams) + ")"; return; }

  PushUndo();
  mFading = false;

  for (int i = 0; i < kNumPresets; ++i)
    std::copy(data.presets[i].begin(), data.presets[i].end(), mPresets[i].begin());
  mCurrentPreset = std::clamp(data.currentPreset, 0, kNumPresets - 1);

  ParamSnapshot cur {};
  std::copy(data.currentValues.begin(), data.currentValues.end(), cur.begin());
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

void ORMBandPass::CopyLtoR()
{
  mFading = false;
  PushUndo();
  SetParamFromEditor(kFreqR, GetParam(kFreqL)->Value());
  SetParamFromEditor(kBwR,   GetParam(kBwL)->Value());
  SetParamFromEditor(kGainR, GetParam(kGainL)->Value());
  SetParamFromEditor(kSlopeR, GetParam(kSlopeL)->Value());
  RefreshAfterEdit();
}

void ORMBandPass::CopyRtoL()
{
  mFading = false;
  PushUndo();
  SetParamFromEditor(kFreqL, GetParam(kFreqR)->Value());
  SetParamFromEditor(kBwL,   GetParam(kBwR)->Value());
  SetParamFromEditor(kGainL, GetParam(kGainR)->Value());
  SetParamFromEditor(kSlopeL, GetParam(kSlopeR)->Value());
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

  int mirror;
  switch (paramIdx)
  {
    case kFreqL: mirror = kFreqR; break;
    case kBwL:   mirror = kBwR;   break;
    case kGainL: mirror = kGainR; break;
    case kSlopeL: mirror = kSlopeR; break;
    case kFreqR: mirror = kFreqL; break;
    case kBwR:   mirror = kBwL;   break;
    case kGainR: mirror = kGainL; break;
    case kSlopeR: mirror = kSlopeL; break;
    default: return;
  }

  if (std::fabs(GetParam(mirror)->Value() - GetParam(paramIdx)->Value()) < 1e-9) return;

  SetParamFromEditor(mirror, GetParam(paramIdx)->Value());
#if IPLUG_EDITOR
  if (GetUI())
    SendParameterValueFromDelegate(mirror, GetParam(mirror)->GetNormalized(), true);
#endif
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
