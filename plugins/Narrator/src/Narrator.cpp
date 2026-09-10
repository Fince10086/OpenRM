#include "Narrator.h"
#include "IPlug_include_in_plug_src.h"
#include "IControls.h"
#include "ICornerResizerControl.h"
#include "Theme.h"
#include "controls/UiUtils.h"
#include "../../common/controls/ThemeCornerResizer.h"
#include "controls/FlatButton.h"
#include "controls/ORMSlider.h"
#include "../../common/controls/SectionTitleControl.h"
#include "controls/SettingsPanelControl.h"
#include "controls/PianoKeyboardControl.h"
#include "controls/PhraseEditorControl.h"
#include "controls/UtteranceTimelineControl.h"
#include "controls/CandidatePanelControl.h"
#include "SettingsFileIO.h"

#include <cstring>
#include <cstdio>
#include <chrono>
#include <functional>
#include <algorithm>
#include <string>
#include <cmath>

#if defined(OS_MAC)
#include <CoreFoundation/CoreFoundation.h>
#elif defined(OS_WIN)
#include <windows.h>
#endif

static constexpr double kGestureGapSec = 0.4;

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

ORMNarrator::ORMNarrator(const InstanceInfo &info) : Plugin(info, MakeConfig(kNumParams, 1)) {
  // 读取全局 UI 偏好
  {
    SettingsData s;
    if (LoadSettings(s)) {
      if (s.lang >= 0 && s.lang < orm::kNumLanguages)
        orm::UILang() = s.lang;
      ThemeHue() = s.hue;
      ThemeSatMax() = s.satMax;
      mThemeMode = s.themeMode;
      ThemeMode() = s.themeMode;
    }
  }

  GetParam(kEngine)->InitEnum("Engine", 0, {"SAM", "DEC", "SP", "TMS", "TSI"});
  GetParam(kMapMode)->InitEnum("Map Mode", 0, {"PITCH", "WORDS"});
  GetParam(kBaseKey)->InitDouble("Base Key", 48., 0., 127., 1., "");
  GetParam(kSamPitch)->InitDouble("SAM Pitch", 64., 0., 255., 1., "");
  GetParam(kSamSpeed)->InitDouble("SAM Speed", 72., 1., 255., 1., "");
  GetParam(kSamMouth)->InitDouble("Mouth", 128., 0., 255., 1., "");
  GetParam(kSamThroat)->InitDouble("Throat", 128., 0., 255., 1., "");
  GetParam(kTmsSpeed)->InitDouble("TMS Speed", 72., 1., 255., 1., "");
  GetParam(kTmsPitch)->InitDouble("TMS Pitch", 64., 0., 255., 1., "");
  GetParam(kTmsBank)->InitEnum("Voice", 0,
                               {"MIL", "TI99", "ACORN", "S&S", "CLOCK"});
  GetParam(kTsiSpeed)->InitDouble("TSI Rate", 72., 1., 255., 1., "");
  GetParam(kTsiBank)->InitEnum("TSI Voice", 0,
                               {"BZ", "F2", "C0", "C1", "C2", "C3", "C4", "C5", "C6"});
  GetParam(kSp0256Speed)->InitDouble("SP0256 Rate", 72., 1., 255., 1., "");
  GetParam(kSp0256Voice)->InitEnum("SP0256 Voice", 0, {"Text", "Phoneme"});
  GetParam(kDectalkVoice)->InitEnum("DT Voice", 0,
                                    {"PAUL", "BETTY", "HARRY", "FRANK", "DENNIS", "KIT", "URS", "RITA", "WENDY"});
  GetParam(kDectalkRate)->InitDouble("DT Rate", 180., 75., 600., 1., "wpm");
  GetParam(kDectalkPitch)->InitDouble("DT Pitch", 0., 0., 400., 1., "Hz");
  GetParam(kAttack)->InitDouble("Attack", 5., 1., 500., 1., "ms");
  GetParam(kRelease)->InitDouble("Release", 120., 1., 2000., 1., "ms");
  GetParam(kMono)->InitBool("Mono", true);
  GetParam(kGain)->InitDouble("Output", -12., -24., 6., 0.1, "dB");
  GetParam(kLoop)->InitBool("Loop", false);

  for (auto &f : mUIHoldState)
    f.store(1, std::memory_order_relaxed);

  mStableSnapshot = Snapshot();

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
    sysFontOk = pGraphics->LoadFont(kFontSystem, "Microsoft YaHei", ETextStyle::Normal);
    if (!sysFontOk)
      sysFontOk = pGraphics->LoadFont(kFontSystem, "Microsoft YaHei UI", ETextStyle::Normal);
    if (!sysFontOk)
      sysFontOk = pGraphics->LoadFont(kFontSystem, "Segoe UI", ETextStyle::Normal);
    if (!sysFontOk) {
      static const char *kSysFontFiles[] = {
          "C:\\Windows\\Fonts\\msyh.ttc",
          "C:\\Windows\\Fonts\\msyh.ttf",
          "C:\\Windows\\Fonts\\Deng.ttf",
          "C:\\Windows\\Fonts\\Nsimsun.ttf",
      };
      for (const char *p : kSysFontFiles)
        if ((sysFontOk = loadFontFile(pGraphics, kFontSystem, p)))
          break;
    }
#endif
    if (!sysFontOk)
      sysFontOk = pGraphics->LoadFont(kFontSystem, MIXED_FN);

    const IVStyle style = MakeORMStyle();
    IVStyle btnStyle = MakeButtonStyle();
    btnStyle.labelText = IText(20, COL_900(), kFontSemiBold, EAlign::Center, EVAlign::Middle);
    btnStyle.valueText = IText(20, COL_900(), kFontSemiBold, EAlign::Center, EVAlign::Middle);
    IVStyle toggleStyle = btnStyle;
    toggleStyle.showLabel = false;
    toggleStyle.showValue = false;

    mTextBindings.clear();
    mTooltipBindings.clear();
    auto bindText = [this](int id, std::function<void(const char *)> apply) {
      mTextBindings.push_back({id, std::move(apply)});
    };
    auto bindTip = [this](IControl *c, int id) { mTooltipBindings.push_back({c, id}); };

    // 布局常量
    constexpr float kOptL = 20.f;
    constexpr float kOptR = 382.f;
    constexpr float kPhraseL = 398.f;
    constexpr float kPhraseR = 760.f;
    constexpr float kBottomL = 20.f;
    constexpr float kBottomR = 588.f;
    constexpr float kTitleX = 604.f;
    constexpr float kRightR = 760.f;

    constexpr float kBtnH = 26.f;
    constexpr float kBtnFontSize = 20.f;

    // 映射模式
    mMapSegment = new FlatSegmentControl(
        IRECT(kOptL, 16.f, kOptR, 16.f + kBtnH),
        std::vector<std::string>{orm::Tr(orm::kTxtMapPitch, orm::UILang()),
                                 orm::Tr(orm::kTxtMapWords, orm::UILang())},
        [this](int idx) { SetMapModeFromUI(idx); },
        GetParam(kMapMode)->Int(), kBtnFontSize);
    pGraphics->AttachControl(mMapSegment);

    // 引擎选择
    mEngineSegment = new FlatSegmentControl(
        IRECT(kOptL, 46.f, kOptR, 46.f + kBtnH),
        std::vector<std::string>{"SAM", "DEC", "SP", "TMS", "TSI"},
        [this](int idx) { SetEngineFromUI(idx); },
        GetParam(kEngine)->Int(), kBtnFontSize);
    pGraphics->AttachControl(mEngineSegment);

    // SAM 文本/音素切换
    mPhoneticSegment = new FlatSegmentControl(
        IRECT(kOptL, 76.f, kOptR, 76.f + kBtnH),
        std::vector<std::string>{orm::Tr(orm::kTxtText, orm::UILang()),
                                 orm::Tr(orm::kTxtPhonetic, orm::UILang())},
        [this](int idx) { SetPhoneticMode(idx == 1); }, mPhonetic ? 1 : 0, kBtnFontSize);
    pGraphics->AttachControl(mPhoneticSegment);

    // TMS 音色/词库
    mVoiceSegment = new FlatSegmentControl(
        IRECT(kOptL, 76.f, kOptR, 76.f + kBtnH),
        std::vector<std::string>{"MIL", "TI99", "ACORN", "S&S", "CLOCK"},
        [this](int idx) { SetVoiceFromUI(idx); },
        GetParam(kTmsBank)->Int(), kBtnFontSize);
    pGraphics->AttachControl(mVoiceSegment);

    // TSI 子集
    mTsiVoiceSegment = new FlatSegmentControl(
        IRECT(kOptL, 76.f, kOptR, 76.f + kBtnH),
        std::vector<std::string>{"BZ", "F2", "C0", "C1", "C2", "C3", "C4", "C5", "C6"},
        [this](int idx) { SetTsiVoiceFromUI(idx); },
        GetParam(kTsiBank)->Int(), kBtnFontSize);
    pGraphics->AttachControl(mTsiVoiceSegment);

    // SP0256 输入模式
    mSp0256VoiceSegment = new FlatSegmentControl(
        IRECT(kOptL, 76.f, kOptR, 76.f + kBtnH),
        std::vector<std::string>{orm::Tr(orm::kTxtText, orm::UILang()),
                                 orm::Tr(orm::kTxtPhonetic, orm::UILang())},
        [this](int idx) { SetSp0256VoiceFromUI(idx); },
        GetParam(kSp0256Voice)->Int(), kBtnFontSize);
    pGraphics->AttachControl(mSp0256VoiceSegment);

    // DECTALK 音色
    mDectalkVoiceSegment = new FlatSegmentControl(
        IRECT(kOptL, 76.f, kOptR, 76.f + kBtnH),
        std::vector<std::string>{"NP", "NB", "NH", "NF", "ND", "NK", "NU", "NR", "NW"},
        [this](int idx) { SetDectalkVoiceFromUI(idx); },
        GetParam(kDectalkVoice)->Int(), kBtnFontSize);
    pGraphics->AttachControl(mDectalkVoiceSegment);
    bindTip(mDectalkVoiceSegment, orm::kTxtTipDectalkVoice);

    struct SliderDef {
      int param;
      int txtId;
      int tipId;
      const char *fallback;
    };
    static const SliderDef kSliders[] = {
        {kSamPitch, orm::kTxtPitch, orm::kTxtTipPitch, "Pitch"},
        {kSamSpeed, orm::kTxtSpeed, orm::kTxtTipSpeed, "Speed"},
        {kSamMouth, orm::kTxtMouth, orm::kTxtTipMouthThroat, "Mouth"},
        {kSamThroat, orm::kTxtThroat, orm::kTxtTipMouthThroat, "Throat"},
        {kAttack, orm::kTxtAttack, orm::kTxtTipAttackRelease, "Attack"},
        {kRelease, orm::kTxtRelease, orm::kTxtTipAttackRelease, "Release"},
    };
    const int nSliders = (int)(sizeof(kSliders) / sizeof(kSliders[0]));
    for (int i = 0; i < nSliders; ++i) {
      const float y = 110.f + i * 48.f;
      ORMSlider *sl = new ORMSlider(IRECT(kOptL, y, kOptR, y + 40.f), kSliders[i].param,
                                    kSliders[i].fallback, style, EDirection::Horizontal);
      mParamSliders[i] = sl;
      pGraphics->AttachControl(sl);
      sl->SetHeaderFont(kFontRegular);
      bindText(kSliders[i].txtId, [sl](const char *s) { sl->SetHeaderLabel(s); });
      if (kSliders[i].tipId >= 0)
        bindTip(sl, kSliders[i].tipId);
    }

    // 语句文本区
    mPhraseEditor = new PhraseEditorControl(
        IRECT(kPhraseL, 16.f, kPhraseR, 84.f),
        PhraseEditorControl::Hooks{
            [this]() -> std::string {
              std::lock_guard<std::mutex> lock(mTextMutex);
              if (GetParam(kMapMode)->Int() == 1 && mUISelected >= 0) {
                auto it = mBank.find(mUISelected);
                if (it != mBank.end())
                  return it->second.text;
                return "";
              }
              return mPhraseText;
            },
            [this](const std::string &s) { SetPhraseText(s); },
        });
    pGraphics->AttachControl(mPhraseEditor);
    bindTip(mPhraseEditor, orm::kTxtTipPhrase);

    IVButtonControl *playBtn =
        MakeMomentary(IRECT(kPhraseL, 90.f, kPhraseL + 96.f, 90.f + kBtnH), [this](IControl *) {
          if (GetParam(kMapMode)->Int() == 1 && mUISelected >= 0)
            OnNoteOnFromUI(mUISelected, false);
          else
            OnNoteOnFromUI(std::clamp(GetParam(kBaseKey)->Int(), 0, 127), false);
        }, "PLAY", btnStyle);
    pGraphics->AttachControl(playBtn);
    bindText(orm::kTxtPlay, [playBtn](const char *s) {
      playBtn->SetLabelStr(s);
      playBtn->SetDirty(false);
    });

    IVButtonControl *clearBtn =
        MakeMomentary(IRECT(kPhraseL + 102.f, 90.f, kPhraseL + 198.f, 90.f + kBtnH), [this](IControl *) {
          if (GetParam(kMapMode)->Int() == 1 && mUISelected >= 0) {
            std::lock_guard<std::mutex> lock(mTextMutex);
            mBank.erase(mUISelected);
          } else {
            SetPhraseText("");
          }
#if IPLUG_EDITOR
          if (GetUI())
            GetUI()->SetAllControlsDirty();
#endif
        }, "CLEAR", btnStyle);
    pGraphics->AttachControl(clearBtn);
    bindText(orm::kTxtClear, [clearBtn](const char *s) {
      clearBtn->SetLabelStr(s);
      clearBtn->SetDirty(false);
    });

    // 候选词库 / 说明书
    constexpr float kCandT = 126.f;
    constexpr float kCandB = 456.f;
    mCandidatePanel = new CandidatePanelControl(
        IRECT(kPhraseL, kCandT, kPhraseR, kCandB),
        [this](const std::string &word) {
          std::string text;
          {
            std::lock_guard<std::mutex> lock(mTextMutex);
            if (GetParam(kMapMode)->Int() == 1 && mUISelected >= 0) {
              auto it = mBank.find(mUISelected);
              if (it != mBank.end())
                text = it->second.text;
            } else {
              text = mPhraseText;
            }
          }
          const bool isSamPhonetic = (GetParam(kEngine)->Int() == kEngineSAM && mPhonetic);
          if (!isSamPhonetic && !text.empty() && text.back() != ' ' && text.back() != '\n') {
            text += " ";
          }
          text += word;
          SetPhraseText(text);
#if IPLUG_EDITOR
          if (GetUI())
            GetUI()->SetAllControlsDirty();
#endif
        });
    mCandidatePanel->SetEngine(GetParam(kEngine)->Int());
    mCandidatePanel->SetTmsBank(GetParam(kTmsBank)->Int());
    mCandidatePanel->SetPhoneticMode(mPhonetic);
    pGraphics->AttachControl(mCandidatePanel);

    // 数值格式
    auto intFmt = [](WDL_String &ds, const IParam *p) {
      if (!p)
        return;
      char buf[32];
      snprintf(buf, sizeof(buf), "%d", p->Int());
      ds.Set(buf);
    };
    for (int i : {0, 2, 3}) {
      mParamSliders[i]->SetValueFormatter(
          [intFmt, this](WDL_String &ds, const IParam *p) {
            if (p && p == GetParam(kDectalkPitch)) {
              if (p->Value() <= 0.5) {
                ds.Set(orm::Tr(orm::kTxtNative, orm::UILang()));
              } else {
                char buf[32];
                snprintf(buf, sizeof(buf), "%d Hz", p->Int());
                ds.Set(buf);
              }
              return;
            }
            intFmt(ds, p);
          });
    }
    mParamSliders[1]->SetValueFormatter([this](WDL_String &ds, const IParam *p) {
      if (!p)
        return;
      if (p == GetParam(kTmsSpeed)) {
        char buf[32];
        snprintf(buf, sizeof(buf), "x%.2f", p->Value() / 72.0);
        ds.Set(buf);
      } else if (p == GetParam(kDectalkRate)) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%d wpm", p->Int());
        ds.Set(buf);
      } else {
        char buf[32];
        snprintf(buf, sizeof(buf), "%d", p->Int());
        ds.Set(buf);
      }
    });
    mParamSliders[4]->SetValueFormatter([](WDL_String &ds, const IParam *p) {
      char buf[32];
      snprintf(buf, sizeof(buf), "%.1f ms", p->Value());
      ds.Set(buf);
    });
    mParamSliders[5]->SetValueFormatter([](WDL_String &ds, const IParam *p) {
      char buf[32];
      snprintf(buf, sizeof(buf), "%.1f ms", p->Value());
      ds.Set(buf);
    });

    // 右下角功能区
    constexpr float kIconRowH = 26.f;
    constexpr float kIconRowB = 610.f;
    constexpr float kIconRowT = kIconRowB - kIconRowH;
    constexpr float kIconGap = 4.f;
    constexpr float kIconBtnW = (kRightR - kTitleX - 3.f * kIconGap) / 4.f;
    constexpr float kIconStep = kIconBtnW + kIconGap;

    constexpr float kModeRowB = kIconRowT - kIconGap;
    constexpr float kModeRowT = kModeRowB - kIconRowH;
    constexpr float kModeBtnW = 2.f * kIconBtnW + kIconGap;

    constexpr float kSliderRowH = 40.f;
    constexpr float kBaseRowB = kModeRowT - kIconGap;
    constexpr float kBaseRowT = kBaseRowB - kSliderRowH;
    constexpr float kOutRowB = kBaseRowT - kIconGap;
    constexpr float kOutRowT = kOutRowB - kSliderRowH;

    // 时间线
    mTimeline = new UtteranceTimelineControl(IRECT(kBottomL, kOutRowT, kBottomR, kModeRowB));
    pGraphics->AttachControl(mTimeline, kCtrlTagTimeline);
    bindTip(mTimeline, orm::kTxtTipTimeline);

    pGraphics->AttachControl(
        MakeIconMomentary(IRECT(kTitleX, kIconRowT, kTitleX + kIconBtnW, kIconRowB),
                          [this](IControl *) { Undo(); }, kIconUndo));
    pGraphics->AttachControl(
        MakeIconMomentary(IRECT(kTitleX + kIconStep, kIconRowT, kTitleX + kIconStep + kIconBtnW, kIconRowB),
                          [this](IControl *) { Redo(); }, kIconRedo));
    pGraphics->AttachControl(
        MakeIconMomentary(IRECT(kTitleX + 2.f * kIconStep, kIconRowT, kTitleX + 2.f * kIconStep + kIconBtnW, kIconRowB),
                          [](IControl *) {}, kIconSave));
    pGraphics->AttachControl(
        MakeIconMomentary(IRECT(kTitleX + 3.f * kIconStep, kIconRowT, kRightR, kIconRowB),
                          [](IControl *) {}, kIconLoad));

    // 单音/复音 + 循环
    FlatCycleButton *monoBtn =
        new FlatCycleButton(IRECT(kTitleX, kModeRowT, kTitleX + kModeBtnW, kModeRowB), kMono,
                            {orm::Tr(orm::kTxtPoly, orm::UILang()), orm::Tr(orm::kTxtMono, orm::UILang())},
                            kBtnFontSize);
    pGraphics->AttachControl(monoBtn);
    bindText(orm::kTxtMono, [monoBtn](const char *) {
      monoBtn->SetLabels({orm::Tr(orm::kTxtPoly, orm::UILang()), orm::Tr(orm::kTxtMono, orm::UILang())});
    });
    FlatToggleControl *loopToggle =
        new FlatToggleControl(IRECT(kTitleX + kModeBtnW + kIconGap, kModeRowT, kRightR, kModeRowB), kLoop, " ",
                               toggleStyle, orm::Tr(orm::kTxtLoop, orm::UILang()), orm::Tr(orm::kTxtLoop, orm::UILang()));
    pGraphics->AttachControl(loopToggle);
    bindText(orm::kTxtLoop, [loopToggle](const char *s) {
      loopToggle->SetOnText(s);
      loopToggle->SetOffText(s);
    });

    // Base Key
    ORMSlider *baseKeySlider =
        new ORMSlider(IRECT(kTitleX, kBaseRowT, kRightR, kBaseRowB), kBaseKey,
                      orm::Tr(orm::kTxtBaseKey, orm::UILang()), style, EDirection::Horizontal);
    pGraphics->AttachControl(baseKeySlider);
    baseKeySlider->SetHeaderFont(kFontRegular);
    baseKeySlider->SetValueFormatter([](WDL_String &ds, const IParam *p) {
      static const char *kNames[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
      const int note = p->Int();
      char buf[32];
      snprintf(buf, sizeof(buf), "%s%d (%d)", kNames[note % 12], note / 12 - 1, note);
      ds.Set(buf);
    });
    bindText(orm::kTxtBaseKey, [baseKeySlider](const char *s) { baseKeySlider->SetHeaderLabel(s); });
    bindTip(baseKeySlider, orm::kTxtTipBaseKey);

    // Output
    mParamSliders[6] = new ORMSlider(IRECT(kTitleX, kOutRowT, kRightR, kOutRowB), kGain,
                                     orm::Tr(orm::kTxtOutput, orm::UILang()), style, EDirection::Horizontal);
    pGraphics->AttachControl(mParamSliders[6]);
    mParamSliders[6]->SetHeaderFont(kFontRegular);
    mParamSliders[6]->SetValueFormatter([](WDL_String &ds, const IParam *p) {
      char buf[32];
      snprintf(buf, sizeof(buf), "%.1f dB", p->Value());
      ds.Set(buf);
    });
    bindText(orm::kTxtOutput, [this](const char *s) {
      if (mParamSliders[6])
        mParamSliders[6]->SetHeaderLabel(s);
    });

    // 键盘
    mKeyboard = new PianoKeyboardControl(
        IRECT(kBottomL, kIconRowT, kBottomR, 684),
        PianoKeyboardControl::Hooks{
            [this](int note) { OnNoteOnFromUI(note); },
            [this](int note) { OnNoteOffFromUI(note); },
            [this]() { return std::clamp(GetParam(kBaseKey)->Int(), 0, 127); },
        },
        PianoKeyboardControl::kLowNoteDefault, PianoKeyboardControl::kHighNoteDefault);
    pGraphics->AttachControl(mKeyboard, kCtrlTagKeyboard);
    bindTip(mKeyboard, orm::kTxtTipKeyboard);

    IText ormText(32, COL_900(), kFontBold, EAlign::Near, EVAlign::Bottom);
    SectionTitleControl *ormTitle = new SectionTitleControl(IRECT(kTitleX, 618, kTitleX + 120, 652), "ORM", ormText, 0);
    pGraphics->AttachControl(ormTitle);
    IRECT ormInk(kTitleX, 618, kTitleX + 120, 652);
    pGraphics->MeasureText(ormText, "ORM", ormInk);
    const float gearL = ormInk.R + 8.f;
    const float gearR = gearL + (ormInk.B - ormInk.T);
    pGraphics->AttachControl(new SettingsMenuButton(IRECT(gearL, ormInk.T, gearR, ormInk.B),
                                                    [this]() { ToggleSettingsPanel(); }));
    pGraphics->AttachControl(new SectionTitleControl(IRECT(kTitleX, 650, kRightR, 684), "Narrator",
                                                     IText(32, COL_900(), kFontBold, EAlign::Near, EVAlign::Middle),
                                                     0));
    pGraphics->AttachControl(new SectionTitleControl(IRECT(gearR + 8.f, 615, kRightR, 649), "v" PLUG_VERSION_STR,
                                                     IText(20, COL_500(), kFontRegular, EAlign::Near, EVAlign::Bottom),
                                                     1, 0));

    // 设置面板
    SettingsPanelControl::Hooks settingsHooks;
    settingsHooks.onLanguage = [this](int lang) {
      if (lang != orm::UILang()) {
        orm::UILang() = lang;
        ApplyLanguage();
        SaveSettingsToDisk();
      }
    };
    settingsHooks.onTheme = [this](int themeMode) {
      mThemeMode = themeMode;
      ApplyTheme();
      SaveSettingsToDisk();
    };
    settingsHooks.onHue = [this](int hue) {
      ThemeHue() = hue;
      RefreshThemeColors();
      SaveSettingsToDisk();
    };
    settingsHooks.onSat = [this](int satMax) {
      ThemeSatMax() = satMax;
      RefreshThemeColors();
      SaveSettingsToDisk();
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
    mSettingsPanel = new SettingsPanelControl(IRECT(0.f, 0.f, (float)PLUG_WIDTH, (float)PLUG_HEIGHT), settingsHooks, false);
    mSettingsPanel->SetVisible(false);
    pGraphics->AttachControl(mSettingsPanel);

    RebindVoiceSliders(GetParam(kEngine)->Int());

    pGraphics->EnableTooltips(true);
    ApplyLanguage();
  };
#endif
}

