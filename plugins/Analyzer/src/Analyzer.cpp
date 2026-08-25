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
#include "controls/ChannelLegendControl.h"
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
  // 读取全局 UI 偏好设置 (语言与主题)
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
  // 初始化参数（默认值、范围与步长）
  GetParam(kMix)->InitDouble("Mix", 1., 0., 1., 0.01, "");
  GetParam(kRelease)->InitDouble("Release", 0.2, 0.05, 0.5, 0.01, "s");
  GetParam(kRange)->InitDouble("Range", 90, 80, 120, 10, "");
  GetParam(kAttack)->InitDouble("Attack", 0.05, 0.001, 0.1, 0.001, "s");
  GetParam(kRes)->InitInt("Res", kNumResOptions - 1, 0, kNumResOptions - 1, "");
  GetParam(kLfRes)->InitInt("LfRes", 0, 0, kNumLfResOptions - 1, "");
  GetParam(kBpo)->InitInt("Bpo", kNumBpoOptions - 1, 0, kNumBpoOptions - 1, "");
  GetParam(kMode)->InitInt("Mode", kModeFFT, 0, 1, "");
  GetParam(kChannelMode)->InitInt("ChanMode", kChanModeLR, 0, kNumChanModes - 1, "");
  GetParam(kMergeAlgo)->InitInt("MergeAlgo", kMergeAlgoPWR, 0, kNumMergeAlgos - 1, "");

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

    // 三通道色块图例 (L / R / M, 颜色跟随主题), 与频谱图形区左缘对齐
    pGraphics->AttachControl(new ChannelLegendControl(IRECT(20, 32, 668, 54)));

    // 主频谱绘制区域
    mSpectrumPad = new SpectrumPad(IRECT(20, 58, 668, 328));
    pGraphics->AttachControl(mSpectrumPad, kCtrlTagPad);

    // CPU 占用率显示
    mCpuMeter = new CpuMeterControl(IRECT(kCol1X, 30, kPanelR, 60));
    pGraphics->AttachControl(mCpuMeter, kCtrlTagCpu);

    // 声道显示模式循环切换按钮 (L/R -> MERGE)
    mChanModeBtn = new FlatCycleButton(IRECT(kCol1X, 85, kCol1X + 78, 115), kChannelMode,
                                       {"L/R", "MERGE"}, btnStyle);
    pGraphics->AttachControl(mChanModeBtn);
    bindTip(mChanModeBtn, orm::kTxtTipChanMode);

    // 合并算法切换按钮 (PWR / SUM)
    IVStyle algoStyle = btnStyle;
    algoStyle.showLabel = false;
    algoStyle.showValue = false;
    mMergeAlgoToggle = new FlatToggleControl(IRECT(kCol1X + 78, 85, kPanelR, 115), kMergeAlgo, " ", algoStyle,
                                            "PWR", "SUM");
    pGraphics->AttachControl(mMergeAlgoToggle);
    bindTip(mMergeAlgoToggle, orm::kTxtTipMergeAlgo);

    // BPO 滑块（每八度频带数，仅 VQT 模式生效）
    mBpoSlider =
        new ORMSlider(IRECT(kCol1X, 140, kPanelR, 182), kBpo, "BPO", style, EDirection::Horizontal);
    pGraphics->AttachControl(mBpoSlider);
    bindText(orm::kTxtBpo, [this](const char *s) { mBpoSlider->SetHeaderLabel(s); });
    bindTip(mBpoSlider, orm::kTxtTipBpo);

    // 分析引擎切换按钮 (FFT / VQT)
    IVStyle toggleStyle = btnStyle;
    toggleStyle.showLabel = false;
    toggleStyle.showValue = false;
    mModeToggle = new FlatToggleControl(IRECT(kCol1X, 192, kPanelR, 222), kMode, " ", toggleStyle, "FFT",
                                        "VQT");
    pGraphics->AttachControl(mModeToggle);
    bindTip(mModeToggle, orm::kTxtTipMode);

    // 分辨率滑块 (FFT 模式下为 FFT 尺寸，VQT 模式下为低频分辨率 γ)
    mResSlider =
        new ORMSlider(IRECT(kCol1X, 234, kPanelR, 276), kRes, "RES", style, EDirection::Horizontal);
    pGraphics->AttachControl(mResSlider);
    bindText(orm::kTxtRes, [this](const char *) { UpdateResHeader(); });
    bindText(orm::kTxtLfRes, [this](const char *) { UpdateResHeader(); });
    bindTip(mResSlider, orm::kTxtTipRes);

    // 动态范围滑块 (dBFS 下限)
    mRangeSlider =
        new ORMSlider(IRECT(kCol1X, 282, kPanelR, 324), kRange, "RANGE", style, EDirection::Horizontal);
    pGraphics->AttachControl(mRangeSlider);
    bindText(orm::kTxtRange, [this](const char *s) { mRangeSlider->SetHeaderLabel(s); });
    bindTip(mRangeSlider, orm::kTxtTipRange);

    // 上升响应时间滑块 (s)
    mAttackSlider =
        new ORMSlider(IRECT(kCol1X, 328, kPanelR, 370), kAttack, "ATTACK", style, EDirection::Horizontal);
    pGraphics->AttachControl(mAttackSlider);
    bindText(orm::kTxtAttack, [this](const char *s) { mAttackSlider->SetHeaderLabel(s); });
    bindTip(mAttackSlider, orm::kTxtTipAttack);

    // 释放衰减时间滑块 (s)
    mReleaseSlider =
        new ORMSlider(IRECT(kCol1X, 374, kPanelR, 416), kRelease, "RELEASE", style, EDirection::Horizontal);
    pGraphics->AttachControl(mReleaseSlider);
    bindText(orm::kTxtRelease, [this](const char *s) { mReleaseSlider->SetHeaderLabel(s); });
    bindTip(mReleaseSlider, orm::kTxtTipRelease);

    // MIX 滑块
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

    // 底部标题栏：ORM 标识、设置齿轮与版本号
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
  const auto cpuT0 = std::chrono::steady_clock::now();
  nFrames = std::min(nFrames, kMaxBlock);

  const int nOuts = NOutChansConnected();
  const int nIns = NInChansConnected();

  // 音频直通（分析器不改变音频信号）
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

  // 采集输入数据到当前分析引擎 (FFT 或 VQT)
  const bool vqt = GetParam(kMode)->Value() > 0.5;
  if (nIns >= 2) {
    std::memcpy(mSpecInL.data(), inputs[0], nFrames * sizeof(sample));
    std::memcpy(mSpecInR.data(), inputs[1], nFrames * sizeof(sample));
    // 计算时域单声道求和 (除以 sqrt(2)，使同相双声道达到 0 dBFS，单声道为 -3 dBFS，反相抵消为 0)
    for (int s = 0; s < nFrames; ++s)
      mSpecInM[s] = (mSpecInL[s] + mSpecInR[s]) * 0.7071067811865475;
    sample *spec[3] = {mSpecInL.data(), mSpecInR.data(), mSpecInM.data()};
    if (vqt)
      mVQT.ProcessBlock(spec, nFrames, kCtrlTagPad, 3);
    else
      mSpectrum.ProcessBlock(spec, nFrames, kCtrlTagPad, 3);
  } else {
    std::memcpy(mSpecInL.data(), inputs[0], nFrames * sizeof(sample));
    std::memcpy(mSpecInR.data(), inputs[0], nFrames * sizeof(sample));
    for (int s = 0; s < nFrames; ++s)
      mSpecInM[s] = mSpecInL[s] * 0.7071067811865475;
    sample *spec[3] = {mSpecInL.data(), mSpecInR.data(), mSpecInM.data()};
    if (vqt)
      mVQT.ProcessBlock(spec, nFrames, kCtrlTagPad, 3);
    else
      mSpectrum.ProcessBlock(spec, nFrames, kCtrlTagPad, 3);
  }

  // 时域峰值计算 (供 Gain 条显示), 基于原始输入样本, 与频谱引擎独立
  ComputeGainPeaks(nFrames);

  // 统计音频线程耗时（一阶平滑）
  {
    using namespace std::chrono;
    const double processMs = duration<double, std::milli>(steady_clock::now() - cpuT0).count();
    const double blockMs = (double)nFrames / std::max(GetSampleRate(), 1.0) * 1000.0;
    if (blockMs > 0.0)
      mCpuAudio += (processMs / blockMs - mCpuAudio) * 0.1;
  }
}

