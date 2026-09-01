#include "Narrator.h"
#include "IPlug_include_in_plug_src.h"
#include "IControls.h"
#include "ICornerResizerControl.h"
#include "Theme.h"
#include "controls/UiUtils.h"
#include "controls/ThemeCornerResizer.h"
#include "controls/FlatButton.h"
#include "controls/ORMSlider.h"
#include "controls/SectionTitleControl.h"
#include "controls/SettingsPanelControl.h"
#include "controls/PianoKeyboardControl.h"
#include "controls/PhraseEditorControl.h"
#include "controls/UtteranceTimelineControl.h"
#include "SettingsFileIO.h"

#include <cstring>
#include <cstdio>
#include <functional>
#include <algorithm>
#include <string>
#include <cmath>

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

ORMNarrator::ORMNarrator(const InstanceInfo &info) : Plugin(info, MakeConfig(kNumParams, 1)) {
  // 读取全局 UI 偏好 (语言/主题), 使界面首次渲染即用用户设置
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

  GetParam(kEngine)->InitEnum("Engine", 0, {"SAM", "TMS", "TSI", "SP0256", "DECTALK"});
  GetParam(kMapMode)->InitEnum("Map Mode", 0, {"PHRASE", "BANK"});
  GetParam(kBaseKey)->InitDouble("Base Key", 48., 0., 127., 1., "");
  // SAM 专属音色参数 (与 TMS 解耦)
  GetParam(kSamPitch)->InitDouble("SAM Pitch", 64., 0., 255., 1., "");
  GetParam(kSamSpeed)->InitDouble("SAM Speed", 72., 1., 255., 1., "");
  GetParam(kSamMouth)->InitDouble("Mouth", 128., 0., 255., 1., "");
  GetParam(kSamThroat)->InitDouble("Throat", 128., 0., 255., 1., "");
  // TMS 专属 (与 SAM 解耦)
  GetParam(kTmsSpeed)->InitDouble("TMS Speed", 72., 1., 255., 1., "");
  GetParam(kTmsPitch)->InitDouble("TMS Pitch", 64., 0., 255., 1., "");
  GetParam(kTmsBank)->InitEnum("Voice", 0,
                               {"MIL", "TI99", "ACORN", "S&S", "CLOCK"});
  // TSI 专属 (与 SAM/TMS 解耦)
  GetParam(kTsiSpeed)->InitDouble("TSI Rate", 72., 1., 255., 1., "");
  GetParam(kTsiBank)->InitEnum("TSI Voice", 0,
                               {"BZ", "F2", "C0", "C1", "C2", "C3", "C4", "C5", "C6"});
  // SP0256 专属 (与 SAM/TMS/TSI 解耦)
  GetParam(kSp0256Speed)->InitDouble("SP0256 Rate", 72., 1., 255., 1., "");
  GetParam(kSp0256Voice)->InitEnum("SP0256 Voice", 0, {"Text", "Phoneme"});
  // DECTALK 专属 (与 SAM/TMS/TSI/SP0256 解耦); 语速/音高走 DECtalk 原生
  // 机制 (rate 管道命令 / ap 音色参数, 保韵律), 键盘变调仍用 VoiceRenderer
  GetParam(kDectalkVoice)->InitEnum("DT Voice", 0,
                                    {"PAUL", "BETTY", "HARRY", "FRANK", "DENNIS", "KIT", "URS", "RITA", "WENDY"});
  GetParam(kDectalkRate)->InitDouble("DT Rate", 180., 75., 600., 1., "wpm");
  GetParam(kDectalkPitch)->InitDouble("DT Pitch", 0., 0., 400., 1., "Hz");
  // 通用
  GetParam(kAttack)->InitDouble("Attack", 5., 1., 500., 1., "ms");
  GetParam(kRelease)->InitDouble("Release", 120., 1., 2000., 1., "ms");
  GetParam(kMono)->InitBool("Mono", true);
  GetParam(kGain)->InitDouble("Output", 0., -24., 6., 0.1, "dB");

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

    constexpr float kLeftR = 640.f;  // 左列右边界
    constexpr float kRightL = 656.f; // 右列左边界
    constexpr float kRightR = 940.f;

    // ---- 语句区 (左列) ----
    SectionTitleControl *phraseTitle = new SectionTitleControl(
        IRECT(20, 20, kLeftR, 44), "PHRASE",
        IText(20, COL_900(), kFontBold, EAlign::Near, EVAlign::Middle), 0, 1);
    pGraphics->AttachControl(phraseTitle);
    phraseTitle->SetTargetRECT(IRECT(20, 20, 170, 44));
    bindText(orm::kTxtPhrase, [phraseTitle](const char *s) {
      phraseTitle->SetStr(s);
      phraseTitle->SetDirty(false);
    });
    bindTip(phraseTitle, orm::kTxtTipPhrase);

    // 映射模式: PHRASE 单句变调 / BANK 逐键绑定
    mMapSegment = new FlatSegmentControl(
        IRECT(180, 20, 400, 44),
        std::vector<std::string>{"PHRASE", "BANK"},
        [this](int idx) { SetMapModeFromUI(idx); },
        GetParam(kMapMode)->Int());
    pGraphics->AttachControl(mMapSegment);

    mPhraseEditor = new PhraseEditorControl(
        IRECT(20, 48, kLeftR, 116),
        PhraseEditorControl::Hooks{
            [this]() -> std::string {
              std::lock_guard<std::mutex> lock(mTextMutex);
              // BANK 模式显示选中键的绑定文本
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
        MakeMomentary(IRECT(20, 122, 116, 154), [this](IControl *) {
          if (GetParam(kMapMode)->Int() == 1 && mUISelected >= 0)
            OnNoteOnFromUI(mUISelected);
          else
            OnNoteOnFromUI(std::clamp(GetParam(kBaseKey)->Int(), 0, 127));
        }, "PLAY", btnStyle);
    pGraphics->AttachControl(playBtn);
    bindText(orm::kTxtPlay, [playBtn](const char *s) {
      playBtn->SetLabelStr(s);
      playBtn->SetDirty(false);
    });

    IVButtonControl *clearBtn =
        MakeMomentary(IRECT(122, 122, 218, 154), [this](IControl *) {
          if (GetParam(kMapMode)->Int() == 1 && mUISelected >= 0) {
            std::lock_guard<std::mutex> lock(mTextMutex);
            mBank.erase(mUISelected); // 解除选中键的绑定
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

    mTimeline = new UtteranceTimelineControl(IRECT(20, 162, kLeftR, 306));
    pGraphics->AttachControl(mTimeline, kCtrlTagTimeline);
    bindTip(mTimeline, orm::kTxtTipTimeline);

    // ---- 引擎选择 + 参数列 (右列) ----
    mEngineSegment = new FlatSegmentControl(
        IRECT(kRightL, 16, kRightR, 40),
        std::vector<std::string>{"SAM", "TMS", "TSI", "SP0256", "DECTalk"},
        [this](int idx) { SetEngineFromUI(idx); },
        GetParam(kEngine)->Int(), 15.f);
    pGraphics->AttachControl(mEngineSegment);

    // SAM 文本/音素切换 (仅 SAM 引擎显示; 与其他引擎的语音选择段同一行)
    mPhoneticSegment = new FlatSegmentControl(
        IRECT(kRightL, 46, kRightR, 74),
        std::vector<std::string>{orm::Tr(orm::kTxtText, orm::UILang()),
                                 orm::Tr(orm::kTxtPhonetic, orm::UILang())},
        [this](int idx) { SetPhoneticMode(idx == 1); }, mPhonetic ? 1 : 0);
    pGraphics->AttachControl(mPhoneticSegment);

    // TMS 音色/词库选择 (SAM 时隐藏; 显式处理器 SetVoiceFromUI, 不依赖 OnParamChange)
    mVoiceSegment = new FlatSegmentControl(
        IRECT(kRightL, 46, kRightR, 74),
        std::vector<std::string>{"MIL", "TI99", "ACORN", "S&S", "CLOCK"},
        [this](int idx) { SetVoiceFromUI(idx); },
        GetParam(kTmsBank)->Int());
    pGraphics->AttachControl(mVoiceSegment);

    // TSI 子集选择 (仅 TSI 引擎显示; 9 段宽度有限用短标签)
    mTsiVoiceSegment = new FlatSegmentControl(
        IRECT(kRightL, 46, kRightR, 74),
        std::vector<std::string>{"BZ", "F2", "C0", "C1", "C2", "C3", "C4", "C5", "C6"},
        [this](int idx) { SetTsiVoiceFromUI(idx); },
        GetParam(kTsiBank)->Int());
    pGraphics->AttachControl(mTsiVoiceSegment);

    // SP0256 输入模式选择 (仅 SP0256 引擎显示; 与 SAM 同款 文本/音素 控件)
    mSp0256VoiceSegment = new FlatSegmentControl(
        IRECT(kRightL, 46, kRightR, 74),
        std::vector<std::string>{orm::Tr(orm::kTxtText, orm::UILang()),
                                 orm::Tr(orm::kTxtPhonetic, orm::UILang())},
        [this](int idx) { SetSp0256VoiceFromUI(idx); },
        GetParam(kSp0256Voice)->Int());
    pGraphics->AttachControl(mSp0256VoiceSegment);

    // DECTALK 音色选择 (仅 DECTALK 引擎显示; 9 段放不下全名, 用 DECtalk 自己的
    // 2 字符音色码, 与 TSI 段一致; 完整名单见 kDectalkVoice 参数与提示)
    mDectalkVoiceSegment = new FlatSegmentControl(
        IRECT(kRightL, 46, kRightR, 74),
        std::vector<std::string>{"NP", "NB", "NH", "NF", "ND", "NK", "NU", "NR", "NW"},
        [this](int idx) { SetDectalkVoiceFromUI(idx); },
        GetParam(kDectalkVoice)->Int());
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
        {kGain, orm::kTxtOutput, -1, "Output"},
    };
    const int nSliders = (int)(sizeof(kSliders) / sizeof(kSliders[0]));
    for (int i = 0; i < nSliders; ++i) {
      const float y = 84.f + i * 48.f;
      ORMSlider *sl = new ORMSlider(IRECT(kRightL, y, kRightR, y + 40.f), kSliders[i].param,
                                    kSliders[i].fallback, style, EDirection::Horizontal);
      mParamSliders[i] = sl;
      pGraphics->AttachControl(sl);
      sl->SetHeaderFont(kFontRegular);
      bindText(kSliders[i].txtId, [sl](const char *s) { sl->SetHeaderLabel(s); });
      if (kSliders[i].tipId >= 0)
        bindTip(sl, kSliders[i].tipId);
    }
    // 数值格式: 整型参数 / 毫秒 / 分贝 (TMS 语速显示为速率倍数)
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
              // DECTalk 平均音高: 0 = 音色原生 (不覆盖), 否则显示 Hz
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
    mParamSliders[6]->SetValueFormatter([](WDL_String &ds, const IParam *p) {
      char buf[32];
      snprintf(buf, sizeof(buf), "%.1f dB", p->Value());
      ds.Set(buf);
    });

    // 单音/复音 (重触发已移除, 始终从头重放)
    FlatToggleControl *monoToggle =
        new FlatToggleControl(IRECT(kRightL, 424, kRightR, 452), kMono, " ", toggleStyle,
                              orm::Tr(orm::kTxtPoly, orm::UILang()), orm::Tr(orm::kTxtMono, orm::UILang()));
    pGraphics->AttachControl(monoToggle);
    bindText(orm::kTxtMono, [monoToggle](const char *s) { monoToggle->SetOnText(s); });
    bindText(orm::kTxtPoly, [monoToggle](const char *s) { monoToggle->SetOffText(s); });

    ORMSlider *baseKeySlider =
        new ORMSlider(IRECT(kRightL, 460, kRightR, 500), kBaseKey,
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

    // ---- 键盘 (C2..C6, 右侧留出标题块位置) ----
    mKeyboard = new PianoKeyboardControl(
        IRECT(20, 504, 616, 660),
        PianoKeyboardControl::Hooks{
            [this](int note) { OnNoteOnFromUI(note); },
            [this](int note) { OnNoteOffFromUI(note); },
            [this]() { return std::clamp(GetParam(kBaseKey)->Int(), 0, 127); },
        },
        PianoKeyboardControl::kLowNoteDefault, PianoKeyboardControl::kHighNoteDefault);
    pGraphics->AttachControl(mKeyboard, kCtrlTagKeyboard);
    bindTip(mKeyboard, orm::kTxtTipKeyboard);

    // ---- 右下角标题块 (与 Analyzer 同构: 底部锚定, 上缘 615, 底到 684) ----
    constexpr float kTitleX = 784.f;
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

    // ---- 设置面板 ----
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
    // 乐器插件无音频输入: 设置面板隐藏输入行
    mSettingsPanel = new SettingsPanelControl(IRECT(0.f, 0.f, (float)PLUG_WIDTH, (float)PLUG_HEIGHT), settingsHooks, false);
    mSettingsPanel->SetVisible(false);
    pGraphics->AttachControl(mSettingsPanel);

    // 初始引擎下把 4 个音色滑块槽改绑到对应参数
    RebindVoiceSliders(GetParam(kEngine)->Int());

    pGraphics->EnableTooltips(true);
    ApplyLanguage();
  };
#endif
}

// ---- 编辑器侧入口 ----

void ORMNarrator::OnNoteOnFromUI(int note) {
  // 注意: iPlug2 的 IMidiMsg 用打包 nibble 语义 (mStatus = channel | (type<<4)),
  // 必须经 MakeNoteOnMsg 构造, 不能手写原始 MIDI 字节
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
    // BANK 模式下同步写入选中键的绑定
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
}

void ORMNarrator::SetEngineFromUI(int idx) {
  const int v = std::clamp(idx, 0, 4);
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
  // 4 个音色滑块槽按引擎改绑 (SAM: pitch/speed/mouth/throat; TMS: pitch/speed + 灰)
  RebindVoiceSliders(v);
#if IPLUG_EDITOR
  if (GetUI())
    GetUI()->SetAllControlsDirty();
#endif
}

void ORMNarrator::SetVoiceFromUI(int idx) {
  const int v = std::clamp(idx, 0, kNumBanks - 1);
  GetParam(kTmsBank)->Set((double) v);
  InformHostOfParamChange(kTmsBank, GetParam(kTmsBank)->GetNormalized());
  mRenderDirty = true; // 分段控件不走参数联动, 必须显式置脏才会重渲染
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
#if IPLUG_EDITOR
  if (GetUI())
    GetUI()->SetAllControlsDirty();
#endif
}

void ORMNarrator::SetTsiVoiceFromUI(int idx) {
  const int v = std::clamp(idx, 0, kNumS14001Sets - 1);
  GetParam(kTsiBank)->Set((double) v);
  InformHostOfParamChange(kTsiBank, GetParam(kTsiBank)->GetNormalized());
  mRenderDirty = true; // 分段控件不走参数联动, 必须显式置脏才会重渲染
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
  GetParam(kSp0256Voice)->Set((double) v);
  InformHostOfParamChange(kSp0256Voice, GetParam(kSp0256Voice)->GetNormalized());
  mRenderDirty = true; // 分段控件不走参数联动, 必须显式置脏才会重渲染
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
  GetParam(kDectalkVoice)->Set((double) v);
  InformHostOfParamChange(kDectalkVoice, GetParam(kDectalkVoice)->GetNormalized());
  mRenderDirty = true; // 分段控件不走参数联动, 必须显式置脏才会重渲染
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
  GetParam(kMapMode)->Set((double) v);
  InformHostOfParamChange(kMapMode, GetParam(kMapMode)->GetNormalized());
  if (mMapSegment)
    mMapSegment->SetActive(v);
  if (v == 1) {
    // 进入 BANK: 始终有编辑目标 —— 沿用当前选中键, 否则自动选中基准键
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
  // 语言切换后重刷滑杆槽绑定 (TSI 槽 0 的头部标签是动态的)
  RebindVoiceSliders((int) GetParam(kEngine)->Int());
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

void ORMNarrator::OnIdle() {
  if (!GetUI())
    return;

  ApplySelectionFromIdle();

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

  // 引擎/音色参数已由 RebindVoiceSliders 管理绑定的滑块; 宿主侧改引擎时同步重绑
  const int eng = (int) GetParam(kEngine)->Int();
  const int expectIdx = (eng == 1) ? kTmsPitch : (eng == 2) ? kTsiSpeed
                                                           : (eng == 3) ? kSp0256Speed
                                                                        : (eng == 4) ? kDectalkPitch : kSamPitch;
  const bool bound = mParamSliders[0] ? (mParamSliders[0]->GetParamIdx() == expectIdx) : false;
  if (!bound)
    RebindVoiceSliders(eng);
  // 宿主自动化改引擎/音色/模式时, 分段控件高亮跟随
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

// 拖拽 App 窗口/宿主缩放视图时, 保持布局逻辑尺寸不变, 按两轴较大比例等比缩放 UI
// (edge-crop, 与 Analyzer/BandPass 一致)。needsPlatformResize=false 避免在 live
// resize 中反向改动窗口尺寸形成回路; 窗口宽高比由 ConstrainEditorResize 锁定。
// 不重写本函数会落到 IGEditorDelegate 的默认实现, 它会把 drawScale 重置为 1 并把
// 逻辑尺寸改成窗口尺寸, 拖拽中缩放被宿主回声反复打回, 表现为跳动/不跟手。
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
  if (status == IMidiMsg::kNoteOn && msg.Velocity() > 0)
    mMidiQueue.push_back({msg.mOffset, true, msg.NoteNumber(), msg.Velocity()});
  else if (status == IMidiMsg::kNoteOff ||
           (status == IMidiMsg::kNoteOn && msg.Velocity() == 0))
    mMidiQueue.push_back({msg.mOffset, false, msg.NoteNumber(), 0});
}

void ORMNarrator::ProcessBlock(sample **inputs, sample **outputs, int nFrames) {
  nFrames = std::min(nFrames, kMaxBlock);
  if (nFrames <= 0) {
    mMidiQueue.clear();
    return;
  }

  const int nOuts = NOutChansConnected();
  if (nOuts <= 0) {
    mMidiQueue.clear();
    return;
  }
  sample *main = outputs[0];
  memset(main, 0, nFrames * sizeof(sample));

  // MIDI 事件按采样偏移分段处理
  int pos = 0;
  if (!mMidiQueue.empty()) {
    std::sort(mMidiQueue.begin(), mMidiQueue.end(),
              [](const MidiEvent &a, const MidiEvent &b) { return a.offset < b.offset; });
    for (const MidiEvent &ev : mMidiQueue) {
      const int off = std::clamp(ev.offset, pos, nFrames);
      RenderSegment(main, pos, off);
      if (ev.isNoteOn)
        TriggerVoice(ev.note);
      else
        ReleaseVoice(ev.note);
      pos = off;
    }
    mMidiQueue.clear();
  }
  RenderSegment(main, pos, nFrames);

  // 输出增益 + 声道复制
  const double gain = std::pow(10., GetParam(kGain)->Value() / 20.);
  for (int i = 0; i < nFrames; ++i)
    main[i] = (sample)(main[i] * gain);
  for (int c = 1; c < nOuts; ++c)
    memcpy(outputs[c], main, nFrames * sizeof(sample));

  // 进度上报: 有声部在响则取其进度, 全停则归零
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
      mRenderDirty = true; // 音色参数变化 -> 下次触发前重渲染
      // BANK 模式: 音色滑杆/音色段直接编辑选中键的绑定 (按参数所属引擎写入)
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
    // BANK 绑定表: 版本 + 数量 + 每项 (引擎, SAM 4 参, TMS 3 参, TSI 2 参,
    // SP0256 2 参, DECTALK 3 参, 文本, 音素标记)
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
        // v1 旧格式: note, engine, pitch, speed, mouth, throat, phon, text → SAM 字段
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
  return UnserializeParams(chunk, startPos);
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
  if (engine == 0) {
    ok = orm::SamEngine::Render(text, phon, s, mPhraseBuffer);
    rate = orm::SamEngine::kSampleRate;
  } else if (engine == 2) {
    // TSI S14001A: 文本 = 词索引 (Wnn / n), rateScale 缩放芯片时钟
    ok = orm::TsiS14001Engine::Render(text, tsiBank, tsiScale, mPhraseBuffer);
    rate = orm::TsiS14001Engine::RateForSet(tsiBank, tsiScale);
  } else if (engine == 3) {
    // SP0256: 文本 = allophone/单词标签或数字码, speedScale 缩放 XTAL
    ok = orm::Sp0256Engine::Render(text, spVariant, spScale, mPhraseBuffer);
    rate = orm::Sp0256Engine::RateFor(spScale);
  } else if (engine == 4) {
    // DECTALK: 文本 = 自由英语文本 (带数字/缩写/标点), 原生 11025 Hz;
    // 语速/音高经 DECtalk 原生机制 (保韵律), 键盘变调仍走 VoiceRenderer
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
    // TMS5220 家族: 文本 = 词库词名 (大写), speed 缩放帧时长
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
  if (e.engine == 0) {
    orm::SamEngine::Render(e.text, e.phonetic, e.sam, e.rendered);
  } else if (e.engine == 2) {
    orm::TsiS14001Engine::Render(e.text, e.tsi.bank, (float)(e.tsi.speed / 72.0), e.rendered);
  } else if (e.engine == 3) {
    orm::Sp0256Engine::Render(e.text, e.sp0256.variant, (float)(e.sp0256.speed / 72.0), e.rendered);
  } else if (e.engine == 4) {
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

  // 把选中绑定的文本与参数载入编辑器 (未绑定键载入空绑定)
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
      mBank[mUISelected] = BankEntry{}; // 建空绑定, 编辑即生效
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
  // 载入该绑定的引擎参数 (各引擎各载各的, 切引擎互不影响)
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
  RebindVoiceSliders(engine);
#if IPLUG_EDITOR
  if (GetUI()) {
    SendCurrentParamValuesFromDelegate();
    GetUI()->SetAllControlsDirty();
  }
#endif
}

void ORMNarrator::RebindVoiceSliders(int engine) {
  // 槽 0..3 = Pitch/Speed/Mouth/Throat (SAM); TMS 槽 2/3 置灰;
  // TSI/SP0256 仅槽 0 有效 (时钟/速率), 槽 1..3 置灰
  if (engine == 1) {
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
  } else if (engine == 2) {
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
  } else if (engine == 3) {
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
  } else if (engine == 4) {
    // DECTALK: 槽 0 = AP 平均音高 (Hz), 槽 1 = 说话速率 (wpm)
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
    // SAM
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
    mPhoneticSegment->Hide(engine != 0);
  if (mVoiceSegment)
    mVoiceSegment->Hide(engine != 1);
  if (mTsiVoiceSegment)
    mTsiVoiceSegment->Hide(engine != 2);
  if (mSp0256VoiceSegment)
    mSp0256VoiceSegment->Hide(engine != 3);
  if (mDectalkVoiceSegment)
    mDectalkVoiceSegment->Hide(engine != 4);
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
  const bool bank = GetParam(kMapMode)->Int() == 1;

  if (bank)
  {
    // BANK 模式: 键 = 绑定项 (词语 + 参数), 原速播放
    const BankEntry *entry = nullptr;
    {
      std::lock_guard<std::mutex> lock(mTextMutex);
      auto it = mBank.find(note);
      if (it != mBank.end())
        entry = &it->second;
    }
    if (mPendingSelect.exchange(note) != note)
      mSelectChanged = true; // 按键即选中 (编辑器随后载入其参数)

    if (!entry)
      return; // 未绑定: 仅选中供编辑, 不发声

    BankEntry *e = const_cast<BankEntry *>(entry);
    if (e->dirty)
      RenderBankEntry(*e);
    if (e->rendered.empty())
      return;

    const bool mono = GetParam(kMono)->Value() > 0.5;
    if (mono)
    {
      // 单音: 停掉当前音后从头顶重放 (重触发始终开启)
      for (Voice &v : mVoices)
        if (v.renderer.IsPlaying())
          v.renderer.Release();
    }

    Voice &v = mVoices[(size_t) mNextVoice];
    mNextVoice = (mNextVoice + 1) % kMaxVoices;
    v.note = note;
    v.renderer.SetEnvelope(GetParam(kAttack)->Value(), GetParam(kRelease)->Value());
    const double bankRate = e->engine == 0 ? orm::SamEngine::kSampleRate
                           : e->engine == 2 ? orm::TsiS14001Engine::RateForSet(e->tsi.bank, (float)(e->tsi.speed / 72.0))
                           : e->engine == 3 ? orm::Sp0256Engine::RateFor((float)(e->sp0256.speed / 72.0))
                           : e->engine == 4 ? orm::DectalkEngine::kSampleRate
                           : (e->tms.bank == kBankSspell) ? orm::Tms5110Engine::kSampleRate
                                                          : orm::Tms5220Engine::kSampleRate;
    v.renderer.SetPhrase(e->rendered.data(), (int) e->rendered.size(), bankRate, GetSampleRate());
    v.renderer.Trigger(e->engine == 1
                           ? std::pow(2., (e->tms.pitch - 64.) / 24.) // TMS 音高以 varispeed 近似
                           : 1.0); // 绑定项原速
    PushBankPhraseToUI(*e);
    return;
  }

  // PHRASE 模式: 单句变调
  EnsureRendered();
  if (mPhraseBuffer.empty())
    return;

  const bool mono = GetParam(kMono)->Value() > 0.5;

  if (mono)
  {
    // 单音: 停掉当前音后从头顶重放 (重触发始终开启)
    for (Voice &v : mVoices)
      if (v.renderer.IsPlaying())
        v.renderer.Release();
  }

  Voice &v = mVoices[(size_t) mNextVoice];
  mNextVoice = (mNextVoice + 1) % kMaxVoices;
  v.note = note;
  v.renderer.SetEnvelope(GetParam(kAttack)->Value(), GetParam(kRelease)->Value());
  const int engine = (int) GetParam(kEngine)->Int();
  const int bankIdx = (int) GetParam(kTmsBank)->Int();
  const int tsiBank = (int) GetParam(kTsiBank)->Int();
  const double phraseRate =
      (engine == 0) ? orm::SamEngine::kSampleRate
      : (engine == 2) ? orm::TsiS14001Engine::RateForSet(tsiBank, (float)(GetParam(kTsiSpeed)->Value() / 72.0))
      : (engine == 3) ? orm::Sp0256Engine::RateFor((float)(GetParam(kSp0256Speed)->Value() / 72.0))
      : (engine == 4) ? orm::DectalkEngine::kSampleRate
      : (bankIdx == kBankSspell) ? orm::Tms5110Engine::kSampleRate
                                 : orm::Tms5220Engine::kSampleRate;
  v.renderer.SetPhrase(mPhraseBuffer.data(), (int) mPhraseBuffer.size(), phraseRate,
                       GetSampleRate());
  double ratio = PitchRatioForNote(note);
  if (engine == 1)
    ratio *= std::pow(2., (GetParam(kTmsPitch)->Value() - 64.) / 24.); // TMS 音高参数以 varispeed 近似
  v.renderer.Trigger(ratio);
}

void ORMNarrator::ReleaseVoice(int note) {
  for (Voice &v : mVoices)
    if (v.note == note && v.renderer.IsPlaying())
      v.renderer.Release();
}

double ORMNarrator::PitchRatioForNote(int note) const {
  const int base = (int) GetParam(kBaseKey)->Value();
  return std::pow(2., (double) (note - base) / 12.);
}

void ORMNarrator::RenderSegment(sample *out, int from, int to) {
  for (Voice &v : mVoices)
    v.renderer.ProcessAdd(out + from, to - from);
}
#endif