void ORMNarrator::OnUIOpen() {
#if IPLUG_EDITOR
  Plugin::OnUIOpen();
#endif
  mStableSnapshot = Snapshot();
}

void ORMNarrator::OnNoteOnFromUI(int note, bool held) {
  mUIHoldState[(size_t) note].store(held ? 1 : 0, std::memory_order_relaxed);
  IMidiMsg msg;
  msg.MakeNoteOnMsg(note, 127, 0);
  SendMidiMsgFromUI(msg);
}

void ORMNarrator::OnNoteOffFromUI(int note) {
  IMidiMsg msg;
  msg.MakeNoteOffMsg(note, 0);
  SendMidiMsgFromUI(msg);
}

void ORMNarrator::SetPhraseText(const std::string &text) {
  {
    std::lock_guard<std::mutex> lock(mTextMutex);
    mPhraseText = text;
    if (GetParam(kMapMode)->Int() == 1 && mUISelected >= 0) {
      auto it = mBank.find(mUISelected);
      if (it != mBank.end()) {
        it->second.text = text;
        it->second.dirty = true;
      }
    }
  }
  mRenderDirty = true;
#if IPLUG_EDITOR
  if (mPhraseEditor)
    mPhraseEditor->RefreshText();
#endif
}

void ORMNarrator::SetPhoneticMode(bool phonetic) {
  {
    std::lock_guard<std::mutex> lock(mTextMutex);
    mPhonetic = phonetic;
    if (GetParam(kMapMode)->Int() == 1 && mUISelected >= 0) {
      auto it = mBank.find(mUISelected);
      if (it != mBank.end()) {
        it->second.phonetic = phonetic;
        it->second.dirty = true;
      }
    }
  }
  mRenderDirty = true;
  if (mPhoneticSegment)
    mPhoneticSegment->SetActive(phonetic ? 1 : 0);
  if (mCandidatePanel)
    mCandidatePanel->SetPhoneticMode(phonetic);
}

