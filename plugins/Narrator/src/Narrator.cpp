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

  GetParam(kEngine)->InitEnum("Engine", 0, {"SAM", "TMS"});
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
  // 通用
  GetParam(kAttack)->InitDouble("Attack", 5., 1., 500., 1., "ms");
  GetParam(kRelease)->InitDouble("Release", 120., 1., 2000., 1., "ms");
  GetParam(kRetrig)->InitBool("Retrig", true);
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

    mModeSegment = new FlatSegmentControl(
        IRECT(226, 122, kLeftR, 154),
        std::vector<std::string>{orm::Tr(orm::kTxtText, orm::UILang()),
                                 orm::Tr(orm::kTxtPhonetic, orm::UILang())},
        [this](int idx) { SetPhoneticMode(idx == 1); }, mPhonetic ? 1 : 0);
    pGraphics->AttachControl(mModeSegment);

    mTimeline = new UtteranceTimelineControl(IRECT(20, 162, kLeftR, 306));
    pGraphics->AttachControl(mTimeline, kCtrlTagTimeline);
    bindTip(mTimeline, orm::kTxtTipTimeline);

    // ---- 引擎选择 + 参数列 (右列) ----
    mEngineSegment = new FlatSegmentControl(
        IRECT(kRightL, 16, kRightR, 40),
        std::vector<std::string>{"SAM", "TMS"},
        [this](int idx) { SetEngineFromUI(idx); },
        GetParam(kEngine)->Int());
    pGraphics->AttachControl(mEngineSegment);

    // TMS 音色/词库选择 (SAM 时隐藏; 显式处理器 SetVoiceFromUI, 不依赖 OnParamChange)
    mVoiceSegment = new FlatSegmentControl(
        IRECT(kRightL, 44, kRightR, 72),
        std::vector<std::string>{"MIL", "TI99", "ACORN", "S&S", "CLOCK"},
        [this](int idx) { SetVoiceFromUI(idx); },
        GetParam(kTmsBank)->Int());
    pGraphics->AttachControl(mVoiceSegment);

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
      const float y = 76.f + i * 44.f;
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
          [intFmt](WDL_String &ds, const IParam *p) { intFmt(ds, p); });
    }
    mParamSliders[1]->SetValueFormatter([this](WDL_String &ds, const IParam *p) {
      if (!p)
        return;
      if (p == GetParam(kTmsSpeed)) {
        char buf[32];
        snprintf(buf, sizeof(buf), "x%.2f", p->Value() / 72.0);
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

    // 单音/复音 + 重触发
    FlatToggleControl *monoToggle =
        new FlatToggleControl(IRECT(kRightL, 384, kRightL + 138.f, 412), kMono, " ", toggleStyle,
                              orm::Tr(orm::kTxtPoly, orm::UILang()), orm::Tr(orm::kTxtMono, orm::UILang()));
    pGraphics->AttachControl(monoToggle);
    bindText(orm::kTxtMono, [monoToggle](const char *s) { monoToggle->SetOnText(s); });
    bindText(orm::kTxtPoly, [monoToggle](const char *s) { monoToggle->SetOffText(s); });
    FlatToggleControl *retrigToggle =
        new FlatToggleControl(IRECT(kRightL + 146.f, 384, kRightR, 412), kRetrig, " ", toggleStyle,
                              orm::Tr(orm::kTxtRetrig, orm::UILang()), orm::Tr(orm::kTxtRetrig, orm::UILang()));
    pGraphics->AttachControl(retrigToggle);
    bindText(orm::kTxtRetrig, [retrigToggle](const char *s) {
      retrigToggle->SetOnText(s);
      retrigToggle->SetOffText(s);
    });

    ORMSlider *baseKeySlider =
        new ORMSlider(IRECT(kRightL, 420, kRightR, 460), kBaseKey,
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
        IRECT(20, 482, 772, 660),
        PianoKeyboardControl::Hooks{
            [this](int note) { OnNoteOnFromUI(note); },
            [this](int note) { OnNoteOffFromUI(note); },
            [this]() { return std::clamp(GetParam(kBaseKey)->Int(), 0, 127); },
        },
        PianoKeyboardControl::kLowNoteDefault, PianoKeyboardControl::kHighNoteDefault);
    pGraphics->AttachControl(mKeyboard, kCtrlTagKeyboard);
    bindTip(mKeyboard, orm::kTxtTipKeyboard);

    // ---- 右下角标题块 (与 BandPass 同构) ----
    IText ormText(32, COL_900(), kFontBold, EAlign::Near, EVAlign::Bottom);
    SectionTitleControl *ormTitle = new SectionTitleControl(IRECT(784, 482, 940, 526), "ORM", ormText, 0);
    pGraphics->AttachControl(ormTitle);
    IRECT ormInk(784, 482, 940, 526);
    pGraphics->MeasureText(ormText, "ORM", ormInk);
    const float gearL = ormInk.R + 8.f;
    const float gearR = gearL + (ormInk.B - ormInk.T);
    pGraphics->AttachControl(new SettingsMenuButton(IRECT(gearL, ormInk.T, gearR, ormInk.B),
                                                    [this]() { ToggleSettingsPanel(); }));
    pGraphics->AttachControl(new SectionTitleControl(IRECT(784, 528, 940, 578), "Narrator",
                                                     IText(32, COL_900(), kFontBold, EAlign::Near, EVAlign::Middle),
                                                     0));
    pGraphics->AttachControl(new SectionTitleControl(IRECT(784, 580, 940, 614), "v" PLUG_VERSION_STR,
                                                     IText(20, COL_500(), kFontRegular, EAlign::Near, EVAlign::Bottom),
                                                     1, 0));

    // 底部提示行
    SectionTitleControl *hint =
        new SectionTitleControl(IRECT(20, 668, 940, 706), "",
                                IText(14, COL_500(), kFontRegular, EAlign::Center, EVAlign::Middle), 1, 0);
    pGraphics->AttachControl(hint);
    bindText(orm::kTxtClickToTalk, [hint](const char *s) {
      hint->SetStr(s);
      hint->SetDirty(false);
    });

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
  if (mModeSegment)
    mModeSegment->SetActive(phonetic ? 1 : 0);
}

void ORMNarrator::SetEngineFromUI(int idx) {
  const int v = std::clamp(idx, 0, 1);
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
  if (mModeSegment) {
    mModeSegment->SetLabels({orm::Tr(orm::kTxtText, orm::UILang()),
                             orm::Tr(orm::kTxtPhonetic, orm::UILang())});
  }
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
  const bool tmsNow = GetParam(kEngine)->Int() == 1;
  const bool tmsBound = mParamSliders[0] ? (mParamSliders[0]->GetParamIdx() == kTmsPitch) : false;
  if (tmsNow != tmsBound)
    RebindVoiceSliders(tmsNow ? 1 : 0);
  // 宿主自动化改引擎/音色/模式时, 分段控件高亮跟随
  if (mEngineSegment)
    mEngineSegment->SetActive((int) GetParam(kEngine)->Int());
  if (mVoiceSegment)
    mVoiceSegment->SetActive((int) GetParam(kTmsBank)->Int());
  if (mMapSegment)
    mMapSegment->SetActive((int) GetParam(kMapMode)->Int());
}
#endif

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
    // BANK 绑定表: 版本 + 数量 + 每项 (引擎, SAM 4 参, TMS 3 参, 文本, 音素标记)
    const int ver = 2;
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
        startPos = chunk.Get(&note, startPos);
        startPos = chunk.Get(&eng, startPos);
        startPos = chunk.Get(&sPit, startPos);
        startPos = chunk.Get(&sSpd, startPos);
        startPos = chunk.Get(&sMou, startPos);
        startPos = chunk.Get(&sThr, startPos);
        startPos = chunk.Get(&tSpd, startPos);
        startPos = chunk.Get(&tPit, startPos);
        startPos = chunk.Get(&tBnk, startPos);
        startPos = chunk.Get(&phn, startPos);
        startPos = chunk.GetStr(str, startPos);
        if (startPos < 0)
          break;
        BankEntry e;
        e.text = str.Get();
        e.phonetic = phn != 0;
        e.engine = std::clamp(eng, 0, 1);
        e.sam.pitch = std::clamp(sPit, 0, 255);
        e.sam.speed = std::clamp(sSpd, 1, 255);
        e.sam.mouth = std::clamp(sMou, 0, 255);
        e.sam.throat = std::clamp(sThr, 0, 255);
        e.tms.speed = std::clamp(tSpd, 1, 255);
        e.tms.pitch = std::clamp(tPit, 0, 255);
        e.tms.bank = std::clamp(tBnk, 0, kNumBanks - 1);
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
  const float tmsScale = (float)(GetParam(kTmsSpeed)->Value() / 72.0);
  bool ok = false;
  if (engine == 0) {
    ok = orm::SamEngine::Render(text, phon, s, mPhraseBuffer);
  } else if (bank == kBankSspell) {
    ok = orm::Tms5110Engine::Render(text, tmsScale, mPhraseBuffer);
  } else {
    // TMS5220 家族: 文本 = 词库词名 (大写), speed 缩放帧时长
    ok = orm::Tms5220Engine::Render(text, bank, tmsScale, mPhraseBuffer);
  }

  if (ok) {
    const double rate = (engine == 0) ? orm::SamEngine::kSampleRate
                       : (bank == kBankSspell) ? orm::Tms5110Engine::kSampleRate
                                               : orm::Tms5220Engine::kSampleRate;
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
  {
    std::lock_guard<std::mutex> lock(mTextMutex);
    auto it = mBank.find(mUISelected);
    if (it != mBank.end()) {
      text = it->second.text;
      phonetic = it->second.phonetic;
      engine = it->second.engine;
      sam = it->second.sam;
      tms = it->second.tms;
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
      mBank[mUISelected].sam = sam;
      mBank[mUISelected].tms = tms;
      mBank[mUISelected].engine = engine;
    }
  }
  // 载入该绑定的引擎参数 (SAM 与 TMS 各载各的, 切引擎互不影响)
  GetParam(kEngine)->Set((double) engine);
  GetParam(kSamPitch)->Set((double) sam.pitch);
  GetParam(kSamSpeed)->Set((double) sam.speed);
  GetParam(kSamMouth)->Set((double) sam.mouth);
  GetParam(kSamThroat)->Set((double) sam.throat);
  GetParam(kTmsSpeed)->Set((double) tms.speed);
  GetParam(kTmsPitch)->Set((double) tms.pitch);
  GetParam(kTmsBank)->Set((double) tms.bank);
  {
    std::lock_guard<std::mutex> lock(mTextMutex);
    mPhonetic = phonetic;
  }
  if (mEngineSegment)
    mEngineSegment->SetActive(engine);
  if (mVoiceSegment)
    mVoiceSegment->SetActive(tms.bank);
  RebindVoiceSliders(engine);
#if IPLUG_EDITOR
  if (GetUI()) {
    SendCurrentParamValuesFromDelegate();
    GetUI()->SetAllControlsDirty();
  }
#endif
}

void ORMNarrator::RebindVoiceSliders(int engine) {
  // 槽 0/1 = Pitch/Speed 两种引擎都有效; 槽 2/3 = Mouth/Throat 仅 SAM (TMS 置灰断开)
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
  } else {
    const int samIdx[4] = {kSamPitch, kSamSpeed, kSamMouth, kSamThroat};
    for (int i = 0; i < 4; ++i) {
      if (mParamSliders[i]) {
        mParamSliders[i]->SetParamIdx(samIdx[i]);
        mParamSliders[i]->SetValueFromDelegate(GetParam(samIdx[i])->GetNormalized());
        mParamSliders[i]->SetGhost(false);
      }
    }
  }
  if (mVoiceSegment)
    mVoiceSegment->Hide(engine != 1);
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
    const bool retrig = GetParam(kRetrig)->Value() > 0.5;
    if (mono)
    {
      bool anyPlaying = false;
      for (Voice &v : mVoices)
        if (v.renderer.IsPlaying())
        {
          anyPlaying = true;
          v.renderer.Release();
        }
      if (anyPlaying && !retrig)
        return;
    }
    else if (!retrig)
    {
      for (Voice &v : mVoices)
        if (v.note == note && v.renderer.IsPlaying())
          return;
    }

    Voice &v = mVoices[(size_t) mNextVoice];
    mNextVoice = (mNextVoice + 1) % kMaxVoices;
    v.note = note;
    v.renderer.SetEnvelope(GetParam(kAttack)->Value(), GetParam(kRelease)->Value());
    const double bankRate = e->engine == 0 ? orm::SamEngine::kSampleRate
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
  const bool retrig = GetParam(kRetrig)->Value() > 0.5;

  if (mono)
  {
    bool anyPlaying = false;
    for (Voice &v : mVoices)
      if (v.renderer.IsPlaying())
      {
        anyPlaying = true;
        v.renderer.Release();
      }
    if (anyPlaying && !retrig)
      return; // 单音 + 不重触发: 忙时忽略
  }
  else if (!retrig)
  {
    for (Voice &v : mVoices)
      if (v.note == note && v.renderer.IsPlaying())
        return; // 复音 + 不重触发: 同音忽略
  }

  Voice &v = mVoices[(size_t) mNextVoice];
  mNextVoice = (mNextVoice + 1) % kMaxVoices;
  v.note = note;
  v.renderer.SetEnvelope(GetParam(kAttack)->Value(), GetParam(kRelease)->Value());
  const int engine = (int) GetParam(kEngine)->Int();
  const int bankIdx = (int) GetParam(kTmsBank)->Int();
  const double phraseRate = (engine == 0) ? orm::SamEngine::kSampleRate
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
