#pragma once

#include "IControls.h"
#include "../Theme.h"
#include "UiUtils.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <utility>
#include <string>
#include <vector>

BEGIN_IPLUG_NAMESPACE
BEGIN_IGRAPHICS_NAMESPACE

class SettingsMenuButton : public IControl {
public:
  SettingsMenuButton(const IRECT &bounds, std::function<void()> onToggle)
      : IControl(bounds), mOnToggle(std::move(onToggle)) {}

  void Draw(IGraphics &g) override {
    const float cx = mRECT.MW(), cy = mRECT.MH();
    const float r = (mRECT.W() * 0.5f - 2.f) * 0.8f;
    const IColor col = GetMouseIsOver() ? COL_900() : COL_700();

    g.PathClear();
    g.PathTransformReset();
    g.PathTransformTranslate(cx, cy);
    g.PathCircle(0.f, 0.f, r * 0.72f);
    for (int i = 0; i < 8; ++i) {
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

  void OnMouseDown(float x, float y, const IMouseMod &mod) override {
    if (mOnToggle)
      mOnToggle();
  }

private:
  std::function<void()> mOnToggle;
};

class SettingsPanelControl : public IControl {
public:
  struct Hooks {
    std::function<void(int lang)> onLanguage;
    std::function<void(int themeMode)> onTheme;
    std::function<void(int hue)> onHue;
    std::function<void(int satMax)> onSat;
    std::function<std::vector<std::string>(bool input)> listAudioDevices;
    std::function<const char *(bool input)> currentAudioDevice;
    std::function<void(bool input, const char *name)> onAudioDevice;
    std::function<std::vector<std::string>()> listAudioAPIs;
    std::function<const char *()> currentAudioAPI;
    std::function<void(const char *name)> onAudioAPI;
  };

  SettingsPanelControl(const IRECT &bounds, Hooks hooks) : IControl(bounds), mHooks(std::move(hooks)) {
    mHasAudio = (bool)(mHooks.listAudioDevices && mHooks.currentAudioDevice && mHooks.onAudioDevice);
    mHasDriver = (bool)(mHooks.listAudioAPIs && mHooks.currentAudioAPI && mHooks.onAudioAPI);
    const float cardH = (mHasAudio || mHasDriver) ? kCardHAudio : kCardH;
    mCard = IRECT(bounds.MW() - kCardW * 0.5f, bounds.MH() - cardH * 0.5f, bounds.MW() + kCardW * 0.5f,
                  bounds.MH() + cardH * 0.5f);
    const float bw = (kCardW - 2.f * kPad - kBtnGap) * 0.5f;
    mLangBtns[0] = IRECT(mCard.L + kPad, mCard.T + kLangBtnY, mCard.L + kPad + bw, mCard.T + kLangBtnY + kBtnH);
    mLangBtns[1] = IRECT(mCard.L + kPad + bw + kBtnGap, mCard.T + kLangBtnY, mCard.L + kPad + 2.f * bw + kBtnGap,
                         mCard.T + kLangBtnY + kBtnH);
    mThemeBtns[0] = IRECT(mCard.L + kPad, mCard.T + kThemeBtnY, mCard.L + kPad + bw, mCard.T + kThemeBtnY + kBtnH);
    mThemeBtns[1] = IRECT(mCard.L + kPad + bw + kBtnGap, mCard.T + kThemeBtnY, mCard.L + kPad + 2.f * bw + kBtnGap,
                          mCard.T + kThemeBtnY + kBtnH);
    const float rowW = mCard.W() - 2.f * kPad;
    for (int i = 0; i < 2; ++i) {
      const float headerY = mCard.T + (i == 0 ? kHueTitleY : kSatTitleY);
      mSliderHeader[i] = IRECT(mCard.L + kPad, headerY, mCard.R - kPad, headerY + kHeaderH);
      const float trackY = mCard.T + (i == 0 ? kHueY : kSatY);
      mSliderTrack[i] = IRECT(mCard.L + kPad, trackY, mCard.L + kPad + rowW, trackY + kTrackH);
      const float devY = mCard.T + (i == 0 ? kAudioRow1Y : kAudioRow2Y);
      mAudioRow[i] = IRECT(mCard.L + kPad, devY, mCard.R - kPad, devY + kAudioRowH);
    }
    if (mHasDriver) {
      const float drvY = mCard.T + kDriverRowY;
      mDriverRow = IRECT(mCard.L + kPad, drvY, mCard.R - kPad, drvY + kAudioRowH);
    }
    mHover = kHoverNone;
  }

  void Draw(IGraphics &g) override {
    g.FillRect(IColor(70, 26, 25, 22), mRECT);

    g.FillRect(COL_100(), mCard);
    g.DrawRect(COL_300(), mCard, &mBlend, 1.f);

    const int lang = orm::UILang();
    const int theme = ThemeMode();
    const float L = mCard.L + kPad;

    g.DrawText(IText(kTitleSize, COL_900(), kFontSemiBold, EAlign::Near, EVAlign::Middle),
               orm::Tr(orm::kTxtLanguage, lang),
               IRECT(L, mCard.T + kLangTitleY, mCard.R - kPad, mCard.T + kLangTitleY + kTitleSize));
    g.DrawText(IText(kTitleSize, COL_900(), kFontSemiBold, EAlign::Near, EVAlign::Middle),
               orm::Tr(orm::kTxtTheme, lang),
               IRECT(L, mCard.T + kThemeTitleY, mCard.R - kPad, mCard.T + kThemeTitleY + kTitleSize));

    DrawButton(g, mLangBtns[0], orm::Tr(orm::kTxtChinese, lang), lang == orm::kLangZH, mHover == kHoverLangZh,
               kFontSemiBold);
    DrawButton(g, mLangBtns[1], "ENGLISH", lang == orm::kLangEN, mHover == kHoverLangEn, kFontSemiBold);
    DrawButton(g, mThemeBtns[0], orm::Tr(orm::kTxtDark, lang), theme == 1, mHover == kHoverThemeDark);
    DrawButton(g, mThemeBtns[1], orm::Tr(orm::kTxtLight, lang), theme == 0, mHover == kHoverThemeLight);

    const float sw = AG_SWATCH;
    const IRECT hueSw(mSliderHeader[0].L, mSliderHeader[0].MH() - sw * 0.5f, mSliderHeader[0].L + sw,
                      mSliderHeader[0].MH() + sw * 0.5f);
    g.FillRect(HSBToIColor(ThemeHue(), 0.85f, 1.f), hueSw);
    DrawSliderHeader(g, mSliderHeader[0], orm::Tr(orm::kTxtHue, lang), HueLabel(lang),
                     mSliderHeader[0].L + sw + AG_SWATCH_GAP);
    DrawSlider(g, mSliderTrack[0], HueNorm());

    const IRECT satSw(mSliderHeader[1].L, mSliderHeader[1].MH() - sw * 0.5f, mSliderHeader[1].L + sw,
                      mSliderHeader[1].MH() + sw * 0.5f);
    for (int i = 0; i < kNumSat; ++i) {
      const float segH = satSw.H() / kNumSat;
      const IRECT seg(satSw.L, satSw.T + i * segH, satSw.R, satSw.T + (i + 1.f) * segH);
      const float b = 1.f - 0.25f * (float)i;
      g.FillRect(HSBToIColor(ThemeHue(), ThemeSatMax() / 100.f, b), seg);
    }
    DrawSliderHeader(g, mSliderHeader[1], orm::Tr(orm::kTxtSaturation, lang), SatLabel(lang),
                     mSliderHeader[1].L + sw + AG_SWATCH_GAP);
    DrawSlider(g, mSliderTrack[1], SatNorm());

    if (mHasAudio || mHasDriver) {
      g.DrawText(IText(kTitleSize, COL_900(), kFontSemiBold, EAlign::Near, EVAlign::Middle),
                 orm::Tr(orm::kTxtAudio, lang),
                 IRECT(L, mCard.T + kAudioTitleY, mCard.R - kPad, mCard.T + kAudioTitleY + kTitleSize));
      if (mHasDriver)
        DrawDeviceRow(g, mDriverRow, orm::Tr(orm::kTxtDriver, lang), mHooks.currentAudioAPI(),
                      mHover == kHoverDriver);
      for (int i = 0; i < 2; ++i)
        DrawDeviceRow(g, mAudioRow[i], orm::Tr(i == 0 ? orm::kTxtAudioInput : orm::kTxtAudioOutput, lang),
                      mHooks.currentAudioDevice(i == 0), mHover == (i == 0 ? kHoverAudioIn : kHoverAudioOut));
    }
  }

  void OnMouseDown(float x, float y, const IMouseMod &mod) override {
    if (!mCard.Contains(x, y)) {
      SetVisible(false);
      return;
    }
    for (int i = 0; i < 2; ++i) {
      if (mLangBtns[i].Contains(x, y) && mHooks.onLanguage) {
        mHooks.onLanguage(i == 0 ? orm::kLangZH : orm::kLangEN);
        return;
      }
      if (mThemeBtns[i].Contains(x, y) && mHooks.onTheme) {
        mHooks.onTheme(i == 0 ? 1 : 0);
        return;
      }
    }
    if (mSliderTrack[0].Contains(x, y)) {
      mDrag = kDragHue;
      DragTo(x, y);
      return;
    }
    if (mSliderTrack[1].Contains(x, y)) {
      mDrag = kDragSat;
      DragTo(x, y);
      return;
    }
    if (mHasAudio || mHasDriver) {
      if (mHasDriver && mDriverRow.Contains(x, y)) {
        OpenDriverMenu();
        return;
      }
      if (mHasAudio) {
        if (mAudioRow[0].Contains(x, y)) {
          OpenDeviceMenu(true);
          return;
        }
        if (mAudioRow[1].Contains(x, y)) {
          OpenDeviceMenu(false);
          return;
        }
      }
    }
  }

  void OnMouseDrag(float x, float y, float dX, float dY, const IMouseMod &mod) override {
    if (mDrag != kDragNone)
      DragTo(x, y);
  }

  void OnMouseUp(float x, float y, const IMouseMod &mod) override { mDrag = kDragNone; }

  void OnMouseOver(float x, float y, const IMouseMod &mod) override {
    const EHover h = HitHover(x, y);
    if (h != mHover) {
      mHover = h;
      SetDirty(false);
    }
    IControl::OnMouseOver(x, y, mod);
  }

  void OnMouseOut() override {
    if (mHover != kHoverNone) {
      mHover = kHoverNone;
      SetDirty(false);
    }
    IControl::OnMouseOut();
  }

  void SetVisible(bool show) {
    Hide(!show);
    SetDirty(false);
  }

private:
  enum EHover {
    kHoverNone,
    kHoverLangZh,
    kHoverLangEn,
    kHoverThemeDark,
    kHoverThemeLight,
    kHoverAudioIn,
    kHoverAudioOut,
    kHoverDriver
  };
  enum EDrag { kDragNone, kDragHue, kDragSat };

  float HueNorm() const {
    const int h = ThemeHue();
    return (h - kHueMin) / (float)(kHueMax - kHueMin);
  }
  float SatNorm() const {
    const int s = ThemeSatMax();
    int idx = 0;
    for (int i = 0; i < kNumSat; ++i)
      if (kSatVals[i] == s) {
        idx = i;
        break;
      }
    return idx / (float)(kNumSat - 1);
  }

  void DragTo(float x, float y) {
    const IRECT *s = (mDrag == kDragHue) ? &mSliderTrack[0] : &mSliderTrack[1];
    const float n = std::clamp((x - s->L) / s->W(), 0.f, 1.f);
    if (mDrag == kDragHue) {
      const int hue = kHueMin + (int)std::lround(n * (kHueMax - kHueMin) / kHueStep) * kHueStep;
      if (hue != ThemeHue()) {
        ThemeHue() = hue;
        if (mHooks.onHue)
          mHooks.onHue(hue);
      }
    } else {
      const int idx = (int)std::lround(n * (kNumSat - 1));
      const int sat = kSatVals[idx];
      if (sat != ThemeSatMax()) {
        ThemeSatMax() = sat;
        if (mHooks.onSat)
          mHooks.onSat(sat);
      }
    }
    SetDirty(false);
  }

  EHover HitHover(float x, float y) const {
    if (mLangBtns[0].Contains(x, y))
      return kHoverLangZh;
    if (mLangBtns[1].Contains(x, y))
      return kHoverLangEn;
    if (mThemeBtns[0].Contains(x, y))
      return kHoverThemeDark;
    if (mThemeBtns[1].Contains(x, y))
      return kHoverThemeLight;
    if (mHasDriver && mDriverRow.Contains(x, y))
      return kHoverDriver;
    if (mHasAudio && mAudioRow[0].Contains(x, y))
      return kHoverAudioIn;
    if (mHasAudio && mAudioRow[1].Contains(x, y))
      return kHoverAudioOut;
    return kHoverNone;
  }

  void DrawButton(IGraphics &g, const IRECT &b, const char *label, bool active, bool hover,
                  const char *font = nullptr) {
    const IColor fill = active ? COL_900() : hover ? COL_500() : COL_300();
    g.FillRect(fill, b);
    const IColor fg = active ? COL_100() : COL_900();
    g.DrawText(IText(20, fg, font ? font : kFontSemiBold, EAlign::Center, EVAlign::Middle), label, b);
  }

  void DrawSliderHeader(IGraphics &g, const IRECT &hdr, const char *title, const char *value, float titleL) {
    g.DrawText(IText(kHeaderFontSize, COL_900(), kFontRegular, EAlign::Near, EVAlign::Middle), title,
               IRECT(titleL, hdr.T, hdr.MW(), hdr.B));
    g.DrawText(IText(kHeaderFontSize, COL_700(), kFontRegular, EAlign::Far, EVAlign::Middle), value,
               IRECT(hdr.MW(), hdr.T, hdr.R, hdr.B));
  }

  void DrawSlider(IGraphics &g, const IRECT &s, float norm) {
    const float y = s.MH();
    const float x = s.L + norm * s.W();
    g.FillRect(COL_300(), IRECT(s.L, y - 2.f, s.R, y + 2.f));
    g.FillRect(COL_500(), IRECT(s.L, y - 2.f, x, y + 2.f));
    DrawKnob(g, x, y);
  }

  const char *HueLabel(int lang) const {
    static char buf[16];
    std::snprintf(buf, sizeof(buf), "%d\xC2\xB0", ThemeHue());
    (void)lang;
    return buf;
  }

  const char *SatLabel(int lang) const {
    static const int kIds[kNumSat] = {orm::kTxtSatNone, orm::kTxtSatLow, orm::kTxtSatMed, orm::kTxtSatHigh};
    int idx = 0;
    for (int i = 0; i < kNumSat; ++i)
      if (kSatVals[i] == ThemeSatMax()) {
        idx = i;
        break;
      }
    return orm::Tr(kIds[idx], lang);
  }

  void OpenDeviceMenu(bool isInput) {
    if (!GetUI() || !mHooks.onAudioDevice)
      return;
    const std::vector<std::string> names = mHooks.listAudioDevices(isInput);
    const char *current = mHooks.currentAudioDevice(isInput);
    mMenu.Clear();
    mMenu.SetFunction([this, isInput, names](IPopupMenu *menu) {
      const int idx = menu ? menu->GetChosenItemIdx() : -1;
      if (idx < 0 || idx >= (int)names.size())
        return;
      mHooks.onAudioDevice(isInput, names[idx].c_str());
      SetDirty(false);
    });
    for (const std::string &n : names)
      mMenu.AddItem(n.c_str());
    for (int i = 0; i < (int)names.size(); ++i)
      if (names[i] == current) {
        mMenu.CheckItemAlone(i);
        break;
      }
    GetUI()->CreatePopupMenu(*this, mMenu, isInput ? mAudioRow[0] : mAudioRow[1], kNoValIdx);
  }

  void OpenDriverMenu() {
    if (!GetUI() || !mHooks.onAudioAPI)
      return;
    const std::vector<std::string> names = mHooks.listAudioAPIs();
    const char *current = mHooks.currentAudioAPI();
    mDriverMenu.Clear();
    mDriverMenu.SetFunction([this, names](IPopupMenu *menu) {
      const int idx = menu ? menu->GetChosenItemIdx() : -1;
      if (idx < 0 || idx >= (int)names.size())
        return;
      mHooks.onAudioAPI(names[idx].c_str());
      SetDirty(false);
    });
    for (const std::string &n : names)
      mDriverMenu.AddItem(n.c_str());
    for (int i = 0; i < (int)names.size(); ++i)
      if (names[i] == current) {
        mDriverMenu.CheckItemAlone(i);
        break;
      }
    GetUI()->CreatePopupMenu(*this, mDriverMenu, mDriverRow, kNoValIdx);
  }

  void DrawDeviceRow(IGraphics &g, const IRECT &r, const char *label, const char *device, bool hover) {
    if (hover)
      g.FillRect(COL_300(), r);
    const IText labelTxt(20, COL_900(), kFontRegular, EAlign::Near, EVAlign::Middle);
    g.DrawText(labelTxt, label, r);
    IRECT labelBox = r;
    g.MeasureText(labelTxt, label, labelBox);
    const IText valTxt(20, COL_700(), kFontSystem, EAlign::Far, EVAlign::Middle);
    const IRECT valRect(labelBox.R + 12.f, r.T, r.R, r.B);
    WDL_String fitted;
    FitText(g, valTxt, device, valRect.W(), fitted);
    g.DrawText(valTxt, fitted.Get(), valRect);
  }

  static void FitText(IGraphics &g, const IText &t, const char *str, float maxW, WDL_String &out) {
    out.Set(str);
    IRECT m;
    g.MeasureText(t, out.Get(), m);
    if (m.W() <= maxW)
      return;
    int len = out.GetLength();
    while (len > 0) {
      do {
        --len;
      } while (len > 0 && ((unsigned char)out.Get()[len] & 0xC0) == 0x80);
      out.SetLen(len);
      out.Append("\xE2\x80\xA6");
      g.MeasureText(t, out.Get(), m);
      if (m.W() <= maxW)
        return;
      out.SetLen(len);
    }
  }

  static constexpr float kCardW = 380.f;
  static constexpr float kCardH = 270.f;
  static constexpr float kCardHAudio = 392.f;
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
  static constexpr float kDriverRowY = 288.f;
  static constexpr float kAudioRow1Y = 320.f;
  static constexpr float kAudioRow2Y = 352.f;
  static constexpr float kAudioRowH = 28.f;

  static constexpr int kHueMin = 15;
  static constexpr int kHueMax = 360;
  static constexpr int kHueStep = 15;
  static constexpr int kSatMin = 0;
  static constexpr int kSatMax = 50;
  static constexpr int kNumSat = 4;
  static constexpr int kSatVals[kNumSat] = {0, 15, 30, 50};

  Hooks mHooks;
  IRECT mCard;
  IRECT mLangBtns[2];
  IRECT mThemeBtns[2];
  IRECT mSliderHeader[2];
  IRECT mSliderTrack[2];
  IRECT mAudioRow[2];
  IRECT mDriverRow;
  IPopupMenu mMenu;
  IPopupMenu mDriverMenu;
  bool mHasAudio = false;
  bool mHasDriver = false;
  EHover mHover = kHoverNone;
  EDrag mDrag = kDragNone;
};

END_IGRAPHICS_NAMESPACE
END_IPLUG_NAMESPACE