void ORMNarrator::SetEngineFromUI(int idx) {
  const int v = std::clamp(idx, 0, 4);
  MaybePushGestureUndo();
  GetParam(kEngine)->Set((double) v);
  InformHostOfParamChange(kEngine, GetParam(kEngine)->GetNormalized());
  mRenderDirty = true;
  {
    std::lock_guard<std::mutex> lock(mTextMutex);
    if (GetParam(kMapMode)->Int() == 1 && mUISelected >= 0) {
      auto it = mBank.find(mUISelected);
      if (it != mBank.end()) {
        it->second.engine = v;
        it->second.dirty = true;
      }
    }
  }
  if (mEngineSegment)
    mEngineSegment->SetActive(v);
  if (mCandidatePanel) {
    mCandidatePanel->SetEngine(v);
    mCandidatePanel->SetPhoneticMode(mPhonetic);
  }
  RebindVoiceSliders(v);
#if IPLUG_EDITOR
  if (GetUI())
    GetUI()->SetAllControlsDirty();
#endif
}

void ORMNarrator::SetVoiceFromUI(int idx) {
  const int v = std::clamp(idx, 0, kNumBanks - 1);
  MaybePushGestureUndo();
  GetParam(kTmsBank)->Set((double) v);
  InformHostOfParamChange(kTmsBank, GetParam(kTmsBank)->GetNormalized());
  mRenderDirty = true;
  {
    std::lock_guard<std::mutex> lock(mTextMutex);
    if (GetParam(kMapMode)->Int() == 1 && mUISelected >= 0) {
      auto it = mBank.find(mUISelected);
      if (it != mBank.end()) {
        it->second.tms.bank = v;
        it->second.dirty = true;
      }
    }
  }
  if (mVoiceSegment)
    mVoiceSegment->SetActive(v);
  if (mCandidatePanel)
    mCandidatePanel->SetTmsBank(v);
#if IPLUG_EDITOR
  if (GetUI())
    GetUI()->SetAllControlsDirty();
#endif
}