// 时域峰值: 每 block 计算 L/R 样本绝对值的最大值, 按攻击/释放时间常数平滑后
// 存到原子成员, 供 UI 线程 OnIdle 读取并转发给 Gain 条显示。
void ORMAnalyzer::ComputeGainPeaks(int nFrames) {
  const float sr = (float)GetSampleRate();
  if (sr <= 0.f || nFrames <= 0)
    return;

  float rawL = 0.f, rawR = 0.f;
  for (int s = 0; s < nFrames; ++s) {
    rawL = std::max(rawL, std::fabs(mSpecInL[s]));
    rawR = std::max(rawR, std::fabs(mSpecInR[s]));
  }

  const float period = (float)nFrames / sr;
  const float aCoef = std::exp(-period / std::max((float)GetParam(kAttack)->Value(), 0.001f));
  const float rCoef = std::exp(-period / std::max((float)GetParam(kRelease)->Value(), 0.01f));

  float prevL = mPeakL.load(std::memory_order_relaxed);
  float prevR = mPeakR.load(std::memory_order_relaxed);
  const float coefL = (rawL > prevL) ? aCoef : rCoef;
  const float coefR = (rawR > prevR) ? aCoef : rCoef;
  mPeakL.store(coefL * prevL + (1.f - coefL) * rawL, std::memory_order_relaxed);
  mPeakR.store(coefR * prevR + (1.f - coefR) * rawR, std::memory_order_relaxed);
}

