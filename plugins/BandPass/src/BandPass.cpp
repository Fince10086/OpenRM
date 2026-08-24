#include "BandPass.h"
#include "IPlug_include_in_plug_src.h"
#include "IControls.h"
#include "ICornerResizerControl.h"
#include "Theme.h"
#include "controls/UiUtils.h"
#include "controls/ThemeCornerResizer.h"
#include "controls/FlatButton.h"
#include "controls/ORMSlider.h"
#include "controls/GainSlider.h"
#include "controls/PresetFadeSlider.h"
#include "controls/SectionTitleControl.h"
#include "controls/RandomColorPickerControl.h"
#include "controls/SettingsPanelControl.h"
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

int orm::DetectSystemLanguage() {
#if defined(OS_MAC)
  bool zh = false;
  CFArrayRef langs = CFLocaleCopyPreferredLanguages();
  if (langs) {
    const CFIndex n = CFArrayGetCount(langs);
    for (CFIndex i = 0; i < n; ++i) {
      CFStringRef lang = (CFStringRef)CFArrayGetValueAtIndex(langs, i);
      char buf[64] = {0};
      if (lang && CFStringGetCString(lang, buf, sizeof(buf), kCFStringEncodingUTF8) &&
          std::strncmp(buf, "zh", 2) == 0) {
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

static double BwMultToOct(double m) {
  return 2. * std::log2(m);
}

ORMBandPass::ORMBandPass(const InstanceInfo &info) : Plugin(info, MakeConfig(kNumParams, 1)) {
  GetParam(kFreqL)->InitDouble("FreqL", std::sqrt(300. * 4000.), 20., 20000., 0.01, "Hz", 0, "", IParam::ShapeExp());
  GetParam(kBwL)->InitDouble("BW L", 3.65, 1., 31., 0.01, "x", 0, "", IParam::ShapeExp());
  GetParam(kGainL)->InitDouble("Gain L", 0., -96., 12., 0.01, "");
  GetParam(kFreqR)->InitDouble("FreqR", std::sqrt(300. * 4000.), 20., 20000., 0.01, "Hz", 0, "", IParam::ShapeExp());
  GetParam(kBwR)->InitDouble("BW R", 3.65, 1., 31., 0.01, "x", 0, "", IParam::ShapeExp());
  GetParam(kGainR)->InitDouble("Gain R", 0., -96., 12., 0.01, "");
  GetParam(kLink)->InitBool("Link", false);
  GetParam(kMix)->InitDouble("Mix", 1., 0., 1., 0.01, "");
  GetParam(kRandomAmountR)->InitDouble("Random Amount", 0.5, 0., 1., 0.01, "");
  GetParam(kRandomRateR)->InitDouble("Random Speed", 0.5, 0.01, 60., 0.01, "", 0, "", IParam::ShapeExp());
  GetParam(kSlopeL)->InitEnum("Slope L", kSlopeDefaultIdx, {"12 dB/oct", "24 dB/oct", "48 dB/oct", "96 dB/oct"});
  GetParam(kSlopeR)->InitEnum("Slope R", kSlopeDefaultIdx, {"12 dB/oct", "24 dB/oct", "48 dB/oct", "96 dB/oct"});
  {
    struct MapDef {
      int colorIdx;
      int enableIdx;
      const char *colorName;
      const char *enableName;
    };
    const MapDef maps[7] = {
        {kRandomColorFreqL, kRandomEnableFreqL, "Random Color Freq L", "Random Freq L"},
        {kRandomColorBwL, kRandomEnableBwL, "Random Color BW L", "Random BW L"},
        {kRandomColorGainL, kRandomEnableGainL, "Random Color Gain L", "Random Gain L"},
        {kRandomColorFreqR, kRandomEnableFreqR, "Random Color Freq R", "Random Freq R"},
        {kRandomColorBwR, kRandomEnableBwR, "Random Color BW R", "Random BW R"},
        {kRandomColorGainR, kRandomEnableGainR, "Random Color Gain R", "Random Gain R"},
        {kRandomColorMix, kRandomEnableMix, "Random Color Mix", "Random Mix"},
    };
    for (const MapDef &m : maps) {
      GetParam(m.colorIdx)->InitEnum(m.colorName, 0, {"Red", "Yellow", "Blue", "Green"});
      GetParam(m.enableIdx)->InitBool(m.enableName, false);
    }
  }
  {
    struct RateDef {
      int amountIdx;
      int rateIdx;
      const char *amountName;
      const char *rateName;
    };
    const RateDef rates[3] = {
        {kRandomAmountY, kRandomRateY, "Random Amount Yellow", "Random Speed Yellow"},
        {kRandomAmountB, kRandomRateB, "Random Amount Blue", "Random Speed Blue"},
        {kRandomAmountG, kRandomRateG, "Random Amount Green", "Random Speed Green"},
    };
    const double kDefaultAmount[3] = {0.3, 0.6, 0.4};
    const double kDefaultRate[3] = {1.0, 0.75, 2.0};
    for (int i = 0; i < 3; ++i) {
      const RateDef &r = rates[i];
      GetParam(r.amountIdx)->InitDouble(r.amountName, kDefaultAmount[i], 0., 1., 0.01, "");
      GetParam(r.rateIdx)->InitDouble(r.rateName, kDefaultRate[i], 0.01, 60., 0.01, "", 0, "", IParam::ShapeExp());
    }
  }

  GetParam(kPassL)->InitBool("Pass L", true);
  GetParam(kPassR)->InitBool("Pass R", true);

  for (int i = 0; i < kNumPresets; ++i) {
    mPresets[i] = Snapshot();
    mSlotNumber[i] = i;
  }

  mDefaultSnapshot = Snapshot();
  mStableSnapshot = Snapshot();

  auto setBand = [](ParamSnapshot &s, double lowL, double highL, double lowR, double highR) {
    s[kFreqL] = std::sqrt(lowL * highL);
    s[kBwL] = std::sqrt(highL / lowL);
    s[kFreqR] = std::sqrt(lowR * highR);
    s[kBwR] = std::sqrt(highR / lowR);
  };

  // 出厂预设 0..13: 低切/高切 (L, R) 数据表 (14..23 保持构造时的默认参数)
  static const struct {
    double loL, hiL, loR, hiR;
  } kFactoryBands[] = {
      {300., 4000., 300., 4000.},     {23., 200., 23., 200.},         {23., 1000., 1000., 22050.},
      {1000., 22050., 23., 1000.},    {5000., 22050., 5000., 22050.}, {400., 4000., 400., 4000.},
      {300., 300., 300., 300.},       {6000., 6000., 6000., 6000.},   {6102., 22050., 6102., 22050.},
      {467., 557., 467., 557.},       {77., 5626., 77., 5626.},       {7784., 9155., 7784., 9155.},
      {4982., 12327., 4982., 12327.}, {254., 329., 254., 329.},
  };
  static_assert(sizeof(kFactoryBands) / sizeof(kFactoryBands[0]) == 14, "factory preset count");

  for (int i = 0; i < 14; ++i) {
    ParamSnapshot s = Snapshot();
    setBand(s, kFactoryBands[i].loL, kFactoryBands[i].hiL, kFactoryBands[i].loR, kFactoryBands[i].hiR);
    mPresets[i] = s;
  }

  for (int i = 0; i < kNumPresets; ++i)
    mPresets[i][kLink] = 0.;

#if IPLUG_EDITOR
  mMakeGraphicsFunc = [&]() {
    return MakeGraphics(*this, PLUG_WIDTH, PLUG_HEIGHT, PLUG_FPS, GetScaleForScreen(PLUG_WIDTH, PLUG_HEIGHT));
  };

  mLayoutFunc = [&](IGraphics *pGraphics) {
    pGraphics->AttachCornerResizer(new ThemeCornerResizer(pGraphics->GetBounds()), EUIResizerMode::Scale, false);
    pGraphics->AttachPanelBackground(COL_100());
    pGraphics->LoadFont(kFontRegular, MIXED_FN);
    pGraphics->LoadFont(kFontSemiBold, MIXED_SB_FN);
    pGraphics->LoadFont(kFontBold, MIXED_BD_FN);

    auto loadFontFile = [](IGraphics *g, const char *id, const char *path) -> bool {
      FILE *f = fopen(path, "rb");
      if (!f)
        return false;
      fseek(f, 0, SEEK_END);
      const long sz = ftell(f);
      fseek(f, 0, SEEK_SET);
      if (sz <= 0) {
        fclose(f);
        return false;
      }
      std::vector<char> buf(static_cast<size_t>(sz));
      const bool ok = fread(buf.data(), 1, buf.size(), f) == buf.size();
      fclose(f);
      return ok && g->LoadFont(id, buf.data(), static_cast<int>(buf.size()));
    };
    bool sysFontOk = false;
#if defined(OS_MAC)
    static const char *kSysFontCandidates[] = {
        "/System/Library/Fonts/Supplemental/Arial Unicode.ttf",
        "/System/Library/Fonts/STHeiti Medium.ttc",
        "/System/Library/Fonts/HelveticaNeue.ttc",
    };
    for (const char *p : kSysFontCandidates)
      if ((sysFontOk = loadFontFile(pGraphics, kFontSystem, p)))
        break;
#else
    // Windows: 优先用微软雅黑(含中文), 设备名等非 ASCII 文本需要中文字形
    sysFontOk = pGraphics->LoadFont(kFontSystem, "Microsoft YaHei UI", ETextStyle::Normal);
    if (!sysFontOk)
      sysFontOk = pGraphics->LoadFont(kFontSystem, "Segoe UI", ETextStyle::Normal);
#endif
    if (!sysFontOk)
      pGraphics->LoadFont(kFontSystem, MIXED_FN);

    const IVStyle style = MakeORMStyle();
    const IVStyle btnStyle = MakeButtonStyle();
    IVStyle toggleStyle = btnStyle;
    toggleStyle.showLabel = false;
    toggleStyle.showValue = false;

    mTextBindings.clear();
    mTooltipBindings.clear();
    auto bindText = [this](int id, std::function<void(const char *)> apply) {
      mTextBindings.push_back({id, std::move(apply)});
    };
    auto bindTip = [this](IControl *c, int id) { mTooltipBindings.push_back({c, id}); };

    constexpr float kCol1X = 740.f;
    constexpr float kCol2X = 824.f;
    constexpr float kBtnW = 72.f;
    constexpr float kBtnH = 30.f;
    constexpr float kPanelR = kCol2X + kBtnW;

    // 预设槽与渐变滑杆布局（同一组槽位的几何唯一来源）
    constexpr float kSlotW = 39.f;     // 槽位宽
    constexpr float kGridSlotH = 32.f; // 4x4 网格槽位高
    constexpr float kGridX = kCol1X;
    constexpr float kGridY = 56.f;
    constexpr float kBottomSlotH = 30.f; // 底部快速槽高
    constexpr float kBottomSlotsY = 552.f;
    constexpr float kBottomTick0 = 39.5f; // 底部槽中心刻度起点（历史布局值, 与渐变轨道端点略有偏移, 保持原样）
    constexpr float kBottomTickSpan = 609.f;
    constexpr float kFadeTrackL = 31.5f; // 渐变滑杆轨道
    constexpr float kFadeTrackR = 656.5f;
    constexpr float kFadeY = 592.f;
    constexpr float kFadeH = 24.f;

    auto padHooks = [&](int kF, int kB, int kSlope, int kPass, int kEnFreq, int kColFreq, int kEnBw,
                        int kColBw) -> FilterNodePad::Hooks {
      return FilterNodePad::Hooks{
          [this, kF, kB](int id, double v) { EditCorner(kF, kB, id, v); },
          [this, kSlope](int slopeDb) { SetSlopeFromMenu(kSlope, slopeDb); },
          [this, kEnFreq, kEnBw](int id) { ToggleRandomMap(id == kCornerCenter ? kEnFreq : kEnBw); },
          [this, kColFreq, kColBw](int id, int c) { SetRandomMapColor(id == kCornerCenter ? kColFreq : kColBw, c); },
          [this, kPass]() { TogglePass(kPass); },
      };
    };

    auto bandHooks = [&](int kF, int kB) -> BandRangeSlider::Hooks {
      return BandRangeSlider::Hooks{
          [this] { MaybePushGestureUndo(); },
          [this, kF, kB](int id, double v) { EditCorner(kF, kB, id, v); },
          [this, kF, kB](double lN, double hN) { EditBand(kF, kB, lN, hN); },
      };
    };

    mPadL = new FilterNodePad(IRECT(20, 30, 668, 210), {kFreqL, kBwL}, "LEFT", style,
                              padHooks(kFreqL, kBwL, kSlopeL, kPassL, kRandomEnableFreqL, kRandomColorFreqL,
                                       kRandomEnableBwL, kRandomColorBwL));
    pGraphics->AttachControl(mPadL, kCtrlTagPadL);
    bindText(orm::kTxtLeft,
             [this](const char *s) { mPadL->SetSideLabel(mMonoDisplay ? orm::Tr(orm::kTxtMono, orm::UILang()) : s); });
    bindText(orm::kTxtCenter, [this](const char *s) { mPadL->SetCenterPrefix(s); });
    bindText(orm::kTxtBandwidth, [this](const char *s) { mPadL->SetBwPrefix(s); });
    bindText(orm::kTxtSlope, [this](const char *s) { mPadL->SetSlopePrefix(s); });
    mBandL = new BandRangeSlider(IRECT(20, 216, 668, 262), {kFreqL, kBwL}, bandHooks(kFreqL, kBwL));
    pGraphics->AttachControl(mBandL);
    bindText(orm::kTxtLowCut, [this](const char *s) { mBandL->SetLowPrefix(s); });
    bindText(orm::kTxtHighCut, [this](const char *s) { mBandL->SetHighPrefix(s); });
    bindTip(mBandL, orm::kTxtTipBand);

    mPadR = new FilterNodePad(IRECT(20, 288, 668, 468), {kFreqR, kBwR}, "RIGHT", style,
                              padHooks(kFreqR, kBwR, kSlopeR, kPassR, kRandomEnableFreqR, kRandomColorFreqR,
                                       kRandomEnableBwR, kRandomColorBwR));
    pGraphics->AttachControl(mPadR, kCtrlTagPadR);
    bindText(orm::kTxtRight, [this](const char *s) { mPadR->SetSideLabel(s); });
    bindText(orm::kTxtCenter, [this](const char *s) { mPadR->SetCenterPrefix(s); });
    bindText(orm::kTxtBandwidth, [this](const char *s) { mPadR->SetBwPrefix(s); });
    bindText(orm::kTxtSlope, [this](const char *s) { mPadR->SetSlopePrefix(s); });
    mBandR = new BandRangeSlider(IRECT(20, 474, 668, 520), {kFreqR, kBwR}, bandHooks(kFreqR, kBwR));
    pGraphics->AttachControl(mBandR);
    bindText(orm::kTxtLowCut, [this](const char *s) { mBandR->SetLowPrefix(s); });
    bindText(orm::kTxtHighCut, [this](const char *s) { mBandR->SetHighPrefix(s); });
    bindTip(mBandR, orm::kTxtTipBand);

    auto gainHooks = [this](int kEnable, int kColor) -> ORMSlider::RandomHooks {
      return ORMSlider::RandomHooks{
          [this, kEnable]() { ToggleRandomMap(kEnable); },
          [this, kColor](int c) { SetRandomMapColor(kColor, c); },
      };
    };
    mGainSliderL = new GainSlider(IRECT(672, 30, 730, 210), kGainL, "GAIN L", style);
    mGainSliderL->SetRandomMapHooks(gainHooks(kRandomEnableGainL, kRandomColorGainL));
    pGraphics->AttachControl(mGainSliderL);
    bindText(orm::kTxtGainL, [this](const char *s) { mGainSliderL->SetHeaderLabel(s); });
    bindTip(mGainSliderL, orm::kTxtTipGain);
    mGainSliderR = new GainSlider(IRECT(672, 288, 730, 468), kGainR, "GAIN R", style);
    mGainSliderR->SetRandomMapHooks(gainHooks(kRandomEnableGainR, kRandomColorGainR));
    pGraphics->AttachControl(mGainSliderR);
    bindText(orm::kTxtGainR, [this](const char *s) { mGainSliderR->SetHeaderLabel(s); });
    bindTip(mGainSliderR, orm::kTxtTipGain);

    SectionTitleControl *presetsTitle =
        new SectionTitleControl(IRECT(kCol1X, 30, kPanelR, 52), "PRESETS",
                                IText(20, COL_900(), kFontBold, EAlign::Near, EVAlign::Middle), 0, 1);
    pGraphics->AttachControl(presetsTitle);
    presetsTitle->SetTargetRECT(IRECT(kCol1X, 30, kCol1X + 130, 52));
    bindText(orm::kTxtPresets, [presetsTitle](const char *s) {
      presetsTitle->SetStr(s);
      presetsTitle->SetDirty(false);
    });
    bindTip(presetsTitle, orm::kTxtTipPresets);

    auto makeSlotHooks = [this](int pos) -> PresetSlotControl::Hooks {
      return PresetSlotControl::Hooks{
          [this, pos]() {
            LoadSlot(mSlotNumber[pos]);
            if (pos < kNumQuick) {
              mFadePos = pos;
              if (mFadeSlider) {
                mFadeSlider->SetValue((float)(pos / (kNumQuick - 1.0)));
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

    for (int r = 0; r < 4; ++r) {
      for (int c = 0; c < 4; ++c) {
        const int pos = kNumQuick + r * 4 + c;
        char label[8];
        snprintf(label, 8, "%d", mSlotNumber[pos] + 1);
        PresetSlotControl *btn =
            new PresetSlotControl(IRECT(kGridX + c * kSlotW, kGridY + r * kGridSlotH, kGridX + c * kSlotW + kSlotW,
                                        kGridY + r * kGridSlotH + kGridSlotH),
                                  makeSlotHooks(pos), label, btnStyle);
        mSlotButtons[pos] = btn;
        pGraphics->AttachControl(btn);
      }
    }

    ORMSlider *morphSlider = new ORMSlider(
        IRECT(kCol1X, 188, kPanelR, 230),
        [this](IControl *pCtrl) {
          MaybePushGestureUndo();
          const double n = pCtrl->GetValue(0);
          mFadeTime = (n <= 0.) ? 0. : 0.01 * std::pow(6000., n);
        },
        "MORPH", style);
    morphSlider->SetValueFormatter([this](WDL_String &ds) {
      char buf[32];
      snprintf(buf, sizeof(buf), "%.2fs", mFadeTime);
      ds.Set(buf);
    });
    pGraphics->AttachControl(morphSlider);
    morphSlider->SetHeaderFont(kFontRegular);
    morphSlider->SetValue(std::log(0.25 / 0.01) / std::log(6000.));
    morphSlider->SetDirty(false);
    bindText(orm::kTxtMorph, [morphSlider](const char *s) { morphSlider->SetHeaderLabel(s); });
    bindTip(morphSlider, orm::kTxtTipMorph);

    SectionTitleControl *randomTitle =
        new SectionTitleControl(IRECT(kCol1X, 234, kPanelR, 260), "RANDOM",
                                IText(20, COL_900(), kFontBold, EAlign::Near, EVAlign::Middle), 0, 1);
    pGraphics->AttachControl(randomTitle);
    randomTitle->SetTargetRECT(IRECT(kCol1X, 234, kCol1X + 130, 260));
    bindText(orm::kTxtRandom, [randomTitle](const char *s) {
      randomTitle->SetStr(s);
      randomTitle->SetDirty(false);
    });
    bindTip(randomTitle, orm::kTxtTipRandom);
    mRandomPicker = new RandomColorPickerControl(IRECT(kPanelR - AG_SWATCH - 1.f, 240.f, kPanelR - 1.f, 254.f),
                                                 [this](int idx) { SetRandomSelectedColor(idx); });
    pGraphics->AttachControl(mRandomPicker);
    bindTip(mRandomPicker, orm::kTxtTipRandomPicker);
    {
      const int kAmountParams[4] = {kRandomAmountR, kRandomAmountY, kRandomAmountB, kRandomAmountG};
      const int kRateParams[4] = {kRandomRateR, kRandomRateY, kRandomRateB, kRandomRateG};
      for (int c = 0; c < 4; ++c) {
        mRandomRangeSlider[c] =
            new ORMSlider(IRECT(kCol1X, 264, kPanelR, 306), kAmountParams[c], "RANGE", style, EDirection::Horizontal);
        pGraphics->AttachControl(mRandomRangeSlider[c]);
        mRandomRangeSlider[c]->SetHeaderSwatchColor(c);
        mRandomRangeSlider[c]->SetHeaderFont(kFontRegular);
        mRandomRangeSlider[c]->Hide(c != mRandomSelColor);
        bindText(orm::kTxtRange, [this, c](const char *s) { mRandomRangeSlider[c]->SetHeaderLabel(s); });
        bindTip(mRandomRangeSlider[c], orm::kTxtTipRange);
        mRandomSpeedSlider[c] =
            new ORMSlider(IRECT(kCol1X, 310, kPanelR, 352), kRateParams[c], "SPEED", style, EDirection::Horizontal);
        pGraphics->AttachControl(mRandomSpeedSlider[c]);
        mRandomSpeedSlider[c]->SetHeaderSwatchColor(c);
        mRandomSpeedSlider[c]->SetHeaderFont(kFontRegular);
        mRandomSpeedSlider[c]->Hide(c != mRandomSelColor);
        bindText(orm::kTxtSpeed, [this, c](const char *s) { mRandomSpeedSlider[c]->SetHeaderLabel(s); });
        bindTip(mRandomSpeedSlider[c], orm::kTxtTipSpeed);
      }
    }

    IVButtonControl *copyLRBtn =
        MakeMomentary(IRECT(kCol1X, 356, kCol1X + 78, 386), [this](IControl *) { CopyLtoR(); }, "L->R", btnStyle);
    pGraphics->AttachControl(copyLRBtn);
    bindText(orm::kTxtCopyLR, [copyLRBtn](const char *s) {
      copyLRBtn->SetLabelStr(s);
      copyLRBtn->SetDirty(false);
    });
    IVButtonControl *copyRLBtn =
        MakeMomentary(IRECT(kCol1X + 78, 356, kPanelR, 386), [this](IControl *) { CopyRtoL(); }, "R->L", btnStyle);
    pGraphics->AttachControl(copyRLBtn);
    bindText(orm::kTxtCopyRL, [copyRLBtn](const char *s) {
      copyRLBtn->SetLabelStr(s);
      copyRLBtn->SetDirty(false);
    });
    FlatToggleControl *linkToggle =
        new FlatToggleControl(IRECT(kCol1X, 386, kCol1X + 78, 416), kLink, " ", toggleStyle, "LINK", "LINK");
    pGraphics->AttachControl(linkToggle);
    bindText(orm::kTxtLink, [linkToggle](const char *s) {
      linkToggle->SetOnText(s);
      linkToggle->SetOffText(s);
    });
    IVButtonControl *flipBtn =
        MakeMomentary(IRECT(kCol1X + 78, 386, kPanelR, 416), [this](IControl *) { FlipLR(); }, "FLIP", btnStyle);
    pGraphics->AttachControl(flipBtn);
    bindText(orm::kTxtFlip, [flipBtn](const char *s) {
      flipBtn->SetLabelStr(s);
      flipBtn->SetDirty(false);
    });
    mMixSlider = new ORMSlider(IRECT(kCol1X, 420, kPanelR, 462), kMix, "MIX", style, EDirection::Horizontal);
    mMixSlider->SetRandomMapHooks(gainHooks(kRandomEnableMix, kRandomColorMix));
    pGraphics->AttachControl(mMixSlider);
    bindText(orm::kTxtMix, [this](const char *s) { mMixSlider->SetHeaderLabel(s); });
    bindTip(mMixSlider, orm::kTxtTipMix);

    IVButtonControl *undoBtn =
        MakeMomentary(IRECT(kCol1X, 466, kCol1X + 78, 496), [this](IControl *) { Undo(); }, "UNDO", btnStyle);
    pGraphics->AttachControl(undoBtn);
    bindText(orm::kTxtUndo, [undoBtn](const char *s) {
      undoBtn->SetLabelStr(s);
      undoBtn->SetDirty(false);
    });
    IVButtonControl *redoBtn =
        MakeMomentary(IRECT(kCol1X + 78, 466, kPanelR, 496), [this](IControl *) { Redo(); }, "REDO", btnStyle);
    pGraphics->AttachControl(redoBtn);
    bindText(orm::kTxtRedo, [redoBtn](const char *s) {
      redoBtn->SetLabelStr(s);
      redoBtn->SetDirty(false);
    });
    IVButtonControl *saveBtn =
        MakeMomentary(IRECT(kCol1X, 496, kCol1X + 78, 526), [this](IControl *) { SaveFile(); }, "SAVE", btnStyle);
    pGraphics->AttachControl(saveBtn);
    bindText(orm::kTxtSave, [saveBtn](const char *s) {
      saveBtn->SetLabelStr(s);
      saveBtn->SetDirty(false);
    });
    IVButtonControl *loadBtn =
        MakeMomentary(IRECT(kCol1X + 78, 496, kPanelR, 526), [this](IControl *) { LoadFile(); }, "LOAD", btnStyle);
    pGraphics->AttachControl(loadBtn);
    bindText(orm::kTxtLoad, [loadBtn](const char *s) {
      loadBtn->SetLabelStr(s);
      loadBtn->SetDirty(false);
    });

    for (int i = 0; i < kNumQuick; ++i) {
      char label[8];
      snprintf(label, 8, "%d", mSlotNumber[i] + 1);
      const float tick = kBottomTick0 + kBottomTickSpan * i / (kNumQuick - 1.f);
      const float l = tick - kSlotW * 0.5f;
      PresetSlotControl *btn = new PresetSlotControl(IRECT(l, kBottomSlotsY, l + kSlotW, kBottomSlotsY + kBottomSlotH),
                                                     makeSlotHooks(i), label, btnStyle);
      mSlotButtons[i] = btn;
      pGraphics->AttachControl(btn);
    }

    mFadeSlider = new PresetFadeSlider(
        IRECT(kFadeTrackL, kFadeY, kFadeTrackR, kFadeY + kFadeH),
        [this](IControl *pCtrl) {
          MaybePushGestureUndo();
          mFadePos = pCtrl->GetValue(0) * (kNumQuick - 1.0);
          OnFadeDrag(pCtrl->GetValue(0));
        },
        btnStyle);
    pGraphics->AttachControl(mFadeSlider);
    bindTip(mFadeSlider, orm::kTxtTipFade);

    IText ormText(32, COL_900(), kFontBold, EAlign::Near, EVAlign::Bottom);
    pGraphics->AttachControl(new SectionTitleControl(IRECT(kCol1X, 544, kCol1X + 120, 578), "ORM", ormText, 0));
    IRECT ormInk(kCol1X, 544, kCol1X + 120, 578);
    pGraphics->MeasureText(ormText, "ORM", ormInk);
    const float gearL = ormInk.R + 8.f;
    const float gearR = gearL + (ormInk.B - ormInk.T);
    pGraphics->AttachControl(
        new SettingsMenuButton(IRECT(gearL, ormInk.T, gearR, ormInk.B), [this]() { ToggleSettingsPanel(); }));
    pGraphics->AttachControl(new SectionTitleControl(IRECT(kCol1X, 576, kPanelR, 610), "BandPass",
                                                     IText(32, COL_900(), kFontBold, EAlign::Near, EVAlign::Middle),
                                                     0));
    pGraphics->AttachControl(new SectionTitleControl(IRECT(gearR + 8.f, 541, kPanelR, 575), "v" PLUG_VERSION_STR,
                                                     IText(20, COL_500(), kFontRegular, EAlign::Near, EVAlign::Bottom),
                                                     1, 0));

    SettingsPanelControl::Hooks settingsHooks;
    settingsHooks.onLanguage = [this](int lang) {
      if (lang != orm::UILang()) {
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
    settingsHooks.listAudioAPIs = [this]() {
      std::vector<std::string> names;
      GetAPPAudioAPIs(names);
      return names;
    };
    settingsHooks.currentAudioAPI = [this]() { return GetAPPCurrentAudioAPI(); };
    settingsHooks.onAudioAPI = [this](const char *name) {
      if (GetAPPCurrentAudioAPI() && std::strcmp(GetAPPCurrentAudioAPI(), name) != 0)
        SetAPPAudioAPI(name);
    };
    settingsHooks.listAudioDevices = [this](bool input) {
      std::vector<std::string> names;
      GetAPPAudioDeviceNames(input ? ERoute::kInput : ERoute::kOutput, names);
      return names;
    };
    settingsHooks.currentAudioDevice = [this](bool input) {
      return GetAPPCurrentAudioDeviceName(input ? ERoute::kInput : ERoute::kOutput);
    };
    settingsHooks.onAudioDevice = [this](bool input, const char *name) {
      if (input)
        SetAPPAudioDevices(name, nullptr);
      else
        SetAPPAudioDevices(nullptr, name);
    };
#endif
    mSettingsPanel = new SettingsPanelControl(IRECT(0.f, 0.f, (float)PLUG_WIDTH, (float)PLUG_HEIGHT), settingsHooks);
    mSettingsPanel->SetVisible(false);
    pGraphics->AttachControl(mSettingsPanel);

    pGraphics->EnableTooltips(true);
    ApplyLanguage();
    UpdatePads();
    UpdateRandomMaps();
  };
#endif
}

#if IPLUG_DSP
void ORMBandPass::ProcessBlock(sample **inputs, sample **outputs, int nFrames) {
  // 宿主块尺寸可能超过 kMaxBlock（定长缓冲上限），统一在此钳制，避免后续 memcpy / 湿声缓冲越界。
  nFrames = std::min(nFrames, kMaxBlock);

  orm::BandPassCore::Params p;
  if (mParamMailbox.consume(p))
    mCore.setParams(p);

  mCore.updateSmoothing(nFrames);

  const int nOuts = NOutChansConnected();
  const int nIns = NInChansConnected();
  mObservedNOuts.store(nOuts, std::memory_order_relaxed);

  const int nSpec = nFrames;
  if (nIns >= 2) {
    std::memcpy(mSpecInL.data(), inputs[0], nSpec * sizeof(sample));
    std::memcpy(mSpecInR.data(), inputs[1], nSpec * sizeof(sample));
  } else {
    std::memcpy(mSpecInL.data(), inputs[0], nSpec * sizeof(sample));
  }

  if (nOuts >= 2 && nIns >= 2) {
    mCore.process(inputs[0], inputs[1], outputs[0], outputs[1], nFrames, mWetL.data(), mWetR.data());
    for (int c = 2; c < nOuts; ++c)
      std::memcpy(outputs[c], inputs[c], nFrames * sizeof(sample));
  } else if (nOuts >= 2) {
    std::memcpy(mMonoIn.data(), inputs[0], nFrames * sizeof(sample));
    mCore.process(inputs[0], mMonoIn.data(), outputs[0], outputs[1], nFrames, mWetL.data(), mWetR.data());
    for (int c = 2; c < nOuts; ++c)
      std::memcpy(outputs[c], outputs[0], nFrames * sizeof(sample));
  } else {
    mCore.process(inputs[0], outputs[0], nFrames, mWetL.data());
    for (int c = 1; c < nOuts; ++c)
      std::memcpy(outputs[c], outputs[0], nFrames * sizeof(sample));
  }

  if (nIns >= 2) {
    sample *specL[2] = {mSpecInL.data(), mWetL.data()};
    sample *specR[2] = {mSpecInR.data(), mWetR.data()};
    mSpectrumL.ProcessBlock(specL, nSpec, kCtrlTagPadL, 2);
    mSpectrumR.ProcessBlock(specR, nSpec, kCtrlTagPadR, 2);
  } else if (nOuts >= 2) {
    sample *specL[2] = {mSpecInL.data(), mWetL.data()};
    sample *specR[2] = {mSpecInL.data(), mWetR.data()};
    mSpectrumL.ProcessBlock(specL, nSpec, kCtrlTagPadL, 2);
    mSpectrumR.ProcessBlock(specR, nSpec, kCtrlTagPadR, 2);
  } else {
    sample *specM[2] = {mSpecInL.data(), mWetL.data()};
    mSpectrumL.ProcessBlock(specM, nSpec, kCtrlTagPadL, 2);
  }

  mRandomDeltaMailbox.publish(mCore.randomDeltas());
}

void ORMBandPass::OnReset() {
  mCore.prepare(GetSampleRate());
  mCore.setParams(CollectParams());

  mSpectrumL.SetFFTSizeAndOverlap(kSpectrumFFTSize, kSpectrumOverlap);
  mSpectrumR.SetFFTSizeAndOverlap(kSpectrumFFTSize, kSpectrumOverlap);

  SendSpectrumConfig();
  mSentSampleRate = GetSampleRate();
  mSentFFTSize = kSpectrumFFTSize;
}

void ORMBandPass::SendSpectrumConfig() {
  const double sr = GetSampleRate();
  const int fftSize = kSpectrumFFTSize;
  SendControlMsgFromDelegate(kCtrlTagPadL, FilterNodePad::kMsgTagSampleRate, sizeof(double), &sr);
  SendControlMsgFromDelegate(kCtrlTagPadL, FilterNodePad::kMsgTagFFTSize, sizeof(int), &fftSize);
  SendControlMsgFromDelegate(kCtrlTagPadR, FilterNodePad::kMsgTagSampleRate, sizeof(double), &sr);
  SendControlMsgFromDelegate(kCtrlTagPadR, FilterNodePad::kMsgTagFFTSize, sizeof(int), &fftSize);
}

void ORMBandPass::OnParamChange(int paramIdx, EParamSource source, int sampleOffset) {
  PublishParamsToCore();
}

void ORMBandPass::OnParamChangeUI(int paramIdx, EParamSource source) {
  PublishParamsToCore();
  if (source == EParamSource::kUI) {
    if (mFading && !mInFadeApply)
      mFading = false;
    MaybePushGestureUndo();
    MirrorLinkedParams(paramIdx);
  }
#if IPLUG_EDITOR
  if (GetUI()) {
    UpdatePads();
    UpdateRandomMaps();
  }
#endif
}
#endif

orm::BandPassCore::Params ORMBandPass::CollectParams() const {
  orm::BandPassCore::Params p;
  p.freqL = GetParam(kFreqL)->Value();
  p.bwL = BwMultToOct(GetParam(kBwL)->Value());
  p.gainL = static_cast<float>(std::pow(10., GetParam(kGainL)->Value() / 20.));
  p.freqR = GetParam(kFreqR)->Value();
  p.bwR = BwMultToOct(GetParam(kBwR)->Value());
  p.gainR = static_cast<float>(std::pow(10., GetParam(kGainR)->Value() / 20.));
  p.linked = GetParam(kLink)->Value() > 0.5;
  p.rejectL = GetParam(kPassL)->Value() < 0.5;
  p.rejectR = GetParam(kPassR)->Value() < 0.5;
  p.mix = static_cast<float>(GetParam(kMix)->Value());
  {
    const int kAmountParams[4] = {kRandomAmountR, kRandomAmountY, kRandomAmountB, kRandomAmountG};
    const int kRateParams[4] = {kRandomRateR, kRandomRateY, kRandomRateB, kRandomRateG};
    for (int c = 0; c < 4; ++c) {
      p.randomAmount[c] = static_cast<float>(GetParam(kAmountParams[c])->Value());
      p.randomPeriodSec[c] = GetParam(kRateParams[c])->Value();
    }
  }
  auto mapEnable = [&](int idx) { return GetParam(idx)->Value() > 0.5; };
  auto mapColor = [&](int idx) { return (std::uint8_t)std::clamp(GetParam(idx)->Int(), 0, 3); };
  p.randomEnableFreqL = mapEnable(kRandomEnableFreqL);
  p.randomColorFreqL = mapColor(kRandomColorFreqL);
  p.randomEnableBwL = mapEnable(kRandomEnableBwL);
  p.randomColorBwL = mapColor(kRandomColorBwL);
  p.randomEnableGainL = mapEnable(kRandomEnableGainL);
  p.randomColorGainL = mapColor(kRandomColorGainL);
  p.randomEnableFreqR = mapEnable(kRandomEnableFreqR);
  p.randomColorFreqR = mapColor(kRandomColorFreqR);
  p.randomEnableBwR = mapEnable(kRandomEnableBwR);
  p.randomColorBwR = mapColor(kRandomColorBwR);
  p.randomEnableGainR = mapEnable(kRandomEnableGainR);
  p.randomColorGainR = mapColor(kRandomColorGainR);
  p.randomEnableMix = mapEnable(kRandomEnableMix);
  p.randomColorMix = mapColor(kRandomColorMix);
  p.slopeDbL = kSlopeDb[std::clamp(GetParam(kSlopeL)->Int(), 0, 3)];
  p.slopeDbR = kSlopeDb[std::clamp(GetParam(kSlopeR)->Int(), 0, 3)];
  return p;
}

void ORMBandPass::PublishParamsToCore() {
  mParamMailbox.publish(CollectParams());
}

void ORMBandPass::SetParamFromEditor(int idx, double value) {
  GetParam(idx)->Set(value);
  // 渐变插值期间每 tick 应用全部参数: 逐参数通知宿主过重 (42 次/帧)。
  // 渐变结束的最终 ApplySnapshot 走正常路径, 会把终值通知给宿主。
  if (!mInFadeApply)
    InformHostOfParamChange(idx, GetParam(idx)->GetNormalized());
  PublishParamsToCore();
}

void ORMBandPass::RefreshAfterEdit() {
#if IPLUG_EDITOR
  if (GetUI()) {
    SendCurrentParamValuesFromDelegate();
    GetUI()->SetAllControlsDirty();
  }
#endif
  UpdatePads();
  UpdateRandomMaps();
  MarkStateStable();
}

namespace {
const std::pair<int, int> kLRParamPairs[] = {
    {kFreqL, kFreqR},
    {kBwL, kBwR},
    {kGainL, kGainR},
    {kSlopeL, kSlopeR},
    {kRandomEnableFreqL, kRandomEnableFreqR},
    {kRandomColorFreqL, kRandomColorFreqR},
    {kRandomEnableBwL, kRandomEnableBwR},
    {kRandomColorBwL, kRandomColorBwR},
    {kRandomEnableGainL, kRandomEnableGainR},
    {kRandomColorGainL, kRandomColorGainR},
    {kPassL, kPassR},
};

int LeftMirrorOf(int idx) {
  for (const auto &pr : kLRParamPairs)
    if (pr.second == idx)
      return pr.first;
  return -1;
}

int RightMirrorOf(int idx) {
  for (const auto &pr : kLRParamPairs)
    if (pr.first == idx)
      return pr.second;
  return -1;
}
} // namespace

void ORMBandPass::EditCorner(int kFreq, int kBw, int cornerId, double value) {
  mFading = false;
  PushUndo();
  const IParam *pf = GetParam(kFreq);
  const double center = pf->FromNormalized(GetParam(kFreq)->GetNormalized());
  const double bw = GetParam(kBw)->Value();
  double lowHz = center / bw;
  double highHz = center * bw;
  double nc = center, nb = bw;
  switch (cornerId) {
  case kCornerCenter:
    nc = value;
    break;
  case kCornerBw:
    nb = value;
    break;
  case kCornerLow:
    lowHz = value;
    nc = std::sqrt(lowHz * highHz);
    nb = std::sqrt(highHz / lowHz);
    break;
  case kCornerHigh:
    highHz = value;
    nc = std::sqrt(lowHz * highHz);
    nb = std::sqrt(highHz / lowHz);
    break;
  }
  ClampAndSet(kFreq, kBw, nc, nb);
}

void ORMBandPass::EditBand(int kFreq, int kBw, double lowNorm, double highNorm) {
  mFading = false;
  const IParam *pf = GetParam(kFreq);
  const double lowHz = pf->FromNormalized(lowNorm);
  const double highHz = pf->FromNormalized(highNorm);
  const double center = std::sqrt(lowHz * highHz);
  const double bw = std::sqrt(highHz / lowHz);
  ClampAndSet(kFreq, kBw, center, bw);
}

void ORMBandPass::ClampAndSet(int kFreq, int kBw, double centerHz, double bw) {
  centerHz = std::clamp(centerHz, 20., 20000.);
  bw = std::clamp(bw, 1., 31.);
  double lowHz = centerHz / bw;
  double highHz = centerHz * bw;
  if (lowHz < 20.) {
    lowHz = 20.;
    centerHz = highHz / bw;
  }
  if (highHz > 20000.) {
    highHz = 20000.;
    centerHz = lowHz * bw;
  }
  SetParamFromEditor(kFreq, centerHz);
  SetParamFromEditor(kBw, bw);
  MirrorLinkedParams(kFreq);
  MirrorLinkedParams(kBw);
  RefreshAfterEdit();
}

void ORMBandPass::SetSlopeFromMenu(int slopeParamIdx, int slopeDb) {
  int idx = kSlopeDefaultIdx;
  for (int i = 0; i < 4; ++i)
    if (kSlopeDb[i] == slopeDb) {
      idx = i;
      break;
    }
  const bool changed = GetParam(slopeParamIdx)->Int() != idx;
  const int mirrorIdx = LeftMirrorOf(slopeParamIdx);
  const bool mirrorNeeds = mirrorIdx >= 0 && GetParam(mirrorIdx)->Int() != idx;
  if (!changed && !mirrorNeeds)
    return;
  mFading = false;
  MaybePushGestureUndo();
  if (changed)
    SetParamFromEditor(slopeParamIdx, (double)idx);
  MirrorLinkedParams(slopeParamIdx);
  RefreshAfterEdit();
}

void ORMBandPass::ToggleRandomMap(int enableParamIdx) {
  mFading = false;
  MaybePushGestureUndo();
  SetParamFromEditor(enableParamIdx, GetParam(enableParamIdx)->Value() > 0.5 ? 0. : 1.);
  MirrorLinkedParams(enableParamIdx);
  RefreshAfterEdit();
}

void ORMBandPass::TogglePass(int passIdx) {
  mFading = false;
  MaybePushGestureUndo();
  SetParamFromEditor(passIdx, GetParam(passIdx)->Value() > 0.5 ? 0. : 1.);
  MirrorLinkedParams(passIdx);
  RefreshAfterEdit();
}

void ORMBandPass::SetRandomMapColor(int colorParamIdx, int colorIdx) {
  const bool changed = GetParam(colorParamIdx)->Int() != colorIdx;
  const int mirrorIdx = LeftMirrorOf(colorParamIdx);
  const bool mirrorNeeds = mirrorIdx >= 0 && GetParam(mirrorIdx)->Int() != colorIdx;
  if (!changed && !mirrorNeeds)
    return;
  mFading = false;
  MaybePushGestureUndo();
  if (changed)
    SetParamFromEditor(colorParamIdx, (double)colorIdx);
  MirrorLinkedParams(colorParamIdx);
  RefreshAfterEdit();
}

void ORMBandPass::SetRandomSelectedColor(int colorIdx) {
  if (colorIdx == mRandomSelColor)
    return;
  mRandomSelColor = std::clamp(colorIdx, 0, 3);
  if (mRandomPicker)
    mRandomPicker->SetColor(mRandomSelColor);
  for (int c = 0; c < 4; ++c) {
    if (mRandomRangeSlider[c])
      mRandomRangeSlider[c]->Hide(c != mRandomSelColor);
    if (mRandomSpeedSlider[c])
      mRandomSpeedSlider[c]->Hide(c != mRandomSelColor);
  }
}

void ORMBandPass::UpdateRandomMaps() {
#if IPLUG_EDITOR
  auto mapOn = [this](int idx) { return GetParam(idx)->Value() > 0.5; };
  auto color = [this](int idx) { return std::clamp(GetParam(idx)->Int(), 0, 3); };
  if (mPadL)
    mPadL->SetRandomMap(mapOn(kRandomEnableFreqL), color(kRandomColorFreqL), mapOn(kRandomEnableBwL),
                        color(kRandomColorBwL));
  if (mPadR)
    mPadR->SetRandomMap(mapOn(kRandomEnableFreqR), color(kRandomColorFreqR), mapOn(kRandomEnableBwR),
                        color(kRandomColorBwR));
  if (mPadL)
    mPadL->SetPass(mapOn(kPassL));
  if (mPadR)
    mPadR->SetPass(mapOn(kPassR));
  if (mGainSliderL)
    mGainSliderL->SetRandomMapState(mapOn(kRandomEnableGainL), color(kRandomColorGainL));
  if (mGainSliderR)
    mGainSliderR->SetRandomMapState(mapOn(kRandomEnableGainR), color(kRandomColorGainR));
  if (mMixSlider)
    mMixSlider->SetRandomMapState(mapOn(kRandomEnableMix), color(kRandomColorMix));
#endif
}

void ORMBandPass::RandomDisplayPush() {
#if IPLUG_EDITOR
  if (!GetUI())
    return;
  if (mPadL)
    mPadL->SetRandomDeltas(mRandomDeltas.freqOct[0], mRandomDeltas.bwOct[0]);
  if (mPadR)
    mPadR->SetRandomDeltas(mRandomDeltas.freqOct[1], mRandomDeltas.bwOct[1]);
  if (mGainSliderL)
    mGainSliderL->SetRandomDeltaDb(mRandomDeltas.gainDb[0]);
  if (mGainSliderR)
    mGainSliderR->SetRandomDeltaDb(mRandomDeltas.gainDb[1]);
  if (mMixSlider)
    mMixSlider->SetRandomDeltaMix(mRandomDeltas.mix);
#endif
}

void ORMBandPass::ApplyMonoDisplay(bool mono) {
  mMonoDisplay = mono;
#if IPLUG_EDITOR
  if (mPadR)
    mPadR->SetGhost(mono);
  if (mGainSliderR)
    mGainSliderR->SetGhost(mono);
  if (mBandR)
    mBandR->SetGhost(mono);
  if (mPadL)
    mPadL->SetSideLabel(orm::Tr(mono ? orm::kTxtMono : orm::kTxtLeft, orm::UILang()));
#endif
}

void ORMBandPass::UpdatePads() {
  if (mPadL) {
    mPadL->SetValueFromDelegate(GetParam(kFreqL)->GetNormalized(), 0);
    mPadL->SetValueFromDelegate(GetParam(kBwL)->GetNormalized(), 1);
    mPadL->SetSlopeIndex(GetParam(kSlopeL)->Int());
    mPadL->SetDirty(false);
  }
  if (mBandL) {
    mBandL->SetValueFromDelegate(GetParam(kFreqL)->GetNormalized(), 0);
    mBandL->SetValueFromDelegate(GetParam(kBwL)->GetNormalized(), 1);
    mBandL->SetDirty(false);
  }
  if (mPadR) {
    mPadR->SetValueFromDelegate(GetParam(kFreqR)->GetNormalized(), 0);
    mPadR->SetValueFromDelegate(GetParam(kBwR)->GetNormalized(), 1);
    mPadR->SetSlopeIndex(GetParam(kSlopeR)->Int());
    mPadR->SetDirty(false);
  }
  if (mBandR) {
    mBandR->SetValueFromDelegate(GetParam(kFreqR)->GetNormalized(), 0);
    mBandR->SetValueFromDelegate(GetParam(kBwR)->GetNormalized(), 1);
    mBandR->SetDirty(false);
  }
}

ParamSnapshot ORMBandPass::Snapshot() const {
  ParamSnapshot s;
  for (int i = 0; i < kNumParams; ++i)
    s[i] = GetParam(i)->Value();
  return s;
}

void ORMBandPass::ApplySnapshot(const ParamSnapshot &s) {
  for (int i = 0; i < kNumParams; ++i)
    SetParamFromEditor(i, s[i]);
  RefreshAfterEdit();
}

void ORMBandPass::PushUndo() {
  PushUndoSnapshot(Snapshot());
}

void ORMBandPass::PushUndoSnapshot(const ParamSnapshot &s) {
  if (!mUndoStack.empty() && mUndoStack.back() == s)
    return;
  mUndoStack.push_back(s);
  if (mUndoStack.size() > 100)
    mUndoStack.pop_front();
  mRedoStack.clear();
}

static constexpr double kGestureGapSec = 0.4;

void ORMBandPass::MaybePushGestureUndo() {
  using namespace std::chrono;
  const double now = duration<double>(steady_clock::now().time_since_epoch()).count();
  if (now - mLastUIChangeTime > kGestureGapSec)
    PushUndoSnapshot(mStableSnapshot);
  mLastUIChangeTime = now;
  mGesturePending = true;
}

void ORMBandPass::OnIdle() {
  mSpectrumL.TransmitData(*this);
  mSpectrumR.TransmitData(*this);

  // 仅在采样率/FFT 尺寸变化时重发 (如 UI 在 OnReset 之后才打开的场景)
  const double sr = GetSampleRate();
  if (sr != mSentSampleRate || kSpectrumFFTSize != mSentFFTSize) {
    mSentSampleRate = sr;
    mSentFFTSize = kSpectrumFFTSize;
    SendSpectrumConfig();
  }

#if IPLUG_EDITOR
  if (mRandomDeltaMailbox.consume(mRandomDeltas))
    RandomDisplayPush();

  const int nOuts = mObservedNOuts.load(std::memory_order_relaxed);
  if ((nOuts < 2) != mMonoDisplay)
    ApplyMonoDisplay(nOuts < 2);
#endif

  using namespace std::chrono;
  const double now = duration<double>(steady_clock::now().time_since_epoch()).count();
  if (mGesturePending && now - mLastUIChangeTime > kGestureGapSec) {
    mStableSnapshot = Snapshot();
    mGesturePending = false;
  }
  if (mFading) {
    const double t = (now - mFadeStartTime) / std::max(mFadeTime, 0.001);
    if (t >= 1.0) {
      mFading = false;
      ApplySnapshot(mFadeTo);
    } else {
      mInFadeApply = true;
      ApplySnapshot(MixSnapshots(mFadeFrom, mFadeTo, t));
      mInFadeApply = false;
    }
  }
}

void ORMBandPass::OnUIClose() {
  mPadL = mPadR = nullptr;
  mBandL = mBandR = nullptr;
  mMixSlider = nullptr;
  mGainSliderL = mGainSliderR = nullptr;
  for (ORMSlider *&s : mRandomRangeSlider)
    s = nullptr;
  for (ORMSlider *&s : mRandomSpeedSlider)
    s = nullptr;
  mRandomPicker = nullptr;
  mFadeSlider = nullptr;
  for (PresetSlotControl *&b : mSlotButtons)
    b = nullptr;
  mSettingsPanel = nullptr;
  mTextBindings.clear();
  mTooltipBindings.clear();
  mMonoDisplay = false;
}

void ORMBandPass::OnParentWindowResize(int width, int height) {
  if (auto *pGraphics = GetUI()) {
    const float platformScale = pGraphics->GetPlatformWindowScale();
    const float targetW = std::ceil(static_cast<float>(width) / platformScale);
    const float targetH = std::ceil(static_cast<float>(height) / platformScale);
    const float sx = targetW / static_cast<float>(pGraphics->Width());
    const float sy = targetH / static_cast<float>(pGraphics->Height());
    const float scale = std::max(sx, sy) * (1.f + 1e-4f);
    pGraphics->Resize(pGraphics->Width(), pGraphics->Height(), scale, false);
  }
}

bool ORMBandPass::ConstrainEditorResize(int &w, int &h) const {
  constexpr double kMinScale = DEFAULT_MIN_DRAW_SCALE;
  w = std::max(w, static_cast<int>(PLUG_WIDTH * kMinScale));

  const int wantH = static_cast<int>(std::lround(w * static_cast<double>(PLUG_HEIGHT) / PLUG_WIDTH));
  const bool ok = (h == wantH);
  h = wantH;
  return ok;
}

void ORMBandPass::MarkStateStable() {
  mStableSnapshot = Snapshot();
  mGesturePending = false;
}

void ORMBandPass::Undo() {
  if (mUndoStack.empty())
    return;
  mFading = false;
  mRedoStack.push_back(Snapshot());
  const ParamSnapshot s = mUndoStack.back();
  mUndoStack.pop_back();
  ApplySnapshot(s);
}

void ORMBandPass::Redo() {
  if (mRedoStack.empty())
    return;
  mFading = false;
  mUndoStack.push_back(Snapshot());
  const ParamSnapshot s = mRedoStack.back();
  mRedoStack.pop_back();
  ApplySnapshot(s);
}

void ORMBandPass::SaveToSlot(int idx) {
  if (idx < 0 || idx >= kNumPresets)
    return;
  mPresets[idx] = Snapshot();
}

void ORMBandPass::LoadSlot(int idx) {
  if (idx < 0 || idx >= kNumPresets)
    return;
  PushUndo();
  mCurrentPreset = idx;
  StartFade(mPresets[idx]);
}

void ORMBandPass::RestoreDefault(int idx) {
  if (idx < 0 || idx >= kNumPresets)
    return;
  mPresets[idx] = mDefaultSnapshot;
  if (idx == mCurrentPreset) {
    mFading = false;
    PushUndo();
    ApplySnapshot(mDefaultSnapshot);
  }
}

void ORMBandPass::SwapSlots(int posA, int posB) {
  if (posA == posB)
    return;
  if (posA < 0 || posA >= kNumPresets || posB < 0 || posB >= kNumPresets)
    return;
  std::swap(mSlotNumber[posA], mSlotNumber[posB]);
  RefreshSlotLabels();
}

void ORMBandPass::RefreshSlotLabels() {
  for (int i = 0; i < kNumPresets; ++i) {
    if (!mSlotButtons[i])
      continue;
    char buf[8];
    snprintf(buf, sizeof(buf), "%d", mSlotNumber[i] + 1);
    mSlotButtons[i]->SetSlotLabel(buf);
  }
}

void ORMBandPass::SaveFile() {
  if (!GetUI())
    return;
  mDialogFileName.Set("ORMBandPass Presets");
  mDialogPath.Set("");
  GetUI()->PromptForFile(mDialogFileName, mDialogPath, EFileAction::Save, "json",
                         [this](const WDL_String &fileName, const WDL_String &path) {
                           if (fileName.GetLength() == 0)
                             return;
                           std::string full = fileName.Get();
                           if (full.size() < 5 || full.compare(full.size() - 5, 5, ".json") != 0)
                             full += ".json";
                           std::string err;
                           WritePresetFileTo(full, err);
                           if (!err.empty() && GetUI())
                             GetUI()->ShowMessageBox(err.c_str(), "Save Failed", kMB_OK);
                         });
}

void ORMBandPass::LoadFile() {
  if (!GetUI())
    return;
  mDialogFileName.Set("");
  GetUI()->PromptForFile(mDialogFileName, mDialogPath, EFileAction::Open, "json",
                         [this](const WDL_String &fileName, const WDL_String &path) {
                           if (fileName.GetLength() == 0)
                             return;
                           std::string err;
                           ReadPresetFileFrom(fileName.Get(), err);
                           if (!err.empty() && GetUI())
                             GetUI()->ShowMessageBox(err.c_str(), "Load Failed", kMB_OK);
                         });
}

void ORMBandPass::WritePresetFileTo(const std::string &path, std::string &err) {
  PresetFileData data;
  for (const auto &p : mPresets) {
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

void ORMBandPass::ReadPresetFileFrom(const std::string &path, std::string &err) {
  PresetFileData data;
  if (!ReadPresetFile(path, data, err))
    return;

  if ((int)data.presets.size() != kNumPresets) {
    err = "Preset count mismatch (expected 24)";
    return;
  }
  const auto checkLen = [&](const std::vector<double> &e) -> bool {
    return (int)e.size() == kNumParams;
  };
  for (const auto &e : data.presets)
    if (!checkLen(e)) {
      err = "Preset parameter count mismatch (expected " + std::to_string(kNumParams) + ")";
      return;
    }
  if (!data.currentValues.empty() && !checkLen(data.currentValues)) {
    err = "Current values count mismatch (expected " + std::to_string(kNumParams) + ")";
    return;
  }

  const auto toSnapshot = [&](const std::vector<double> &e) -> ParamSnapshot {
    ParamSnapshot s = mDefaultSnapshot;
    const int n = std::min((int)e.size(), (int)kNumParams);
    for (int i = 0; i < n; ++i)
      s[i] = e[i];
    return s;
  };

  PushUndo();
  mFading = false;

  for (int i = 0; i < kNumPresets; ++i)
    mPresets[i] = toSnapshot(data.presets[i]);
  mCurrentPreset = std::clamp(data.currentPreset, 0, kNumPresets - 1);

  ParamSnapshot cur = data.currentValues.empty() ? mDefaultSnapshot : toSnapshot(data.currentValues);
  ApplySnapshot(cur);

  mFadePos = std::clamp(data.fadePos, 0.0, (double)(kNumQuick - 1));
  if (mFadeSlider) {
    mFadeSlider->SetValue((float)(mFadePos / (kNumQuick - 1.0)));
    mFadeSlider->SetDirty(false);
  }

  for (int i = 0; i < kNumPresets; ++i)
    mSlotNumber[i] = i;
  RefreshSlotLabels();

  err.clear();
}

void ORMBandPass::OnDragBegin(int src) {
  mDragSourceSlot = src;
  mDragTargetSlot = -1;
}

int ORMBandPass::HitTestSlot(float x, float y) {
  for (int i = 0; i < kNumPresets; ++i)
    if (mSlotButtons[i] && mSlotButtons[i]->GetWidgetBounds().Contains(x, y))
      return i;
  return -1;
}

void ORMBandPass::OnDragMove(float x, float y) {
  if (mDragSourceSlot < 0)
    return;
  int target = HitTestSlot(x, y);
  if (target == mDragSourceSlot)
    target = -1;
  if (target == mDragTargetSlot)
    return;

  if (mDragTargetSlot >= 0 && mSlotButtons[mDragTargetSlot])
    mSlotButtons[mDragTargetSlot]->SetDragTarget(false);
  mDragTargetSlot = target;
  if (mDragTargetSlot >= 0 && mSlotButtons[mDragTargetSlot])
    mSlotButtons[mDragTargetSlot]->SetDragTarget(true);
}

void ORMBandPass::OnDragDrop(int src, float x, float y) {
  if (mDragTargetSlot >= 0 && mSlotButtons[mDragTargetSlot])
    mSlotButtons[mDragTargetSlot]->SetDragTarget(false);
  mDragTargetSlot = -1;

  const int target = HitTestSlot(x, y);
  mDragSourceSlot = -1;
  if (target >= 0 && target != src)
    SwapSlots(src, target);
}

void ORMBandPass::ApplyLtoRParams() {
  for (const auto &pr : kLRParamPairs) {
    SetParamFromEditor(pr.second, GetParam(pr.first)->Value());
#if IPLUG_EDITOR
    if (GetUI())
      SendParameterValueFromDelegate(pr.second, GetParam(pr.second)->GetNormalized(), true);
#endif
  }
}

void ORMBandPass::ApplyRtoLParams() {
  for (const auto &pr : kLRParamPairs) {
    SetParamFromEditor(pr.first, GetParam(pr.second)->Value());
#if IPLUG_EDITOR
    if (GetUI())
      SendParameterValueFromDelegate(pr.first, GetParam(pr.first)->GetNormalized(), true);
#endif
  }
}

void ORMBandPass::CopyLtoR() {
  mFading = false;
  PushUndo();
  ApplyLtoRParams();
  RefreshAfterEdit();
}

void ORMBandPass::CopyRtoL() {
  mFading = false;
  PushUndo();
  ApplyRtoLParams();
  RefreshAfterEdit();
}

void ORMBandPass::FlipLR() {
  mFading = false;
  PushUndo();
  const double fL = GetParam(kFreqL)->Value(), bL = GetParam(kBwL)->Value(), gL = GetParam(kGainL)->Value(),
               sL = GetParam(kSlopeL)->Value();
  SetParamFromEditor(kFreqL, GetParam(kFreqR)->Value());
  SetParamFromEditor(kBwL, GetParam(kBwR)->Value());
  SetParamFromEditor(kGainL, GetParam(kGainR)->Value());
  SetParamFromEditor(kSlopeL, GetParam(kSlopeR)->Value());
  SetParamFromEditor(kFreqR, fL);
  SetParamFromEditor(kBwR, bL);
  SetParamFromEditor(kGainR, gL);
  SetParamFromEditor(kSlopeR, sL);
  RefreshAfterEdit();
}

void ORMBandPass::MirrorLinkedParams(int paramIdx) {
  if (GetParam(kLink)->Value() < 0.5)
    return;

  if (paramIdx == kLink) {
    ApplyLtoRParams();
    return;
  }

  const int toLeft = LeftMirrorOf(paramIdx);
  if (toLeft >= 0 && std::fabs(GetParam(toLeft)->Value() - GetParam(paramIdx)->Value()) >= 1e-9) {
    SetParamFromEditor(toLeft, GetParam(paramIdx)->Value());
#if IPLUG_EDITOR
    if (GetUI())
      SendParameterValueFromDelegate(toLeft, GetParam(toLeft)->GetNormalized(), true);
#endif
  }

  const int mirror = RightMirrorOf(paramIdx);
  if (mirror >= 0 && std::fabs(GetParam(mirror)->Value() - GetParam(paramIdx)->Value()) >= 1e-9) {
    SetParamFromEditor(mirror, GetParam(paramIdx)->Value());
#if IPLUG_EDITOR
    if (GetUI())
      SendParameterValueFromDelegate(mirror, GetParam(mirror)->GetNormalized(), true);
#endif
  }
}

ParamSnapshot ORMBandPass::MixSnapshots(const ParamSnapshot &a, const ParamSnapshot &b, double t) const {
  ParamSnapshot out;
  for (int i = 0; i < kNumParams; ++i) {
    const IParam *p = GetParam(i);
    const double na = p->ToNormalized(a[i]);
    const double nb = p->ToNormalized(b[i]);
    out[i] = p->FromNormalized(na + (nb - na) * t);
  }
  return out;
}

ParamSnapshot ORMBandPass::InterpolatePresets(double pos) {
  const int i0 = std::clamp(static_cast<int>(std::floor(pos)), 0, kNumQuick - 1);
  const int i1 = std::min(i0 + 1, kNumQuick - 1);
  const double t = std::clamp(pos - i0, 0.0, 1.0);
  return MixSnapshots(mPresets[mSlotNumber[i0]], mPresets[mSlotNumber[i1]], t);
}

void ORMBandPass::OnFadeDrag(double normalizedPos) {
  mFading = false;
  ApplySnapshot(InterpolatePresets(normalizedPos * (kNumQuick - 1)));
}

void ORMBandPass::StartFade(const ParamSnapshot &to) {
  if (mFadeTime <= 0.001 || !GetUI()) {
    ApplySnapshot(to);
    return;
  }
  mFadeFrom = Snapshot();
  mFadeTo = to;
  mFadeStartTime = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
  mFading = true;
}

void ORMBandPass::ApplyLanguage() {
  for (auto &binding : mTextBindings)
    if (binding.second)
      binding.second(orm::Tr(binding.first, orm::UILang()));
  ApplyTooltips();
#if IPLUG_EDITOR
  if (GetUI()) {
    GetUI()->SetAllControlsDirty();
    GetUI()->UpdateTooltips();
  }
#endif
}

void ORMBandPass::ApplyTooltips() {
  for (auto &binding : mTooltipBindings)
    if (binding.first)
      binding.first->SetTooltip(orm::Tr(binding.second, orm::UILang()));
}

void ORMBandPass::ApplyTheme() {
  ThemeMode() = mThemeMode;
  RefreshThemeColors();
}

void ORMBandPass::RefreshThemeColors() {
#if IPLUG_EDITOR
  if (GetUI()) {
    if (IControl *pBG = GetUI()->GetBackgroundControl()) {
      if (IPanelControl *pPanel = dynamic_cast<IPanelControl *>(pBG))
        pPanel->SetPattern(COL_100());
    }
    GetUI()->SetAllControlsDirty();
  }
#endif
}

void ORMBandPass::ToggleSettingsPanel() {
  if (mSettingsPanel)
    mSettingsPanel->SetVisible(mSettingsPanel->IsHidden());
}