void ORMNarrator::SetTsiVoiceFromUI(int idx) {
  const int v = std::clamp(idx, 0, kNumS14001Sets - 1);
  MaybePushGestureUndo();
  GetParam(kTsiBank)->Set((double) v);
  InformHostOfParamChange(kTsiBank, GetParam(kTsiBank)->GetNormalized());
  mRenderDirty = true;
  {
    std::lock_guard<std::mutex> lock(mTextMutex);
    if (GetParam(kMapMode)->Int() == 1 && mUISelected >= 0) {
      auto it = mBank.find(mUISelected);
      if (it != mBank.end()) {
        it->second.tsi.bank = v;
        it->second.dirty = true;
      }
    }
  }
  if (mTsiVoiceSegment)
    mTsiVoiceSegment->SetActive(v);
#if IPLUG_EDITOR
  if (GetUI())
    GetUI()->SetAllControlsDirty();
#endif
}

void ORMNarrator::SetSp0256VoiceFromUI(int idx) {
  const int v = std::clamp(idx, 0, kNumSp0256 - 1);
  MaybePushGestureUndo();
  GetParam(kSp0256Voice)->Set((double) v);
  InformHostOfParamChange(kSp0256Voice, GetParam(kSp0256Voice)->GetNormalized());
  mRenderDirty = true;
  {
    std::lock_guard<std::mutex> lock(mTextMutex);
    if (GetParam(kMapMode)->Int() == 1 && mUISelected >= 0) {
      auto it = mBank.find(mUISelected);
      if (it != mBank.end()) {
        it->second.sp0256.variant = v;
        it->second.dirty = true;
      }
    }
  }
  if (mSp0256VoiceSegment)
    mSp0256VoiceSegment->SetActive(v);
#if IPLUG_EDITOR
  if (GetUI())
    GetUI()->SetAllControlsDirty();
#endif
}

void ORMNarrator::SetDectalkVoiceFromUI(int idx) {
  const int v = std::clamp(idx, 0, orm::kNumDectalkVoices - 1);
  MaybePushGestureUndo();
  GetParam(kDectalkVoice)->Set((double) v);
  InformHostOfParamChange(kDectalkVoice, GetParam(kDectalkVoice)->GetNormalized());
  mRenderDirty = true;
  {
    std::lock_guard<std::mutex> lock(mTextMutex);
    if (GetParam(kMapMode)->Int() == 1 && mUISelected >= 0) {
      auto it = mBank.find(mUISelected);
      if (it != mBank.end()) {
        it->second.dectalk.voice = v;
        it->second.dirty = true;
      }
    }
  }
  if (mDectalkVoiceSegment)
    mDectalkVoiceSegment->SetActive(v);
#if IPLUG_EDITOR
  if (GetUI())
    GetUI()->SetAllControlsDirty();
#endif
}

void ORMNarrator::SetMapModeFromUI(int idx) {
  const int v = std::clamp(idx, 0, 1);
  MaybePushGestureUndo();
  GetParam(kMapMode)->Set((double) v);
  InformHostOfParamChange(kMapMode, GetParam(kMapMode)->GetNormalized());
  if (mMapSegment)
    mMapSegment->SetActive(v);
  if (v == 1) {
    if (mUISelected < 0) {
      mPendingSelect = std::clamp(GetParam(kBaseKey)->Int(), 0, 127);
      mSelectChanged = true;
    }
  }
#if IPLUG_EDITOR
  if (GetUI())
    GetUI()->SetAllControlsDirty();
#endif
}

#if IPLUG_EDITOR
void ORMNarrator::ApplyLanguage() {
  for (auto &binding : mTextBindings)
    if (binding.second)
      binding.second(orm::Tr(binding.first, orm::UILang()));
  ApplyTooltips();
  if (mPhoneticSegment) {
    mPhoneticSegment->SetLabels({orm::Tr(orm::kTxtText, orm::UILang()),
                                 orm::Tr(orm::kTxtPhonetic, orm::UILang())});
  }
  if (mSp0256VoiceSegment) {
    mSp0256VoiceSegment->SetLabels({orm::Tr(orm::kTxtText, orm::UILang()),
                                    orm::Tr(orm::kTxtPhonetic, orm::UILang())});
  }
  if (mMapSegment) {
    mMapSegment->SetLabels({orm::Tr(orm::kTxtMapPitch, orm::UILang()),
                            orm::Tr(orm::kTxtMapWords, orm::UILang())});
  }
  RebindVoiceSliders((int) GetParam(kEngine)->Int());
  if (mCandidatePanel)
    mCandidatePanel->SetLanguage(orm::UILang());
  if (GetUI()) {
    GetUI()->SetAllControlsDirty();
    GetUI()->UpdateTooltips();
  }
}

void ORMNarrator::ApplyTooltips() {
  for (auto &binding : mTooltipBindings)
    if (binding.first)
      binding.first->SetTooltip(orm::Tr(binding.second, orm::UILang()));
}

void ORMNarrator::ApplyTheme() {
  ThemeMode() = mThemeMode;
  RefreshThemeColors();
}

void ORMNarrator::RefreshThemeColors() {
  if (GetUI()) {
    if (IControl *pBG = GetUI()->GetBackgroundControl()) {
      if (IPanelControl *pPanel = dynamic_cast<IPanelControl *>(pBG))
        pPanel->SetPattern(COL_100());
    }
    GetUI()->SetAllControlsDirty();
  }
}

void ORMNarrator::ToggleSettingsPanel() {
  if (mSettingsPanel)
    mSettingsPanel->SetVisible(mSettingsPanel->IsHidden());
}

// 参数快照撤销/重做
ParamSnapshot ORMNarrator::Snapshot() const {
  ParamSnapshot s;
  for (int i = 0; i < kNumParams; ++i)
    s[i] = GetParam(i)->Value();
  return s;
}

void ORMNarrator::SetParamFromEditor(int idx, double value) {
  GetParam(idx)->Set(value);
  InformHostOfParamChange(idx, GetParam(idx)->GetNormalized());
}

void ORMNarrator::ApplySnapshot(const ParamSnapshot &s) {
  for (int i = 0; i < kNumParams; ++i)
    SetParamFromEditor(i, s[i]);
  RebindVoiceSliders((int) GetParam(kEngine)->Int());
  RefreshAfterEdit();
}

void ORMNarrator::RefreshAfterEdit() {
  if (GetUI()) {
    SendCurrentParamValuesFromDelegate();
    GetUI()->SetAllControlsDirty();
  }
  MarkStateStable();
}

void ORMNarrator::PushUndoSnapshot(const ParamSnapshot &s) {
  if (!mUndoStack.empty() && mUndoStack.back() == s)
    return;
  mUndoStack.push_back(s);
  if (mUndoStack.size() > 100)
    mUndoStack.pop_front();
  mRedoStack.clear();
}

void ORMNarrator::MaybePushGestureUndo() {
  using namespace std::chrono;
  const double now = duration<double>(steady_clock::now().time_since_epoch()).count();
  if (now - mLastUIChangeTime > kGestureGapSec)
    PushUndoSnapshot(mStableSnapshot);
  mLastUIChangeTime = now;
  mGesturePending = true;
}