void ORMAnalyzer::OnReset() {
  mSpectrum.SetFFTSizeAndOverlap(CurrentFFTSize(), 4);
  mVQT.SetSampleRate(GetSampleRate());
  mVQT.SetGamma(CurrentLfRes());
  mVQT.SetBpo(CurrentBpo());
  mPeakL.store(0.f, std::memory_order_relaxed);
  mPeakR.store(0.f, std::memory_order_relaxed);
}

void ORMAnalyzer::SendSpectrumConfig() {
  const double sr = GetSampleRate();
  const int fftSize = CurrentFFTSize();
  const float release = (float)GetParam(kRelease)->Value();
  const float range = (float)GetParam(kRange)->Value();
  const float attack = (float)GetParam(kAttack)->Value();
  const int chanMode = (int)GetParam(kChannelMode)->Value();
  const int mergeAlgo = (int)GetParam(kMergeAlgo)->Value();
  SendControlMsgFromDelegate(kCtrlTagPad, SpectrumPad::kMsgTagSampleRate, sizeof(double), &sr);
  SendControlMsgFromDelegate(kCtrlTagPad, SpectrumPad::kMsgTagFFTSize, sizeof(int), &fftSize);
  SendControlMsgFromDelegate(kCtrlTagPad, SpectrumPad::kMsgTagRelease, sizeof(float), &release);
  SendControlMsgFromDelegate(kCtrlTagPad, SpectrumPad::kMsgTagRange, sizeof(float), &range);
  SendControlMsgFromDelegate(kCtrlTagPad, SpectrumPad::kMsgTagAttack, sizeof(float), &attack);
  SendControlMsgFromDelegate(kCtrlTagPad, SpectrumPad::kMsgTagChanMode, sizeof(int), &chanMode);
  SendControlMsgFromDelegate(kCtrlTagPad, SpectrumPad::kMsgTagMergeAlgo, sizeof(int), &mergeAlgo);
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
  // 参数变化时更新分析引擎配置
  if (paramIdx == kRes)
    mSpectrum.SetFFTSizeAndOverlap(CurrentFFTSize(), 4);
  else if (paramIdx == kLfRes) {
    if (mVQT.SetGamma(CurrentLfRes()))
      SendResetToPad();
  } else if (paramIdx == kBpo && GetParam(kMode)->Value() > 0.5) {
    if (mVQT.SetBpo(CurrentBpo()))
      SendResetToPad();
  }
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

  // 若 VQT 参数发生变动，按需重建频带表与多速率金字塔
  mVQT.CheckRebuild();

  // 模式切换处理 (FFT <-> VQT)
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

  // 声道显示模式与合并算法变动检测
  const int chanMode = (int)GetParam(kChannelMode)->Value();
  const int mergeAlgo = (int)GetParam(kMergeAlgo)->Value();
  if (chanMode != mSentChanMode) {
    mSentChanMode = chanMode;
    SendControlMsgFromDelegate(kCtrlTagPad, SpectrumPad::kMsgTagChanMode, sizeof(int), &chanMode);
  }
  if (mergeAlgo != mSentMergeAlgo) {
    mSentMergeAlgo = mergeAlgo;
    SendControlMsgFromDelegate(kCtrlTagPad, SpectrumPad::kMsgTagMergeAlgo, sizeof(int), &mergeAlgo);
  }

  // 检查并下发有变动的频谱配置
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

  // 执行频谱分析计算并将数据发送给 UI 控件
  mSpectrum.TransmitData(*this);
  mVQT.TransmitData(*this);

  // 转发时域峰值给 Gain 条 (float[2] = {L, R})
  {
    const float peaks[2] = {mPeakL.load(std::memory_order_relaxed),
                            mPeakR.load(std::memory_order_relaxed)};
    SendControlMsgFromDelegate(kCtrlTagPad, SpectrumPad::kMsgTagGainPeak, sizeof(peaks), peaks);
  }

  // 统计 UI 线程耗时并计算综合 CPU 占用率
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
  mChanModeBtn = nullptr;
  mMergeAlgoToggle = nullptr;
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
  mSentChanMode = -1;
  mSentMergeAlgo = -1;
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
