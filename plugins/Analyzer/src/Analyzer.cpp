#include "Analyzer.h"
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
#include "controls/SpectrumPad.h"
#include "controls/CpuMeterControl.h"
#include "StateFileIO.h"
#include "SettingsFileIO.h"

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

// 手势撤销的时间窗: 两次 UI 改动间隔超过该值时, 下一次改动前推一次撤销快照
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

ORMAnalyzer::ORMAnalyzer(const InstanceInfo &info) : Plugin(info, MakeConfig(kNumParams, 1)) {
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
  // Analyzer 目前为纯分析器: mix 参数保留自 BandPass, 作为撤销/重做、保存/读取的
  // 载体 (DSP 直通, 暂不参与处理); release/attack 参数控制频谱显示的释放/上升
  // 时间 (s); range 参数控制频谱显示下限 (-80..-120 dBFS, 存正数幅度);
  // res/lfRes/bpo 为 FFT 尺寸 / VQT 低频带宽下限 γ / VQT bins-per-octave 档位索引
  // (映射见 Params.h); mode 选择分析引擎 (FFT / VQT)。
  GetParam(kMix)->InitDouble("Mix", 1., 0., 1., 0.01, "");
  GetParam(kRelease)->InitDouble("Release", 0.2, 0.05, 0.5, 0.01, "s");
  GetParam(kRange)->InitDouble("Range", 90, 80, 120, 10, "");
  GetParam(kAttack)->InitDouble("Attack", 0.05, 0.001, 0.1, 0.001, "s");
  GetParam(kRes)->InitInt("Res", kNumResOptions - 1, 0, kNumResOptions - 1, "");
  GetParam(kLfRes)->InitInt("LfRes", 0, 0, kNumLfResOptions - 1, "");
  GetParam(kBpo)->InitInt("Bpo", kNumBpoOptions - 1, 0, kNumBpoOptions - 1, "");
  GetParam(kMode)->InitInt("Mode", kModeFFT, 0, 1, "");

  mDefaultSnapshot = Snapshot();
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
    // Windows: 系统字体 fallback 链, 必须落在含完整 CJK 的字体上。
    sysFontOk = pGraphics->LoadFont(kFontSystem, "Microsoft YaHei", ETextStyle::Normal);
    if (!sysFontOk)
      sysFontOk = pGraphics->LoadFont(kFontSystem, "Microsoft YaHei UI", ETextStyle::Normal);
    if (!sysFontOk)
      sysFontOk = pGraphics->LoadFont(kFontSystem, "Segoe UI", ETextStyle::Normal);
    if (!sysFontOk) {
      static const char *kSysFontFiles[] = {
          "C:\\Windows\\Fonts\\msyh.ttc",    // 微软雅黑 (Vista+, TTC 集合)
          "C:\\Windows\\Fonts\\msyh.ttf",
          "C:\\Windows\\Fonts\\Deng.ttf",    // 等线 (Win8+, TTF)
          "C:\\Windows\\Fonts\\Nsimsun.ttf", // 新宋体 (TTF)
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

    // 频谱显示 pad: 空 xypad, 位置 = BandPass 原 LEFT 频谱 (下半部分预留做别的)
    mSpectrumPad = new SpectrumPad(IRECT(20, 30, 668, 210));
    pGraphics->AttachControl(mSpectrumPad, kCtrlTagPad);

    // CPU 占用率显示 (右上角, 横向占满右列整行 156x30, 与按钮行同宽, 只读, 实时刷新)
    mCpuMeter = new CpuMeterControl(IRECT(kCol1X, 30, kPanelR, 60));
    pGraphics->AttachControl(mCpuMeter, kCtrlTagCpu);

    // BPO (VQT bins per octave 档位 12/24, 仅 VQT 模式生效; 位于引擎切换按钮上方)
    mBpoSlider =
        new ORMSlider(IRECT(kCol1X, 140, kPanelR, 182), kBpo, "BPO", style, EDirection::Horizontal);
    pGraphics->AttachControl(mBpoSlider);
    bindText(orm::kTxtBpo, [this](const char *s) { mBpoSlider->SetHeaderLabel(s); });
    bindTip(mBpoSlider, orm::kTxtTipBpo);

    // 分析引擎切换按钮 (FFT / VQT, 与 CPU 框同宽; VQT 时黑底白字,
    // 样式参考 BandPass 的 LINK 按钮 = FlatToggleControl)
    IVStyle toggleStyle = btnStyle;
    toggleStyle.showLabel = false;
    toggleStyle.showValue = false;
    mModeToggle = new FlatToggleControl(IRECT(kCol1X, 192, kPanelR, 222), kMode, " ", toggleStyle, "FFT",
                                        "VQT");
    pGraphics->AttachControl(mModeToggle);
    bindTip(mModeToggle, orm::kTxtTipMode);

    // RES / LF RES 滑块 (FFT 模式为 FFT 尺寸档位, VQT 模式切换为低频分辨率档位)
    mResSlider =
        new ORMSlider(IRECT(kCol1X, 234, kPanelR, 276), kRes, "RES", style, EDirection::Horizontal);
    pGraphics->AttachControl(mResSlider);
    bindText(orm::kTxtRes, [this](const char *) { UpdateResHeader(); });
    bindText(orm::kTxtLfRes, [this](const char *) { UpdateResHeader(); });
    bindTip(mResSlider, orm::kTxtTipRes);

    // RANGE (频谱显示下限 dBFS, 列顶)
    mRangeSlider =
        new ORMSlider(IRECT(kCol1X, 282, kPanelR, 324), kRange, "RANGE", style, EDirection::Horizontal);
    pGraphics->AttachControl(mRangeSlider);
    bindText(orm::kTxtRange, [this](const char *s) { mRangeSlider->SetHeaderLabel(s); });
    bindTip(mRangeSlider, orm::kTxtTipRange);

    // ATTACK (频谱显示上升时间, 位于 RELEASE 上方)
    mAttackSlider =
        new ORMSlider(IRECT(kCol1X, 328, kPanelR, 370), kAttack, "ATTACK", style, EDirection::Horizontal);
    pGraphics->AttachControl(mAttackSlider);
    bindText(orm::kTxtAttack, [this](const char *s) { mAttackSlider->SetHeaderLabel(s); });
    bindTip(mAttackSlider, orm::kTxtTipAttack);

    // RELEASE (频谱显示释放时间, 位于 MIX 上方)
    mReleaseSlider =
        new ORMSlider(IRECT(kCol1X, 374, kPanelR, 416), kRelease, "RELEASE", style, EDirection::Horizontal);
    pGraphics->AttachControl(mReleaseSlider);
    bindText(orm::kTxtRelease, [this](const char *s) { mReleaseSlider->SetHeaderLabel(s); });
    bindTip(mReleaseSlider, orm::kTxtTipRelease);

    // MIX (保留自 BandPass, 位置不变)
    mMixSlider = new ORMSlider(IRECT(kCol1X, 420, kPanelR, 462), kMix, "MIX", style, EDirection::Horizontal);
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

    // ORM logo + 设置齿轮 + 标题 "Analyzer" + 版本 (标题替换自 BandPass)
    IText ormText(32, COL_900(), kFontBold, EAlign::Near, EVAlign::Bottom);
    pGraphics->AttachControl(new SectionTitleControl(IRECT(kCol1X, 544, kCol1X + 120, 578), "ORM", ormText, 0));
    IRECT ormInk(kCol1X, 544, kCol1X + 120, 578);
    pGraphics->MeasureText(ormText, "ORM", ormInk);
    const float gearL = ormInk.R + 8.f;
    const float gearR = gearL + (ormInk.B - ormInk.T);
    pGraphics->AttachControl(
        new SettingsMenuButton(IRECT(gearL, ormInk.T, gearR, ormInk.B), [this]() { ToggleSettingsPanel(); }));
    pGraphics->AttachControl(new SectionTitleControl(IRECT(kCol1X, 576, kPanelR, 610), "Analyzer",
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
    mSettingsPanel = new SettingsPanelControl(IRECT(0.f, 0.f, (float)PLUG_WIDTH, (float)PLUG_HEIGHT), settingsHooks);
    mSettingsPanel->SetVisible(false);
    pGraphics->AttachControl(mSettingsPanel);

    pGraphics->EnableTooltips(true);
    ApplyLanguage();
  };
#endif
}

#if IPLUG_DSP
void ORMAnalyzer::ProcessBlock(sample **inputs, sample **outputs, int nFrames) {
  // CPU 占用率测量: 记录本块处理起点 (全程耗时 / 块时长 = 占用率, 一阶平滑)
  const auto cpuT0 = std::chrono::steady_clock::now();

  // 宿主块尺寸可能超过 kMaxBlock（定长缓冲上限），统一在此钳制。
  nFrames = std::min(nFrames, kMaxBlock);

  const int nOuts = NOutChansConnected();
  const int nIns = NInChansConnected();

  // Analyzer: 音频直通 (纯分析器, 不处理信号), 输出 = 输入
  if (nOuts >= 2 && nIns >= 2) {
    std::memcpy(outputs[0], inputs[0], nFrames * sizeof(sample));
    std::memcpy(outputs[1], inputs[1], nFrames * sizeof(sample));
    for (int c = 2; c < nOuts; ++c)
      std::memcpy(outputs[c], inputs[c], nFrames * sizeof(sample));
  } else if (nOuts >= 2) {
    std::memcpy(outputs[0], inputs[0], nFrames * sizeof(sample));
    std::memcpy(outputs[1], inputs[0], nFrames * sizeof(sample));
    for (int c = 2; c < nOuts; ++c)
      std::memcpy(outputs[c], outputs[0], nFrames * sizeof(sample));
  } else {
    std::memcpy(outputs[0], inputs[0], nFrames * sizeof(sample));
    for (int c = 1; c < nOuts; ++c)
      std::memcpy(outputs[c], outputs[0], nFrames * sizeof(sample));
  }

  // 频谱: 先快照输入再交给分析引擎 (输入/输出可能别名, 与 BandPass 一致)
  const bool vqt = GetParam(kMode)->Value() > 0.5;
  if (nIns >= 2) {
    std::memcpy(mSpecInL.data(), inputs[0], nFrames * sizeof(sample));
    std::memcpy(mSpecInR.data(), inputs[1], nFrames * sizeof(sample));
    sample *spec[2] = {mSpecInL.data(), mSpecInR.data()};
    if (vqt)
      mVQT.ProcessBlock(spec, nFrames, kCtrlTagPad, 2);
    else
      mSpectrum.ProcessBlock(spec, nFrames, kCtrlTagPad, 2);
  } else {
    std::memcpy(mSpecInL.data(), inputs[0], nFrames * sizeof(sample));
    sample *spec[1] = {mSpecInL.data()};
    if (vqt)
      mVQT.ProcessBlock(spec, nFrames, kCtrlTagPad, 1);
    else
      mSpectrum.ProcessBlock(spec, nFrames, kCtrlTagPad, 1);
  }

  // CPU 占用率(音频部分)结算: 处理耗时 / 块时长, 一阶平滑 (0.1 → 约 150ms 时间常数 @60Hz 推送);
  // UI 部分在 OnIdle 内合计
  {
    using namespace std::chrono;
    const double processMs = duration<double, std::milli>(steady_clock::now() - cpuT0).count();
    const double blockMs = (double)nFrames / std::max(GetSampleRate(), 1.0) * 1000.0;
    if (blockMs > 0.0)
      mCpuAudio += (processMs / blockMs - mCpuAudio) * 0.1;
  }
}

void ORMAnalyzer::OnReset() {
  mSpectrum.SetFFTSizeAndOverlap(CurrentFFTSize(), 4);
  // VQT 引擎配置: 采样率/γ/BPO 变更仅置位重建标记, band 表与历史缓冲由 UI 线程
  // (OnIdle 开头的 CheckRebuild) 惰性重建。不再在这里直接 SendSpectrumConfig:
  // 配置与 VQT band 频率表统一由 OnIdle 去重后下发, 保证总是基于已重建的 band 表。
  mVQT.SetSampleRate(GetSampleRate());
  mVQT.SetGamma(CurrentLfRes());
  mVQT.SetBpo(CurrentBpo());
}

void ORMAnalyzer::SendSpectrumConfig() {
  const double sr = GetSampleRate();
  const int fftSize = CurrentFFTSize();
  const float release = (float)GetParam(kRelease)->Value();
  const float range = (float)GetParam(kRange)->Value();
  const float attack = (float)GetParam(kAttack)->Value();
  SendControlMsgFromDelegate(kCtrlTagPad, SpectrumPad::kMsgTagSampleRate, sizeof(double), &sr);
  SendControlMsgFromDelegate(kCtrlTagPad, SpectrumPad::kMsgTagFFTSize, sizeof(int), &fftSize);
  SendControlMsgFromDelegate(kCtrlTagPad, SpectrumPad::kMsgTagRelease, sizeof(float), &release);
  SendControlMsgFromDelegate(kCtrlTagPad, SpectrumPad::kMsgTagRange, sizeof(float), &range);
  SendControlMsgFromDelegate(kCtrlTagPad, SpectrumPad::kMsgTagAttack, sizeof(float), &attack);
  const int mode = (int)GetParam(kMode)->Value();
  SendControlMsgFromDelegate(kCtrlTagPad, SpectrumPad::kMsgTagMode, sizeof(int), &mode);
  if (mode == kModeVQT)
    SendVQTBandFreqs();
}

void ORMAnalyzer::SendVQTBandFreqs() {
  const auto &freqs = mVQT.BandFreqs();
  if (freqs.empty())
    return;
  std::vector<float> buf(freqs.begin(), freqs.end());
  SendControlMsgFromDelegate(kCtrlTagPad, SpectrumPad::kMsgTagVQTBands,
                             (int)(buf.size() * sizeof(float)), buf.data());
}

void ORMAnalyzer::SendResetToPad() {
  const int dummy = 0;
  SendControlMsgFromDelegate(kCtrlTagPad, SpectrumPad::kMsgTagReset, sizeof(int), &dummy);
}

void ORMAnalyzer::UpdateResHeader() {
  if (!mResSlider)
    return;
  const bool vqt = GetParam(kMode)->Value() > 0.5;
  mResSlider->SetHeaderLabel(
      vqt ? orm::Tr(orm::kTxtLfRes, orm::UILang()) : orm::Tr(orm::kTxtRes, orm::UILang()));
}

void ORMAnalyzer::OnParamChange(int paramIdx, EParamSource source, int sampleOffset) {
  // 分析配置变化时在音频线程重配对应引擎 (重建会清空频谱历史, 显示短暂清零)
  if (paramIdx == kRes)
    mSpectrum.SetFFTSizeAndOverlap(CurrentFFTSize(), 4);
  else if (paramIdx == kLfRes) {
    // SetGamma 仅在 γ 档位实际变化 (40/20/10 之间跨越) 时重建并返回 true;
    // 拖动过程中参数值连续经过同档内的小数 (如 0.9->1.1 仍属同档区间) 不会重建,
    // 也就不会误发 Reset 导致频谱连续闪烁。
    if (mVQT.SetGamma(CurrentLfRes()))
      SendResetToPad();
  } else if (paramIdx == kBpo && GetParam(kMode)->Value() > 0.5) {
    if (mVQT.SetBpo(CurrentBpo()))
      SendResetToPad();
  }
  // kMode: DSP 路由在 ProcessBlock 按参数分支, 无需额外动作 (OnIdle 同步模式时也会 reset)
}

void ORMAnalyzer::OnParamChangeUI(int paramIdx, EParamSource source) {
  if (source == EParamSource::kUI)
    MaybePushGestureUndo();
}
#endif

void ORMAnalyzer::SetParamFromEditor(int idx, double value) {
  GetParam(idx)->Set(value);
  InformHostOfParamChange(idx, GetParam(idx)->GetNormalized());
}

void ORMAnalyzer::RefreshAfterEdit() {
#if IPLUG_EDITOR
  if (GetUI()) {
    SendCurrentParamValuesFromDelegate();
    GetUI()->SetAllControlsDirty();
  }
#endif
  MarkStateStable();
}

void ORMAnalyzer::OnIdle() {
  using namespace std::chrono;
  const auto wallNow = steady_clock::now();
  const double idleGapMs = duration<double, std::milli>(wallNow - mLastIdleTp).count();
  mLastIdleTp = wallNow;
  const auto workT0 = wallNow;

  // VQT 惰性重建: 音频线程若请求了重建 (γ/BPO/采样率变化), 在此先重建 band 表与历史缓冲,
  // 确保下面发送的 band 频率表与幅度数据都基于最新配置 (bands/freqs 由 UI 线程独占)。
  mVQT.CheckRebuild();

  // 分析模式变化: 切换滑块参数 (RES<->LF RES)、重发 pad 模式消息与 band 频率表,
  // 并清空 pad 平滑缓冲 (FFT bins 与 VQT bands 语义不同, 不能混叠)
  const int mode = (int)GetParam(kMode)->Value();
  if (mode != mSentMode) {
    mSentMode = mode;
    if (mResSlider) {
      mResSlider->SetParamIdx(mode == kModeVQT ? kLfRes : kRes);
      UpdateResHeader();
    }
    SendControlMsgFromDelegate(kCtrlTagPad, SpectrumPad::kMsgTagMode, sizeof(int), &mode);
    SendResetToPad();
    if (mode == kModeVQT)
      SendVQTBandFreqs();
  }

  // 仅在采样率/FFT 尺寸/释放/下限/上升时间/低频 γ/BPO 变化时重发
  // (如 UI 在 OnReset 之后才打开; γ/BPO 变化时同步刷新 VQT band 频率表)
  const double sr = GetSampleRate();
  const int fftSize = CurrentFFTSize();
  const double release = GetParam(kRelease)->Value();
  const double range = GetParam(kRange)->Value();
  const double attack = GetParam(kAttack)->Value();
  const double lfRes = GetParam(kLfRes)->Value();
  const double bpo = GetParam(kBpo)->Value();
  if (sr != mSentSampleRate || fftSize != mSentFFTSize || release != mSentRelease ||
      range != mSentRange || attack != mSentAttack || lfRes != mSentLfRes || bpo != mSentBpo) {
    mSentSampleRate = sr;
    mSentFFTSize = fftSize;
    mSentRelease = release;
    mSentRange = range;
    mSentAttack = attack;
    mSentLfRes = lfRes;
    mSentBpo = bpo;
    SendSpectrumConfig();
  }

  // 频谱数据转发: FFT/VQT 的频谱计算都在此完成 (PrepareDataForUI 内), 跑在 UI 线程
  mSpectrum.TransmitData(*this);
  mVQT.TransmitData(*this);

  // CPU 占用率(UI部分)结算: OnIdle 分析工作耗时 / 两次 OnIdle 墙钟间隔, 每 ~0.5s 滑窗;
  // 与音频线程占用 (mCpuAudio) 合计后推送 (单位: 一个核)
  {
    const double uiMs = duration<double, std::milli>(steady_clock::now() - workT0).count();
    mUiWorkMs += uiMs;
    mUiWinMs += idleGapMs;
    if (mUiWinMs >= 500.0) {
      mCpuUi = (mUiWinMs > 0.0) ? mUiWorkMs / mUiWinMs : 0.0;
      mUiWorkMs = 0.0;
      mUiWinMs = 0.0;
    }
    mCpuPct = mCpuAudio + mCpuUi;
  }
  // 实时推送 CPU 占用率 (每次 OnIdle, 约 60Hz; 消息开销可忽略)
  SendControlMsgFromDelegate(kCtrlTagCpu, CpuMeterControl::kMsgTagCpu, sizeof(double), &mCpuPct);

  const double now = duration<double>(steady_clock::now().time_since_epoch()).count();
  if (mGesturePending && now - mLastUIChangeTime > kGestureGapSec) {
    mStableSnapshot = Snapshot();
    mGesturePending = false;
  }
}

void ORMAnalyzer::OnUIClose() {
  mSpectrumPad = nullptr;
  mBpoSlider = nullptr;
  mResSlider = nullptr;
  mRangeSlider = nullptr;
  mAttackSlider = nullptr;
  mReleaseSlider = nullptr;
  mMixSlider = nullptr;
  mCpuMeter = nullptr;
  mModeToggle = nullptr;
  mSettingsPanel = nullptr;
  mTextBindings.clear();
  mTooltipBindings.clear();
  // 重开 UI 后控件是新的, 重置去重标记让下一次 OnIdle 重发完整配置与模式同步
  mSentSampleRate = 0.0;
  mSentFFTSize = 0;
  mSentRelease = -1.0;
  mSentMode = -1;
  mSentRange = -1.0;
  mSentAttack = -1.0;
  mSentLfRes = -1.0;
  mSentBpo = -1.0;
}

void ORMAnalyzer::OnParentWindowResize(int width, int height) {
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

bool ORMAnalyzer::ConstrainEditorResize(int &w, int &h) const {
  constexpr double kMinScale = DEFAULT_MIN_DRAW_SCALE;
  w = std::max(w, static_cast<int>(PLUG_WIDTH * kMinScale));

  const int wantH = static_cast<int>(std::lround(w * static_cast<double>(PLUG_HEIGHT) / PLUG_WIDTH));
  const bool ok = (h == wantH);
  h = wantH;
  return ok;
}

ParamSnapshot ORMAnalyzer::Snapshot() const {
  ParamSnapshot s;
  for (int i = 0; i < kNumParams; ++i)
    s[i] = GetParam(i)->Value();
  return s;
}

void ORMAnalyzer::ApplySnapshot(const ParamSnapshot &s) {
  for (int i = 0; i < kNumParams; ++i)
    SetParamFromEditor(i, s[i]);
  RefreshAfterEdit();
}

void ORMAnalyzer::PushUndo() {
  PushUndoSnapshot(Snapshot());
}

void ORMAnalyzer::PushUndoSnapshot(const ParamSnapshot &s) {
  if (!mUndoStack.empty() && mUndoStack.back() == s)
    return;
  mUndoStack.push_back(s);
  if (mUndoStack.size() > 100)
    mUndoStack.pop_front();
  mRedoStack.clear();
}

void ORMAnalyzer::MaybePushGestureUndo() {
  using namespace std::chrono;
  const double now = duration<double>(steady_clock::now().time_since_epoch()).count();
  if (now - mLastUIChangeTime > kGestureGapSec)
    PushUndoSnapshot(mStableSnapshot);
  mLastUIChangeTime = now;
  mGesturePending = true;
}

void ORMAnalyzer::MarkStateStable() {
  mStableSnapshot = Snapshot();
  mGesturePending = false;
}

void ORMAnalyzer::Undo() {
  if (mUndoStack.empty())
    return;
  mRedoStack.push_back(Snapshot());
  const ParamSnapshot s = mUndoStack.back();
  mUndoStack.pop_back();
  ApplySnapshot(s);
}

void ORMAnalyzer::Redo() {
  if (mRedoStack.empty())
    return;
  mUndoStack.push_back(Snapshot());
  const ParamSnapshot s = mRedoStack.back();
  mRedoStack.pop_back();
  ApplySnapshot(s);
}

void ORMAnalyzer::SaveFile() {
  if (!GetUI())
    return;
  mDialogFileName.Set("ORM Analyzer State");
  mDialogPath.Set("");
  GetUI()->PromptForFile(mDialogFileName, mDialogPath, EFileAction::Save, "orm",
                         [this](const WDL_String &fileName, const WDL_String &path) {
                           if (fileName.GetLength() == 0)
                             return;
                           std::string full = fileName.Get();
                           if (full.size() < 4 || full.compare(full.size() - 4, 4, ".orm") != 0)
                             full += ".orm";
                           std::string err;
                           WriteStateFileTo(full, err);
                           if (!err.empty() && GetUI())
                             GetUI()->ShowMessageBox(err.c_str(), "Save Failed", kMB_OK);
                         });
}

void ORMAnalyzer::LoadFile() {
  if (!GetUI())
    return;
  mDialogFileName.Set("");
  GetUI()->PromptForFile(mDialogFileName, mDialogPath, EFileAction::Open, "orm",
                         [this](const WDL_String &fileName, const WDL_String &path) {
                           if (fileName.GetLength() == 0)
                             return;
                           std::string err;
                           ReadStateFileFrom(fileName.Get(), err);
                           if (!err.empty() && GetUI())
                             GetUI()->ShowMessageBox(err.c_str(), "Load Failed", kMB_OK);
                         });
}

void ORMAnalyzer::WriteStateFileTo(const std::string &path, std::string &err) {
  StateFileData data;
  const ParamSnapshot cur = Snapshot();
  data.values.assign(cur.begin(), cur.end());
  if (WriteStateFile(path, data, err))
    err.clear();
}

void ORMAnalyzer::ReadStateFileFrom(const std::string &path, std::string &err) {
  StateFileData data;
  if (!ReadStateFile(path, data, err))
    return;

  // 兼容旧文件: 参数数少于当前版本时, 缺失项用默认值 (新参数自动取默认档)
  if ((int)data.values.size() > kNumParams) {
    err = "Parameter count mismatch (expected " + std::to_string(kNumParams) + ")";
    return;
  }

  ParamSnapshot s = mDefaultSnapshot;
  for (int i = 0; i < (int)data.values.size(); ++i)
    s[i] = data.values[i];

  PushUndo();
  ApplySnapshot(s);
  err.clear();
}

void ORMAnalyzer::ApplyLanguage() {
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

void ORMAnalyzer::ApplyTooltips() {
  for (auto &binding : mTooltipBindings)
    if (binding.first)
      binding.first->SetTooltip(orm::Tr(binding.second, orm::UILang()));
}

void ORMAnalyzer::ApplyTheme() {
  ThemeMode() = mThemeMode;
  RefreshThemeColors();
}

void ORMAnalyzer::RefreshThemeColors() {
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

void ORMAnalyzer::ToggleSettingsPanel() {
  if (mSettingsPanel)
    mSettingsPanel->SetVisible(mSettingsPanel->IsHidden());
}

void ORMAnalyzer::SaveSettingsToDisk() {
  SettingsData s;
  s.lang = orm::UILang();
  s.hue = ThemeHue();
  s.satMax = ThemeSatMax();
  s.themeMode = mThemeMode;
  SaveSettings(s);
}