void ORMNarrator::MarkStateStable() {
  mStableSnapshot = Snapshot();
  mGesturePending = false;
}

void ORMNarrator::Undo() {
  if (mUndoStack.empty())
    return;
  mRedoStack.push_back(Snapshot());
  const ParamSnapshot s = mUndoStack.back();
  mUndoStack.pop_back();
  ApplySnapshot(s);
}

void ORMNarrator::Redo() {
  if (mRedoStack.empty())
    return;
  mUndoStack.push_back(Snapshot());
  const ParamSnapshot s = mRedoStack.back();
  mRedoStack.pop_back();
  ApplySnapshot(s);
}

void ORMNarrator::OnParamChangeUI(int paramIdx, EParamSource source) {
  if (source == EParamSource::kUI)
    MaybePushGestureUndo();
}

void ORMNarrator::OnIdle() {
  if (!GetUI())
    return;

  ApplySelectionFromIdle();

  // 手势结束后固化稳定快照
  using namespace std::chrono;
  const double now = duration<double>(steady_clock::now().time_since_epoch()).count();
  if (mGesturePending && now - mLastUIChangeTime > kGestureGapSec) {
    mStableSnapshot = Snapshot();
    mGesturePending = false;
  }

  // 音频线程队列 -> 控件
  TimelineMsg t;
  if (mTimelineQueue.ElementsAvailable()) {
    mTimelineQueue.Pop(t);
    mTimeline->OnMsgFromDelegate(UtteranceTimelineControl::kMsgTagPhrase,
                                 (int) sizeof(t.env), t.env.data());
  }
  ProgressMsg p;
  while (mProgressQueue.ElementsAvailable())
    mProgressQueue.Pop(p), mLastUIProgress = p.progress;
  if (mTimeline)
    mTimeline->SetProgress(mLastUIProgress, mPhraseDuration.load(std::memory_order_relaxed));

  NoteMsg n;
  while (mNoteQueue.ElementsAvailable()) {
    mNoteQueue.Pop(n);
    if (mKeyboard)
      mKeyboard->SetNoteActive(n.note, n.on);
  }
  if (mKeyboard) {
    mKeyboard->SetBaseKey(std::clamp(GetParam(kBaseKey)->Int(), 0, 127));
    mKeyboard->SetSelectedNote(mUISelected);
  }

  // 宿主侧改引擎时同步重绑滑块
  const int eng = (int) GetParam(kEngine)->Int();
  const int expectIdx = (eng == kEngineTMS) ? kTmsPitch : (eng == kEngineTSI) ? kTsiSpeed
                                                        : (eng == kEngineSP)  ? kSp0256Speed
                                                        : (eng == kEngineDEC) ? kDectalkPitch : kSamPitch;
  const bool bound = mParamSliders[0] ? (mParamSliders[0]->GetParamIdx() == expectIdx) : false;
  if (!bound)
    RebindVoiceSliders(eng);
  // 分段控件高亮跟随参数
  if (mEngineSegment)
    mEngineSegment->SetActive((int) GetParam(kEngine)->Int());
  if (mVoiceSegment)
    mVoiceSegment->SetActive((int) GetParam(kTmsBank)->Int());
  if (mTsiVoiceSegment)
    mTsiVoiceSegment->SetActive((int) GetParam(kTsiBank)->Int());
  if (mSp0256VoiceSegment)
    mSp0256VoiceSegment->SetActive((int) GetParam(kSp0256Voice)->Int());
  if (mDectalkVoiceSegment)
    mDectalkVoiceSegment->SetActive((int) GetParam(kDectalkVoice)->Int());
  if (mMapSegment)
    mMapSegment->SetActive((int) GetParam(kMapMode)->Int());
}
#endif

void ORMNarrator::OnParentWindowResize(int width, int height) {
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

bool ORMNarrator::ConstrainEditorResize(int &w, int &h) const {
  constexpr double kMinScale = DEFAULT_MIN_DRAW_SCALE;
  w = std::max(w, static_cast<int>(PLUG_WIDTH * kMinScale));
  const int wantH = static_cast<int>(std::lround(w * static_cast<double>(PLUG_HEIGHT) / PLUG_WIDTH));
  const bool ok = (h == wantH);
  h = wantH;
  return ok;
}

void ORMNarrator::SaveSettingsToDisk() {
  SettingsData s;
  s.lang = orm::UILang();
  s.hue = ThemeHue();
  s.satMax = ThemeSatMax();
  s.themeMode = mThemeMode;
  SaveSettings(s);
}

#if IPLUG_DSP
void ORMNarrator::ProcessMidiMsg(const IMidiMsg &msg) {
  const IMidiMsg::EStatusMsg status = msg.StatusMsg();
  bool isNoteOn = false;
  if (status == IMidiMsg::kNoteOn && msg.Velocity() > 0)
    isNoteOn = true;
  else if (status == IMidiMsg::kNoteOff ||
           (status == IMidiMsg::kNoteOn && msg.Velocity() == 0))
    isNoteOn = false;
  else
    return;

  if (mMidiCount >= kMaxMidiPerBlock)
    return;
  mMidiQueue[(size_t) mMidiCount++] = {msg.mOffset, isNoteOn, msg.NoteNumber(),
                                       isNoteOn ? msg.Velocity() : 0};
}

void ORMNarrator::ProcessBlock(sample **inputs, sample **outputs, int nFrames) {
  nFrames = std::min(nFrames, kMaxBlock);
  if (nFrames <= 0) {
    mMidiCount = 0;
    return;
  }

  const int nOuts = NOutChansConnected();
  if (nOuts <= 0) {
    mMidiCount = 0;
    return;
  }
  sample *main = outputs[0];
  memset(main, 0, nFrames * sizeof(sample));

  // MIDI 事件按采样偏移分段处理
  int pos = 0;
  if (mMidiCount > 0) {
    std::sort(mMidiQueue.begin(), mMidiQueue.begin() + mMidiCount,
              [](const MidiEvent &a, const MidiEvent &b) { return a.offset < b.offset; });
    for (int i = 0; i < mMidiCount; ++i) {
      const MidiEvent &ev = mMidiQueue[(size_t) i];
      const int off = std::clamp(ev.offset, pos, nFrames);
      RenderSegment(main, pos, off);
      if (ev.isNoteOn)
        TriggerVoice(ev.note);
      else
        ReleaseVoice(ev.note);
      pos = off;
    }
    mMidiCount = 0;
  }
  RenderSegment(main, pos, nFrames);

  // 输出增益 + 声道复制
  const double gain = std::pow(10., GetParam(kGain)->Value() / 20.);
  for (int i = 0; i < nFrames; ++i)
    main[i] = (sample)(main[i] * gain);
  for (int c = 1; c < nOuts; ++c)
    memcpy(outputs[c], main, nFrames * sizeof(sample));

  // 进度上报
  float progress = 0.f;
  for (const Voice &v : mVoices)
    if (v.renderer.IsPlaying())
      progress = (float) v.renderer.Progress();
  if (progress > 0.f || mAudioProgress > 0.f) {
    if (std::fabs(progress - mAudioProgress) > 0.01f || (mAudioProgress > 0.f && progress <= 0.f)) {
      mProgressQueue.Push({progress});
      mAudioProgress = progress;
    }
  }
}

void ORMNarrator::OnReset() {
  mRenderDirty = true;
}

void ORMNarrator::OnParamChange(int paramIdx, EParamSource source, int sampleOffset) {
  switch (paramIdx) {
    case kSamPitch:
    case kSamSpeed:
    case kSamMouth:
    case kSamThroat:
    case kTmsSpeed:
    case kTmsPitch:
    case kTmsBank:
    case kTsiSpeed:
    case kTsiBank:
    case kSp0256Speed:
    case kSp0256Voice:
    case kDectalkVoice:
    case kDectalkRate:
    case kDectalkPitch:
      mRenderDirty = true;
      // BANK 模式: 同步写入选中键的绑定
      if (GetParam(kMapMode)->Int() == 1 && mUISelected >= 0) {
        std::lock_guard<std::mutex> lock(mTextMutex);
        auto it = mBank.find(mUISelected);
        if (it != mBank.end()) {
          switch (paramIdx) {
            case kSamPitch: it->second.sam.pitch = (int) GetParam(kSamPitch)->Value(); break;
            case kSamSpeed: it->second.sam.speed = (int) GetParam(kSamSpeed)->Value(); break;
            case kSamMouth: it->second.sam.mouth = (int) GetParam(kSamMouth)->Value(); break;
            case kSamThroat: it->second.sam.throat = (int) GetParam(kSamThroat)->Value(); break;
            case kTmsSpeed: it->second.tms.speed = (int) GetParam(kTmsSpeed)->Value(); break;
            case kTmsPitch: it->second.tms.pitch = (int) GetParam(kTmsPitch)->Value(); break;
            case kTmsBank: it->second.tms.bank = (int) GetParam(kTmsBank)->Int(); break;
            case kTsiSpeed: it->second.tsi.speed = (int) GetParam(kTsiSpeed)->Value(); break;
            case kTsiBank: it->second.tsi.bank = (int) GetParam(kTsiBank)->Int(); break;
            case kSp0256Speed: it->second.sp0256.speed = (int) GetParam(kSp0256Speed)->Value(); break;
            case kSp0256Voice: it->second.sp0256.variant = (int) GetParam(kSp0256Voice)->Int(); break;
            case kDectalkVoice: it->second.dectalk.voice = (int) GetParam(kDectalkVoice)->Int(); break;
            case kDectalkRate: it->second.dectalk.rate = (int) GetParam(kDectalkRate)->Value(); break;
            case kDectalkPitch: it->second.dectalk.pitch = (int) GetParam(kDectalkPitch)->Value(); break;
            default: break;
          }
          it->second.dirty = true;
        }
      }
      break;
    default:
      break;
  }
}

bool ORMNarrator::SerializeState(IByteChunk &chunk) const {
  {
    std::lock_guard<std::mutex> lock(mTextMutex);
    chunk.PutStr(mPhraseText.c_str());
    const int ph = mPhonetic ? 1 : 0;
    chunk.Put(&ph);
    // BANK 绑定表: 版本 + 数量 + 每项
    const int ver = 5;
    chunk.Put(&ver);
    const int count = (int) mBank.size();
    chunk.Put(&count);
    for (const auto &kv : mBank) {
      const int note = kv.first;
      const int eng = kv.second.engine;
      const int sPit = kv.second.sam.pitch;
      const int sSpd = kv.second.sam.speed;
      const int sMou = kv.second.sam.mouth;
      const int sThr = kv.second.sam.throat;
      const int tSpd = kv.second.tms.speed;
      const int tPit = kv.second.tms.pitch;
      const int tBnk = kv.second.tms.bank;
      const int rSpd = kv.second.tsi.speed;
      const int rBnk = kv.second.tsi.bank;
      const int nSpd = kv.second.sp0256.speed;
      const int nVoi = kv.second.sp0256.variant;
      const int dVoi = kv.second.dectalk.voice;
      const int dRte = kv.second.dectalk.rate;
      const int dPit = kv.second.dectalk.pitch;
      const int phn = kv.second.phonetic ? 1 : 0;
      chunk.Put(&note);
      chunk.Put(&eng);
      chunk.Put(&sPit);
      chunk.Put(&sSpd);
      chunk.Put(&sMou);
      chunk.Put(&sThr);
      chunk.Put(&tSpd);
      chunk.Put(&tPit);
      chunk.Put(&tBnk);
      chunk.Put(&rSpd);
      chunk.Put(&rBnk);
      chunk.Put(&nSpd);
      chunk.Put(&nVoi);
      chunk.Put(&dVoi);
      chunk.Put(&dRte);
      chunk.Put(&dPit);
      chunk.Put(&phn);
      chunk.PutStr(kv.second.text.c_str());
    }
  }
  return SerializeParams(chunk);
}

int ORMNarrator::UnserializeState(const IByteChunk &chunk, int startPos) {
  WDL_String str;
  startPos = chunk.GetStr(str, startPos);
  int ph = 0;
  startPos = chunk.Get(&ph, startPos);
  {
    std::lock_guard<std::mutex> lock(mTextMutex);
    mPhraseText = str.Get();
    mPhonetic = ph != 0;

    mBank.clear();
    int ver = 0, count = 0;
    startPos = chunk.Get(&ver, startPos);
    startPos = chunk.Get(&count, startPos);
    for (int i = 0; i < count && startPos >= 0; ++i) {
      if (ver >= 2) {
        int note = 0, eng = 0, sPit = 0, sSpd = 0, sMou = 0, sThr = 0;
        int tSpd = 0, tPit = 0, tBnk = 0, phn = 0;
        int rSpd = 72, rBnk = 0;
        int nSpd = 72, nVoi = 0;
        int dVoi = 0, dRte = 180, dPit = 0;
        startPos = chunk.Get(&note, startPos);
        startPos = chunk.Get(&eng, startPos);
        startPos = chunk.Get(&sPit, startPos);
        startPos = chunk.Get(&sSpd, startPos);
        startPos = chunk.Get(&sMou, startPos);
        startPos = chunk.Get(&sThr, startPos);
        startPos = chunk.Get(&tSpd, startPos);
        startPos = chunk.Get(&tPit, startPos);
        startPos = chunk.Get(&tBnk, startPos);
        if (ver >= 3) {
          startPos = chunk.Get(&rSpd, startPos);
          startPos = chunk.Get(&rBnk, startPos);
        }
        if (ver >= 4) {
          startPos = chunk.Get(&nSpd, startPos);
          startPos = chunk.Get(&nVoi, startPos);
        }
        if (ver >= 5) {
          startPos = chunk.Get(&dVoi, startPos);
          startPos = chunk.Get(&dRte, startPos);
          startPos = chunk.Get(&dPit, startPos);
        }
        startPos = chunk.Get(&phn, startPos);
        startPos = chunk.GetStr(str, startPos);
        if (startPos < 0)
          break;
        BankEntry e;
        e.text = str.Get();
        e.phonetic = phn != 0;
        e.engine = std::clamp(eng, 0, 4);
        e.sam.pitch = std::clamp(sPit, 0, 255);
        e.sam.speed = std::clamp(sSpd, 1, 255);
        e.sam.mouth = std::clamp(sMou, 0, 255);
        e.sam.throat = std::clamp(sThr, 0, 255);
        e.tms.speed = std::clamp(tSpd, 1, 255);
        e.tms.pitch = std::clamp(tPit, 0, 255);
        e.tms.bank = std::clamp(tBnk, 0, kNumBanks - 1);
        e.tsi.speed = std::clamp(rSpd, 1, 255);
        e.tsi.bank = std::clamp(rBnk, 0, kNumS14001Sets - 1);
        e.sp0256.speed = std::clamp(nSpd, 1, 255);
        e.sp0256.variant = std::clamp(nVoi, 0, kNumSp0256 - 1);
        e.dectalk.voice = std::clamp(dVoi, 0, orm::kNumDectalkVoices - 1);
        e.dectalk.rate = std::clamp(dRte, 75, 600);
        e.dectalk.pitch = std::clamp(dPit, 0, 400);
        e.dirty = true;
        mBank[note] = std::move(e);
      } else {
        // v1 旧格式
        int note = 0, eng = 0, pit = 0, spd = 0, mou = 0, thr = 0, phn = 0;
        startPos = chunk.Get(&note, startPos);
        startPos = chunk.Get(&eng, startPos);
        startPos = chunk.Get(&pit, startPos);
        startPos = chunk.Get(&spd, startPos);
        startPos = chunk.Get(&mou, startPos);
        startPos = chunk.Get(&thr, startPos);
        startPos = chunk.Get(&phn, startPos);
        startPos = chunk.GetStr(str, startPos);
        if (startPos < 0)
          break;
        BankEntry e;
        e.text = str.Get();
        e.phonetic = phn != 0;
        e.engine = std::clamp(eng, 0, 1);
        e.sam.pitch = std::clamp(pit, 0, 255);
        e.sam.speed = std::clamp(spd, 1, 255);
        e.sam.mouth = std::clamp(mou, 0, 255);
        e.sam.throat = std::clamp(thr, 0, 255);
        e.dirty = true;
        mBank[note] = std::move(e);
      }
    }
  }
  mRenderDirty = true;
  startPos = UnserializeParams(chunk, startPos);
  mStableSnapshot = Snapshot();
  return startPos;
}

void ORMNarrator::EnsureRendered() {
  if (!mRenderDirty)
    return;

  std::string text;
  bool phon = false;
  {
    std::lock_guard<std::mutex> lock(mTextMutex);
    text = mPhraseText;
    phon = mPhonetic;
  }

  orm::SamSettings s;
  s.pitch = (int) GetParam(kSamPitch)->Value();
  s.speed = (int) GetParam(kSamSpeed)->Value();
  s.mouth = (int) GetParam(kSamMouth)->Value();
  s.throat = (int) GetParam(kSamThroat)->Value();

  const int engine = (int) GetParam(kEngine)->Int();
  const int bank = (int) GetParam(kTmsBank)->Int();
  const int tsiBank = (int) GetParam(kTsiBank)->Int();
  const int spVariant = (int) GetParam(kSp0256Voice)->Int();
  const float tmsScale = (float)(GetParam(kTmsSpeed)->Value() / 72.0);
  const float tsiScale = (float)(GetParam(kTsiSpeed)->Value() / 72.0);
  const float spScale = (float)(GetParam(kSp0256Speed)->Value() / 72.0);
  bool ok = false;
  double rate = 0.0;
  if (engine == kEngineSAM) {
    ok = orm::SamEngine::Render(text, phon, s, mPhraseBuffer);
    rate = orm::SamEngine::kSampleRate;
  } else if (engine == kEngineTSI) {
    ok = orm::TsiS14001Engine::Render(text, tsiBank, tsiScale, mPhraseBuffer);
    rate = orm::TsiS14001Engine::RateForSet(tsiBank, tsiScale);
  } else if (engine == kEngineSP) {
    ok = orm::Sp0256Engine::Render(text, spVariant, spScale, mPhraseBuffer);
    rate = orm::Sp0256Engine::RateFor(spScale);
  } else if (engine == kEngineDEC) {
    orm::DectalkSettings d;
    d.voice = (int) GetParam(kDectalkVoice)->Int();
    d.rate = (int) GetParam(kDectalkRate)->Value();
    d.pitch = (int) GetParam(kDectalkPitch)->Value();
    ok = orm::DectalkEngine::Render(text, phon, d, mPhraseBuffer);
    rate = orm::DectalkEngine::kSampleRate;
  } else if (bank == kBankSspell) {
    ok = orm::Tms5110Engine::Render(text, tmsScale, mPhraseBuffer);
    rate = orm::Tms5110Engine::kSampleRate;
  } else {
    ok = orm::Tms5220Engine::Render(text, bank, tmsScale, mPhraseBuffer);
    rate = orm::Tms5220Engine::kSampleRate;
  }

  if (ok) {
    mPhraseDuration = (double) mPhraseBuffer.size() / rate;
    PushPhraseToUI();
  } else {
    mPhraseBuffer.clear();
    mPhraseDuration = 0.0;
  }
  mRenderDirty = false;
}

void ORMNarrator::RenderBankEntry(BankEntry &e) {
  e.rendered.clear();
  if (e.engine == kEngineSAM) {
    orm::SamEngine::Render(e.text, e.phonetic, e.sam, e.rendered);
  } else if (e.engine == kEngineTSI) {
    orm::TsiS14001Engine::Render(e.text, e.tsi.bank, (float)(e.tsi.speed / 72.0), e.rendered);
  } else if (e.engine == kEngineSP) {
    orm::Sp0256Engine::Render(e.text, e.sp0256.variant, (float)(e.sp0256.speed / 72.0), e.rendered);
  } else if (e.engine == kEngineDEC) {
    orm::DectalkEngine::Render(e.text, e.phonetic, e.dectalk, e.rendered);
  } else if (e.tms.bank == kBankSspell) {
    orm::Tms5110Engine::Render(e.text, (float)(e.tms.speed / 72.0), e.rendered);
  } else {
    orm::Tms5220Engine::Render(e.text, e.tms.bank, (float)(e.tms.speed / 72.0), e.rendered);
  }
  e.dirty = false;
}

void ORMNarrator::PushBankPhraseToUI(const BankEntry &e) {
  TimelineMsg t{};
  const int n = (int) e.rendered.size();
  if (n <= 0)
    return;
  for (int i = 0; i < kPhraseEnvPoints; ++i) {
    const int start = n * i / kPhraseEnvPoints;
    const int end = std::max(start + 1, n * (i + 1) / kPhraseEnvPoints);
    float peak = 0.f;
    for (int j = start; j < end && j < n; ++j) {
      const float a = std::abs(e.rendered[(size_t) j]);
      if (a > peak)
        peak = a;
    }
    t.env[(size_t) i] = peak;
  }
  mTimelineQueue.Push(t);
}

void ORMNarrator::ApplySelectionFromIdle() {
  if (!mSelectChanged.exchange(false))
    return;
  mUISelected = mPendingSelect.load(std::memory_order_relaxed);

  std::string text;
  bool phonetic = false;
  int engine = 0;
  orm::SamSettings sam;
  orm::TmsSettings tms;
  orm::TsiSettings tsi;
  orm::Sp0256Settings sp0256;
  orm::DectalkSettings dectalk;
  {
    std::lock_guard<std::mutex> lock(mTextMutex);
    auto it = mBank.find(mUISelected);
    if (it != mBank.end()) {
      text = it->second.text;
      phonetic = it->second.phonetic;
      engine = it->second.engine;
      sam = it->second.sam;
      tms = it->second.tms;
      tsi = it->second.tsi;
      sp0256 = it->second.sp0256;
      dectalk = it->second.dectalk;
    } else {
      mBank[mUISelected] = BankEntry{};
      text = "";
      engine = (int) GetParam(kEngine)->Int();
      sam.pitch = (int) GetParam(kSamPitch)->Value();
      sam.speed = (int) GetParam(kSamSpeed)->Value();
      sam.mouth = (int) GetParam(kSamMouth)->Value();
      sam.throat = (int) GetParam(kSamThroat)->Value();
      tms.speed = (int) GetParam(kTmsSpeed)->Value();
      tms.pitch = (int) GetParam(kTmsPitch)->Value();
      tms.bank = (int) GetParam(kTmsBank)->Int();
      tsi.speed = (int) GetParam(kTsiSpeed)->Value();
      tsi.bank = (int) GetParam(kTsiBank)->Int();
      sp0256.speed = (int) GetParam(kSp0256Speed)->Value();
      sp0256.variant = (int) GetParam(kSp0256Voice)->Int();
      dectalk.voice = (int) GetParam(kDectalkVoice)->Int();
      dectalk.rate = (int) GetParam(kDectalkRate)->Value();
      dectalk.pitch = (int) GetParam(kDectalkPitch)->Value();
      mBank[mUISelected].sam = sam;
      mBank[mUISelected].tms = tms;
      mBank[mUISelected].tsi = tsi;
      mBank[mUISelected].sp0256 = sp0256;
      mBank[mUISelected].dectalk = dectalk;
      mBank[mUISelected].engine = engine;
    }
  }
  GetParam(kEngine)->Set((double) engine);
  GetParam(kSamPitch)->Set((double) sam.pitch);
  GetParam(kSamSpeed)->Set((double) sam.speed);
  GetParam(kSamMouth)->Set((double) sam.mouth);
  GetParam(kSamThroat)->Set((double) sam.throat);
  GetParam(kTmsSpeed)->Set((double) tms.speed);
  GetParam(kTmsPitch)->Set((double) tms.pitch);
  GetParam(kTmsBank)->Set((double) tms.bank);
  GetParam(kTsiSpeed)->Set((double) tsi.speed);
  GetParam(kTsiBank)->Set((double) tsi.bank);
  GetParam(kSp0256Speed)->Set((double) sp0256.speed);
  GetParam(kSp0256Voice)->Set((double) sp0256.variant);
  GetParam(kDectalkVoice)->Set((double) dectalk.voice);
  GetParam(kDectalkRate)->Set((double) dectalk.rate);
  GetParam(kDectalkPitch)->Set((double) dectalk.pitch);
  {
    std::lock_guard<std::mutex> lock(mTextMutex);
    mPhonetic = phonetic;
  }
  if (mEngineSegment)
    mEngineSegment->SetActive(engine);
  if (mVoiceSegment)
    mVoiceSegment->SetActive(tms.bank);
  if (mTsiVoiceSegment)
    mTsiVoiceSegment->SetActive(tsi.bank);
  if (mSp0256VoiceSegment)
    mSp0256VoiceSegment->SetActive(sp0256.variant);
  if (mDectalkVoiceSegment)
    mDectalkVoiceSegment->SetActive(dectalk.voice);
  if (mCandidatePanel) {
    mCandidatePanel->SetEngine(engine);
    mCandidatePanel->SetTmsBank(tms.bank);
    mCandidatePanel->SetPhoneticMode(phonetic);
  }
  RebindVoiceSliders(engine);
#if IPLUG_EDITOR
  if (GetUI()) {
    SendCurrentParamValuesFromDelegate();
    GetUI()->SetAllControlsDirty();
  }
#endif
}

void ORMNarrator::RebindVoiceSliders(int engine) {
  if (engine == kEngineTMS) {
    const int tmsIdx[2] = {kTmsPitch, kTmsSpeed};
    for (int i = 0; i < 2; ++i) {
      if (mParamSliders[i]) {
        mParamSliders[i]->SetParamIdx(tmsIdx[i]);
        mParamSliders[i]->SetValueFromDelegate(GetParam(tmsIdx[i])->GetNormalized());
        mParamSliders[i]->SetGhost(false);
      }
    }
    for (int i = 2; i < 4; ++i) {
      if (mParamSliders[i]) {
        mParamSliders[i]->SetParamIdx(kNoParameter);
        mParamSliders[i]->SetGhost(true);
      }
    }
    if (mParamSliders[0])
      mParamSliders[0]->SetHeaderLabel(orm::Tr(orm::kTxtPitch, orm::UILang()));
  } else if (engine == kEngineTSI) {
    if (mParamSliders[0]) {
      mParamSliders[0]->SetParamIdx(kTsiSpeed);
      mParamSliders[0]->SetValueFromDelegate(GetParam(kTsiSpeed)->GetNormalized());
      mParamSliders[0]->SetGhost(false);
      mParamSliders[0]->SetHeaderLabel(orm::Tr(orm::kTxtTsiRate, orm::UILang()));
    }
    for (int i = 1; i < 4; ++i) {
      if (mParamSliders[i]) {
        mParamSliders[i]->SetParamIdx(kNoParameter);
        mParamSliders[i]->SetGhost(true);
      }
    }
  } else if (engine == kEngineSP) {
    if (mParamSliders[0]) {
      mParamSliders[0]->SetParamIdx(kSp0256Speed);
      mParamSliders[0]->SetValueFromDelegate(GetParam(kSp0256Speed)->GetNormalized());
      mParamSliders[0]->SetGhost(false);
      mParamSliders[0]->SetHeaderLabel(orm::Tr(orm::kTxtSp0256Rate, orm::UILang()));
    }
    for (int i = 1; i < 4; ++i) {
      if (mParamSliders[i]) {
        mParamSliders[i]->SetParamIdx(kNoParameter);
        mParamSliders[i]->SetGhost(true);
      }
    }
  } else if (engine == kEngineDEC) {
    const int dtIdx[2] = {kDectalkPitch, kDectalkRate};
    for (int i = 0; i < 2; ++i) {
      if (mParamSliders[i]) {
        mParamSliders[i]->SetParamIdx(dtIdx[i]);
        mParamSliders[i]->SetValueFromDelegate(GetParam(dtIdx[i])->GetNormalized());
        mParamSliders[i]->SetGhost(false);
      }
    }
    for (int i = 2; i < 4; ++i) {
      if (mParamSliders[i]) {
        mParamSliders[i]->SetParamIdx(kNoParameter);
        mParamSliders[i]->SetGhost(true);
      }
    }
    if (mParamSliders[0]) {
      mParamSliders[0]->SetHeaderLabel(orm::Tr(orm::kTxtPitch, orm::UILang()));
      mParamSliders[0]->SetTooltip(orm::Tr(orm::kTxtTipDectalkPitch, orm::UILang()));
    }
    if (mParamSliders[1]) {
      mParamSliders[1]->SetHeaderLabel(orm::Tr(orm::kTxtTsiRate, orm::UILang()));
      mParamSliders[1]->SetTooltip(orm::Tr(orm::kTxtTipDectalkRate, orm::UILang()));
    }
  } else {
    const int samIdx[4] = {kSamPitch, kSamSpeed, kSamMouth, kSamThroat};
    for (int i = 0; i < 4; ++i) {
      if (mParamSliders[i]) {
        mParamSliders[i]->SetParamIdx(samIdx[i]);
        mParamSliders[i]->SetValueFromDelegate(GetParam(samIdx[i])->GetNormalized());
        mParamSliders[i]->SetGhost(false);
      }
    }
    if (mParamSliders[0])
      mParamSliders[0]->SetHeaderLabel(orm::Tr(orm::kTxtPitch, orm::UILang()));
    if (mParamSliders[0])
      mParamSliders[0]->SetTooltip(orm::Tr(orm::kTxtTipPitch, orm::UILang()));
    if (mParamSliders[1])
      mParamSliders[1]->SetTooltip(orm::Tr(orm::kTxtTipSpeed, orm::UILang()));
  }
  if (mPhoneticSegment)
    mPhoneticSegment->Hide(engine != kEngineSAM);
  if (mVoiceSegment)
    mVoiceSegment->Hide(engine != kEngineTMS);
  if (mTsiVoiceSegment)
    mTsiVoiceSegment->Hide(engine != kEngineTSI);
  if (mSp0256VoiceSegment)
    mSp0256VoiceSegment->Hide(engine != kEngineSP);
  if (mDectalkVoiceSegment)
    mDectalkVoiceSegment->Hide(engine != kEngineDEC);
}

void ORMNarrator::PushPhraseToUI() {
  TimelineMsg t{};
  const int n = (int) mPhraseBuffer.size();
  if (n <= 0)
    return;
  for (int i = 0; i < kPhraseEnvPoints; ++i) {
    const int start = n * i / kPhraseEnvPoints;
    const int end = std::max(start + 1, n * (i + 1) / kPhraseEnvPoints);
    float peak = 0.f;
    for (int j = start; j < end && j < n; ++j) {
      const float a = std::abs(mPhraseBuffer[(size_t) j]);
      if (a > peak)
        peak = a;
    }
    t.env[(size_t) i] = peak;
  }
  mTimelineQueue.Push(t);
}

void ORMNarrator::TriggerVoice(int note) {
  const bool held = mUIHoldState[(size_t) note].exchange(1, std::memory_order_relaxed) != 0;
  const bool bank = GetParam(kMapMode)->Int() == 1;

  if (bank)
  {
    const BankEntry *entry = nullptr;
    {
      std::lock_guard<std::mutex> lock(mTextMutex);
      auto it = mBank.find(note);
      if (it != mBank.end())
        entry = &it->second;
    }
    if (mPendingSelect.exchange(note) != note)
      mSelectChanged = true;

    if (!entry)
      return;

    BankEntry *e = const_cast<BankEntry *>(entry);
    if (e->dirty)
      RenderBankEntry(*e);
    if (e->rendered.empty())
      return;

    const bool mono = GetParam(kMono)->Value() > 0.5;
    if (mono)
    {
      for (Voice &v : mVoices)
      {
        v.held = false;
        if (v.renderer.IsPlaying())
          v.renderer.Release();
      }
    }

    Voice &v = mVoices[(size_t) mNextVoice];
    mNextVoice = (mNextVoice + 1) % kMaxVoices;
    v.note = note;
    v.held = held;
    v.renderer.SetEnvelope(GetParam(kAttack)->Value(), GetParam(kRelease)->Value());
    const double bankRate = e->engine == kEngineSAM ? orm::SamEngine::kSampleRate
                           : e->engine == kEngineTSI ? orm::TsiS14001Engine::RateForSet(e->tsi.bank, (float)(e->tsi.speed / 72.0))
                           : e->engine == kEngineSP ? orm::Sp0256Engine::RateFor((float)(e->sp0256.speed / 72.0))
                           : e->engine == kEngineDEC ? orm::DectalkEngine::kSampleRate
                           : (e->tms.bank == kBankSspell) ? orm::Tms5110Engine::kSampleRate
                                                          : orm::Tms5220Engine::kSampleRate;
    v.renderer.SetPhrase(e->rendered.data(), (int) e->rendered.size(), bankRate, GetSampleRate());
    v.ratio = e->engine == kEngineTMS ? std::pow(2., (e->tms.pitch - 64.) / 24.)
                             : 1.0;
    v.renderer.Trigger(v.ratio);
    PushBankPhraseToUI(*e);
    return;
  }

  // PHRASE 模式
  EnsureRendered();
  if (mPhraseBuffer.empty())
    return;

  const bool mono = GetParam(kMono)->Value() > 0.5;

  if (mono)
  {
    for (Voice &v : mVoices)
    {
      v.held = false;
      if (v.renderer.IsPlaying())
        v.renderer.Release();
    }
  }

  Voice &v = mVoices[(size_t) mNextVoice];
  mNextVoice = (mNextVoice + 1) % kMaxVoices;
  v.note = note;
  v.held = held;
  v.renderer.SetEnvelope(GetParam(kAttack)->Value(), GetParam(kRelease)->Value());
  const int engine = (int) GetParam(kEngine)->Int();
  const int bankIdx = (int) GetParam(kTmsBank)->Int();
  const int tsiBank = (int) GetParam(kTsiBank)->Int();
  const double phraseRate =
      (engine == kEngineSAM) ? orm::SamEngine::kSampleRate
      : (engine == kEngineTSI) ? orm::TsiS14001Engine::RateForSet(tsiBank, (float)(GetParam(kTsiSpeed)->Value() / 72.0))
      : (engine == kEngineSP) ? orm::Sp0256Engine::RateFor((float)(GetParam(kSp0256Speed)->Value() / 72.0))
      : (engine == kEngineDEC) ? orm::DectalkEngine::kSampleRate
      : (bankIdx == kBankSspell) ? orm::Tms5110Engine::kSampleRate
                                 : orm::Tms5220Engine::kSampleRate;
  v.renderer.SetPhrase(mPhraseBuffer.data(), (int) mPhraseBuffer.size(), phraseRate,
                       GetSampleRate());
  double ratio = PitchRatioForNote(note);
  if (engine == kEngineTMS)
    ratio *= std::pow(2., (GetParam(kTmsPitch)->Value() - 64.) / 24.);
  v.ratio = ratio;
  v.renderer.Trigger(ratio);
}

void ORMNarrator::ReleaseVoice(int note) {
  for (Voice &v : mVoices)
    if (v.note == note)
    {
      v.held = false;
      if (v.renderer.IsPlaying())
        v.renderer.Release();
    }
}

double ORMNarrator::PitchRatioForNote(int note) const {
  const int base = (int) GetParam(kBaseKey)->Value();
  return std::pow(2., (double) (note - base) / 12.);
}

void ORMNarrator::RenderSegment(sample *out, int from, int to) {
  const bool loop = GetParam(kLoop)->Value() > 0.5;
  for (Voice &v : mVoices) {
    if (loop && v.held && !v.renderer.IsPlaying())
      v.renderer.Trigger(v.ratio);
    v.renderer.ProcessAdd(out + from, to - from);
  }
}
#endif
