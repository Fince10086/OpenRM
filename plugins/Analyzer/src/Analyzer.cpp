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
#include "controls/StereoFieldControl.h"
#include "controls/OscilloscopeControl.h"
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
#include <mach/mach.h>
#elif defined(OS_WIN)
#include <windows.h>
#elif defined(__linux__)
#include <ctime>
#endif

// 线程 CPU 时间（纳秒）：只计实际执行时间，排除被抢占/调度延迟，避免系统负载导致读数虚高
static uint64_t ThreadCpuNs() {
#if defined(OS_MAC)
  thread_basic_info_data_t tbi;
  mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;
  if (thread_info(mach_thread_self(), THREAD_BASIC_INFO, (thread_info_t)&tbi, &count) == KERN_SUCCESS) {
    const auto us = [](time_value_t t) { return (uint64_t)t.seconds * 1000000ULL + (uint64_t)t.microseconds; };
    return (us(tbi.user_time) + us(tbi.system_time)) * 1000ULL;
  }
  return 0;
#elif defined(OS_WIN)
  FILETIME create, exit, kernel, user;
  if (GetThreadTimes(GetCurrentThread(), &create, &exit, &kernel, &user)) {
    const auto ft = [](FILETIME f) { return (((uint64_t)f.dwHighDateTime << 32) | f.dwLowDateTime) * 100ULL; };
    return ft(kernel) + ft(user);
  }
  return 0;
#elif defined(__linux__)
  timespec ts;
  if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) == 0)
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
  return 0;
#else
  return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
#endif
}

// 手势撤销时间窗：两次 UI 改动间隔超过该值时，下一次改动前推一次撤销快照
static constexpr double kGestureGapSec = 0.4;

// hop 恒 1024，四引擎帧进给同格，冻结回放帧格对齐无需按档位分支
static inline int FFTOverlapForSize(int fftSize) {
  return std::max(1, fftSize / 1024);
}

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

// 速度档 × 释放模式 -> 释放时间常数（s）：LOG 0.2~4s，LIN = LOG×2
static double SpeedReleaseSec(int speedIdx, int releaseMode) {
  const int s = std::clamp(speedIdx, 0, kNumSpeedOptions - 1);
  return (releaseMode == 1) ? kSpeedReleaseLin[s] : kSpeedReleaseLog[s];
}

ORMAnalyzer::ORMAnalyzer(const InstanceInfo &info) : Plugin(info, MakeConfig(kNumParams, 1)) {
  // 读取全局 UI 偏好
  SettingsData s;
  {
    if (LoadSettings(s)) {
      if (s.lang >= 0 && s.lang < orm::kNumLanguages)
        orm::UILang() = s.lang;
      ThemeHue() = s.hue;
      ThemeSatMax() = s.satMax;
      mThemeMode = s.themeMode;
      ThemeMode() = s.themeMode;
#if ORM_ENABLE_TEST_GEN
      if (s.genType >= 0 && s.genType < orm::kNumGenSignals)
        mGenType.store(s.genType, std::memory_order_relaxed);
      mGenFreq.store((float)std::clamp(s.genFreq, 1.0, 20000.0), std::memory_order_relaxed);
      mGenLevel.store((float)std::clamp(s.genLevel, -120.0, 0.0), std::memory_order_relaxed);
      mGenHold.store(s.genHold != 0, std::memory_order_relaxed);
      mGenToOutput.store(s.genToOutput != 0, std::memory_order_relaxed);
#endif
    }
  }
  // 初始化参数
  GetParam(kReleaseMode)->InitInt("ReleaseMode", 0, 0, 1, "");
  GetParam(kSpeed)->InitInt("Speed", kSpeedMAX, 0, kNumSpeedOptions - 1, "");
  GetParam(kRange)->InitInt("Range", 0, 0, 2, "");
  GetParam(kRes)->InitInt("Res", 1, 0, kNumResOptions - 1, "");
  GetParam(kLfRes)->InitInt("LfRes", 2, 0, kNumPbtLfResOptions - 1, "");
  GetParam(kMode)->InitInt("Mode", kModeFFT, 0, kNumModes - 1, "");
  GetParam(kChannelMode)->InitInt("ChanMode", kChanModePWR, 0, kNumChanModes - 1, "");
  GetParam(kLevelMode)->InitInt("LevelMode", kLevelModeDBTP, 0, kNumLevelModes - 1, "");
  GetParam(kLevelHold)->InitInt("LevelHold", 1, 0, kNumHoldTimeOptions - 1, "");
  GetParam(kLevelHoldOn)->InitBool("LevelHoldOn", false);
  if (s.holdOn >= 0)
    GetParam(kLevelHoldOn)->Set(s.holdOn > 0 ? 1.0 : 0.0);
  GetParam(kFreeze)->InitBool("Freeze", false);
  GetParam(kVQTGamma)->InitInt("VQTGamma", 2, 0, kNumVQTGammaOptions - 1, "");
  GetParam(kSlopeFFT)->InitInt("SlopeFFT", 1, 0, kNumSlopeOptions - 1, "");
  GetParam(kSlopeVQT)->InitInt("SlopeVQT", 1, 0, kNumSlopeOptions - 1, "");
  GetParam(kSlopePBT)->InitInt("SlopePBT", 1, 0, kNumSlopeOptions - 1, "");
  GetParam(kSlopeRTA)->InitInt("SlopeRTA", 1, 0, kNumSlopeOptions - 1, "");
  // STFT 与 VQT 窗函数档位各自独立
  GetParam(kFFTWindow)->InitInt("WindowFFT", kFFTWindowHann, 0, kNumFFTWindows - 1, "");
  GetParam(kWindowVQT)->InitInt("WindowVQT", kFFTWindowClean, 0, kNumFFTWindows - 1, "");
  GetParam(kRtaOctave)->InitInt("RtaOctave", 0, 0, kNumRtaOctaveOptions - 1, "");
  GetParam(kLoudPreset)->InitInt("LoudPreset", 1, 0, kNumLoudPresets - 1, "");
  GetParam(kLoudScale)->InitInt("LoudScale", 0, 0, kNumLoudScaleOptions - 1, "");
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
    // Windows 系统字体 fallback 链，需含完整 CJK
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

    // 右栏布局常量
    constexpr float kCol1X = 784.f;
    constexpr float kBtnW = 76.f;
    constexpr float kBtnGap = 4.f;
    constexpr float kCol2X = kCol1X + kBtnW + kBtnGap;
    constexpr float kPanelR = kCol2X + kBtnW;

    // 顶行按钮
    constexpr float kTopBtnW = 62.f;
    constexpr float kTopBtnH = 26.f;
    constexpr float kTopBtnY = 22.f;
    constexpr float kTopBtnGap = 6.f;

    // 声道模式（PWR / L/R / SUM）
    mChanModeBtn = new FlatCycleButton(IRECT(20.f, kTopBtnY, 20.f + kTopBtnW, kTopBtnY + kTopBtnH),
                                       kChannelMode, {"PWR", "L/R", "SUM"}, btnStyle, true);
    pGraphics->AttachControl(mChanModeBtn);
    bindTip(mChanModeBtn, orm::kTxtTipChanMode);

    // 分析引擎（STFT / VQT / PBT / RTA）
    constexpr float kModeX = 20.f + kTopBtnW + kTopBtnGap;
    mModeBtn = new FlatCycleButton(IRECT(kModeX, kTopBtnY, kModeX + kTopBtnW, kTopBtnY + kTopBtnH),
                                   kMode, {"STFT", "VQT", "PBT", "RTA"}, btnStyle);
    pGraphics->AttachControl(mModeBtn);
    bindTip(mModeBtn, orm::kTxtTipMode);

    // 分辨率槽位（L/M/H），各模式按钮互斥可见
    constexpr float kResBtnW = 31.f;
    constexpr float kResX = kModeX + kTopBtnW;
    mResBtn = new FlatCycleButton(IRECT(kResX, kTopBtnY, kResX + kResBtnW, kTopBtnY + kTopBtnH), kRes,
                                  {"L", "M", "H"}, btnStyle);
    pGraphics->AttachControl(mResBtn);
    bindTip(mResBtn, orm::kTxtTipRes);

    mPbtLfResBtn = new FlatCycleButton(IRECT(kResX, kTopBtnY, kResX + kResBtnW, kTopBtnY + kTopBtnH), kLfRes,
                                       {"L", "M", "H"}, btnStyle);
    pGraphics->AttachControl(mPbtLfResBtn);
    bindTip(mPbtLfResBtn, orm::kTxtTipLfRes);
    mPbtLfResBtn->Hide(true);

    mGammaBtn = new FlatCycleButton(IRECT(kResX, kTopBtnY, kResX + kResBtnW, kTopBtnY + kTopBtnH),
                                    kVQTGamma, {"L", "M", "H"}, btnStyle);
    pGraphics->AttachControl(mGammaBtn);
    bindTip(mGammaBtn, orm::kTxtTipVQTGamma);
    mGammaBtn->Hide(true);

    mRtaOctBtn = new FlatCycleButton(IRECT(kResX, kTopBtnY, kResX + kResBtnW, kTopBtnY + kTopBtnH),
                                     kRtaOctave, {"L", "M", "H"}, btnStyle);
    pGraphics->AttachControl(mRtaOctBtn);
    bindTip(mRtaOctBtn, orm::kTxtTipRtaOctave);
    mRtaOctBtn->Hide(true);

    // 窗函数（SHARP/CLEAN），STFT 与 VQT 按模式改绑参数
    constexpr float kWinX = kResX + kResBtnW + kTopBtnGap;
    const int initMode = (int)GetParam(kMode)->Value();
    mWindowBtn = new FlatCycleButton(IRECT(kWinX, kTopBtnY, kWinX + kTopBtnW, kTopBtnY + kTopBtnH),
                                     initMode == kModeFFT ? kFFTWindow : kWindowVQT, {"SHARP", "CLEAN"},
                                     btnStyle);
    pGraphics->AttachControl(mWindowBtn);
    bindTip(mWindowBtn, orm::kTxtTipWindow);
    mWindowBtn->Hide(initMode != kModeFFT && initMode != kModeVQT);

    // 释放模式（LOG / LIN）
    constexpr float kLogX = kWinX + kTopBtnW + kTopBtnGap;
    mReleaseModeBtn =
        new FlatCycleButton(IRECT(kLogX, kTopBtnY, kLogX + kTopBtnW, kTopBtnY + kTopBtnH), kReleaseMode,
                            {"LOG", "LIN"}, btnStyle);
    pGraphics->AttachControl(mReleaseModeBtn);
    bindTip(mReleaseModeBtn, orm::kTxtTipReleaseMode);

    // 频谱斜率（标签带单位，加宽）
    constexpr float kSlopeTopX = kLogX + kTopBtnW + kTopBtnGap;
    constexpr float kSlopeTopW = 116.f;
    mSlopeBtn = new FlatCycleButton(
        IRECT(kSlopeTopX, kTopBtnY, kSlopeTopX + kSlopeTopW, kTopBtnY + kTopBtnH), SlopeParamForMode(initMode),
        initMode == kModeFFT ? std::initializer_list<const char *>{"0 dB/Oct", "3 dB/Oct", "4.5 dB/Oct"}
                             : std::initializer_list<const char *>{"-3 dB/Oct", "0 dB/Oct", "1.5 dB/Oct"},
        btnStyle);
    pGraphics->AttachControl(mSlopeBtn);
    bindTip(mSlopeBtn, orm::kTxtTipSlope);

    // 响应速度（MIN..MAX）
    constexpr float kSpeedX = kSlopeTopX + kSlopeTopW + kTopBtnGap;
    mSpeedBtn = new FlatCycleButton(IRECT(kSpeedX, kTopBtnY, kSpeedX + kTopBtnW, kTopBtnY + kTopBtnH),
                                    kSpeed, {"MIN", "SLOW", "MED", "FAST", "MAX"}, btnStyle);
    pGraphics->AttachControl(mSpeedBtn);
    bindTip(mSpeedBtn, orm::kTxtTipSpeed);

    // 主频谱区
    mSpectrumPad = new SpectrumPad(IRECT(20, 58, 784, 328));
    pGraphics->AttachControl(mSpectrumPad, kCtrlTagPad);

    // 电平条点击交互
    mSpectrumPad->mMeterClickHandler = [this](SpectrumPad::EMeterClick c) {
      switch (c) {
      case SpectrumPad::kClickResetPersist:
        mLevelResetPersistFlag.store(true, std::memory_order_relaxed);
        break;
      case SpectrumPad::kClickResetMeterHold:
        mLevelResetMeterHoldFlag.store(true, std::memory_order_relaxed);
        break;
      case SpectrumPad::kClickResetOver:
        mLevelResetOverFlag.store(true, std::memory_order_relaxed);
        break;
      case SpectrumPad::kClickLoudScale:
      case SpectrumPad::kClickLoudPreset: {
        const int param = (c == SpectrumPad::kClickLoudScale) ? kLoudScale : kLoudPreset;
        const int num = (c == SpectrumPad::kClickLoudScale) ? kNumLoudScaleOptions : kNumLoudPresets;
        MaybePushGestureUndo();
        const int cur = (int)std::clamp(std::lround(GetParam(param)->Value()), 0L, (long)num - 1);
        const int next = (cur + 1) % num;
        // 必须传 plain 档位值，否则多档参数循环会被吞
        SetParamFromEditor(param, (double)next);
        break;
      }
      }
    };

    // 下半区布局：频谱下新增一排示波器按钮（同频谱顶行风格），
    // 下方左侧立体声像（Stereo Field），右侧时域示波器（Oscilloscope）
    constexpr float kBotBtnY = 336.f;  // 频谱下沿 328 + 8
    constexpr float kBotBtnH = 26.f;
    constexpr float kBotBtnGap = 6.f;
    constexpr float kBottomTop = kBotBtnY + kBotBtnH + 10.f; // 372
    constexpr float kBottomB = kBottomTop + 268.f;           // 640：面板整体下移，原高度不变
    constexpr float kBottomMidX = 398.f;

    float scopeBtnX = 20.f;
    auto scopeBtnRect = [&](float w) {
      const IRECT r(scopeBtnX, kBotBtnY, scopeBtnX + w, kBotBtnY + kBotBtnH);
      scopeBtnX += w + kBotBtnGap;
      return r;
    };

    // 触发模式（EDGE / AUTOCORR / FREQ）
    mScopeTrigBtn = new FlatCycleButton(scopeBtnRect(104.f), kNoParameter, {"EDGE", "AUTOCORR", "FREQ"}, btnStyle);
    mScopeTrigBtn->SetCycleHandler([this](int v) {
      if (mOscilloscopeCtrl)
        mOscilloscopeCtrl->SetTrigSource(v);
      if (mScopeSrcBtn)
        mScopeSrcBtn->Hide(v != OscilloscopeControl::kTrigFrequency); // 刷新频率档仅 FREQ 模式需要
    });
    pGraphics->AttachControl(mScopeTrigBtn);
    bindTip(mScopeTrigBtn, orm::kTxtTipScopeTrig);

    // 刷新频率档（仅 FREQ 模式显示）；触发参考通道随 L/R/M 显示掩码自动决定，无独立 SRC 按钮
    mScopeSrcBtn = new FlatCycleButton(scopeBtnRect(64.f), kNoParameter,
                                       {"10Hz", "20Hz", "30Hz", "60Hz", "120Hz"}, btnStyle);
    mScopeSrcBtn->SetCycleHandler([this](int v) {
      if (mOscilloscopeCtrl)
        mOscilloscopeCtrl->SetFreqPreset(v);
    });
    mScopeSrcBtn->Hide(true);
    pGraphics->AttachControl(mScopeSrcBtn);
    bindTip(mScopeSrcBtn, orm::kTxtTipScopeSrc);

    // 声道选择：L / R / M 独立开关，弹起灰色、按下显示对应通道色
    IColor ccL, ccR, ccM;
    GetChannelColors(ccL, ccR, ccM);
    mScopeBtnL = new FlatColorToggleControl(scopeBtnRect(34.f), "L", btnStyle);
    mScopeBtnL->SetColor(ccL);
    mScopeBtnL->SetValue(1.0); // 默认 L+R 叠加
    mScopeBtnL->SetActionFunction([this](IControl *) { SyncScopeChanMask(); });
    pGraphics->AttachControl(mScopeBtnL);
    bindTip(mScopeBtnL, orm::kTxtTipScopeChan);

    mScopeBtnR = new FlatColorToggleControl(scopeBtnRect(34.f), "R", btnStyle);
    mScopeBtnR->SetColor(ccR);
    mScopeBtnR->SetValue(1.0);
    mScopeBtnR->SetActionFunction([this](IControl *) { SyncScopeChanMask(); });
    pGraphics->AttachControl(mScopeBtnR);
    bindTip(mScopeBtnR, orm::kTxtTipScopeChan);

    mScopeBtnM = new FlatColorToggleControl(scopeBtnRect(34.f), "M", btnStyle);
    mScopeBtnM->SetColor(ccM);
    mScopeBtnM->SetActionFunction([this](IControl *) { SyncScopeChanMask(); });
    pGraphics->AttachControl(mScopeBtnM);
    bindTip(mScopeBtnM, orm::kTxtTipScopeChan);

    // 时基（1ms..2s）
    mScopeTimeBtn = new FlatCycleButton(
        scopeBtnRect(64.f), kNoParameter,
        {"1ms", "2ms", "5ms", "10ms", "20ms", "50ms", "100ms", "500ms", "1s", "2s"}, btnStyle);
    mScopeTimeBtn->SetCycleHandler([this](int v) {
      if (mOscilloscopeCtrl)
        mOscilloscopeCtrl->SetTimebase(v);
    });
    pGraphics->AttachControl(mScopeTimeBtn);
    bindTip(mScopeTimeBtn, orm::kTxtTipScopeTime);

    // 垂直缩放（1x..8x）
    mScopeZoomBtn = new FlatCycleButton(scopeBtnRect(40.f), kNoParameter, {"1x", "2x", "4x", "8x"}, btnStyle);
    mScopeZoomBtn->SetCycleHandler([this](int v) {
      if (mOscilloscopeCtrl)
        mOscilloscopeCtrl->SetZoom(v);
    });
    pGraphics->AttachControl(mScopeZoomBtn);
    bindTip(mScopeZoomBtn, orm::kTxtTipScopeZoom);

    mScopeCtrl = new StereoFieldControl(IRECT(20.f, kBottomTop, kBottomMidX - 4.f, kBottomB));
    pGraphics->AttachControl(mScopeCtrl, kCtrlTagScope);

    // 右缘 780（原 784）：与右栏按钮留出间隙，不再紧贴
    mOscilloscopeCtrl = new OscilloscopeControl(IRECT(kBottomMidX + 2.f, kBottomTop, 780.f, kBottomB));
    pGraphics->AttachControl(mOscilloscopeCtrl, kCtrlTagOscilloscope);

    // 动态范围按钮（频谱底部右缘）
    constexpr float kRangeBtnW = 38.f;
    constexpr float kRangeBtnH = 19.f;
    const float plotR = 784.f - kMeterStripW;
    const float plotB = 328.f;
    mRangeBtn = new FlatCycleButton(IRECT(plotR - kRangeBtnW, plotB - kRangeBtnH, plotR, plotB), kRange,
                                    {"-80", "-100", "-120"}, btnStyle);
    pGraphics->AttachControl(mRangeBtn);
    mRangeBtn->SetScaleLabelStyle(true);
    bindTip(mRangeBtn, orm::kTxtTipRange);

    // 右栏控件组
    constexpr float kRowGap = kBtnGap;
    constexpr float kRowH = 26.f;
    constexpr float kIconRowY = 610.f - kRowH;
    constexpr float kHoldRowY = kIconRowY - kRowGap - kRowH;
    constexpr float kFreezeRowY = kHoldRowY - kRowGap - kRowH;
    constexpr float kCpuY = kFreezeRowY - kRowGap - kRowH;
    mCpuMeter = new CpuMeterControl(IRECT(kCol1X, kCpuY, kPanelR, kCpuY + kRowH));
    pGraphics->AttachControl(mCpuMeter, kCtrlTagCpu);

    // 撤销/重做/保存/读取图标行
    constexpr float kIconBtnW = (kBtnW - kBtnGap) / 2.f;
    constexpr float kIconBtnY = kIconRowY;
    constexpr float kIconStep = kIconBtnW + kBtnGap;
    IControl *undoBtn = MakeIconMomentary(
        IRECT(kCol1X, kIconBtnY, kCol1X + kIconBtnW, kIconBtnY + kRowH), [this](IControl *) { Undo(); }, kIconUndo);
    pGraphics->AttachControl(undoBtn);
    IControl *redoBtn = MakeIconMomentary(
        IRECT(kCol1X + kIconStep, kIconBtnY, kCol1X + kIconStep + kIconBtnW, kIconBtnY + kRowH),
        [this](IControl *) { Redo(); }, kIconRedo);
    pGraphics->AttachControl(redoBtn);
    IControl *saveBtn = MakeIconMomentary(
        IRECT(kCol1X + 2.f * kIconStep, kIconBtnY, kCol1X + 2.f * kIconStep + kIconBtnW, kIconBtnY + kRowH),
        [this](IControl *) { SaveFile(); }, kIconSave);
    pGraphics->AttachControl(saveBtn);
    IControl *loadBtn = MakeIconMomentary(
        IRECT(kCol1X + 3.f * kIconStep, kIconBtnY, kPanelR, kIconBtnY + kRowH),
        [this](IControl *) { LoadFile(); }, kIconLoad);
    pGraphics->AttachControl(loadBtn);

    // 电平表模式按钮（dBTP/dBFS，幽灵样式覆盖在电平条上）
    mLevelModeBtn =
        new FlatCycleButton(IRECT(plotR, plotB - kRangeBtnH, plotR + 2.f * kGainBarW, plotB), kLevelMode,
                            {"dBTP", "dBFS"}, btnStyle);
    mLevelModeBtn->SetTextSize(11.f);
    mLevelModeBtn->SetGhostStyle(true);
    pGraphics->AttachControl(mLevelModeBtn);
    bindTip(mLevelModeBtn, orm::kTxtTipLevelMode);

    // RESET（全量重置：电平表 + 频谱 hold + 响度）
    mLevelResetBtn =
        MakeMomentary(IRECT(kCol2X, kFreezeRowY, kPanelR, kFreezeRowY + kRowH), [this](IControl *) {
          mLevelResetFlag.store(true);
          mLoudResetFlag.store(true);
          if (mSpectrumPad)
            mSpectrumPad->ClearPeakHold();
          if (mScopeCtrl)
            mScopeCtrl->ClearPeakHold();
          if (mOscilloscopeCtrl)
            mOscilloscopeCtrl->Clear();
        }, "RESET", btnStyle);
    pGraphics->AttachControl(mLevelResetBtn);
    bindText(orm::kTxtReset, [this](const char *s) {
      if (mLevelResetBtn) {
        mLevelResetBtn->SetLabelStr(s);
        mLevelResetBtn->SetDirty(false);
      }
    });
    bindTip(mLevelResetBtn, orm::kTxtTipReset);

    // Freeze 开关
    mFreezeBtn = new FlatToggleControl(IRECT(kCol1X, kFreezeRowY, kCol1X + kBtnW, kFreezeRowY + kRowH), kFreeze, " ", toggleStyle,
                                       "FREEZE", "FREEZE");
    pGraphics->AttachControl(mFreezeBtn);
    bindText(orm::kTxtFreeze, [this](const char *s) {
      if (mFreezeBtn) {
        mFreezeBtn->SetOnText(s);
        mFreezeBtn->SetOffText(s);
      }
    });
    bindTip(mFreezeBtn, orm::kTxtTipFreeze);

    // 峰值保持开关 + 时长
    mLevelHoldBtn = new FlatToggleControl(IRECT(kCol1X, kHoldRowY, kCol1X + kBtnW, kHoldRowY + kRowH), kLevelHoldOn, " ", toggleStyle,
                                          "HOLD", "HOLD");
    pGraphics->AttachControl(mLevelHoldBtn);
    bindText(orm::kTxtLevelHold, [this](const char *s) {
      if (mLevelHoldBtn) {
        mLevelHoldBtn->SetOnText(s);
        mLevelHoldBtn->SetOffText(s);
      }
    });
    bindTip(mLevelHoldBtn, orm::kTxtTipLevelHold);

    mLevelHoldTimeBtn =
        new FlatCycleButton(IRECT(kCol2X, kHoldRowY, kPanelR, kHoldRowY + kRowH), kLevelHold, {"0.5s", "2s", "∞"}, btnStyle);
    pGraphics->AttachControl(mLevelHoldTimeBtn);
    bindTip(mLevelHoldTimeBtn, orm::kTxtTipLevelHoldTime);

    // 底部标题栏
    IText ormText(32, COL_900(), kFontBold, EAlign::Near, EVAlign::Bottom);
    pGraphics->AttachControl(new SectionTitleControl(IRECT(kCol1X, 618, kCol1X + 120, 652), "ORM", ormText, 0));
    IRECT ormInk(kCol1X, 618, kCol1X + 120, 652);
    pGraphics->MeasureText(ormText, "ORM", ormInk);
    const float gearL = ormInk.R + 8.f;
    const float gearR = gearL + (ormInk.B - ormInk.T);
    pGraphics->AttachControl(
        new SettingsMenuButton(IRECT(gearL, ormInk.T, gearR, ormInk.B), [this]() { ToggleSettingsPanel(); }));
    pGraphics->AttachControl(new SectionTitleControl(IRECT(kCol1X, 650, kPanelR, 684), "Analyzer",
                                                     IText(32, COL_900(), kFontBold, EAlign::Near, EVAlign::Middle),
                                                     0));
    pGraphics->AttachControl(new SectionTitleControl(IRECT(gearR + 8.f, 615, kPanelR, 649), "v" PLUG_VERSION_STR,
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
#if ORM_ENABLE_TEST_GEN
    // 频率/电平拖动不逐像素写盘，打 pending 标记由 OnIdle 稳定后落盘
    auto markGenSave = [this]() {
      mGenSavePending.store(true, std::memory_order_relaxed);
      mGenSaveTp = std::chrono::steady_clock::now();
    };
    settingsHooks.gen.type = [this]() { return mGenType.load(std::memory_order_relaxed); };
    settingsHooks.gen.onType = [this](int t) {
      mGenType.store(std::clamp(t, 0, orm::kNumGenSignals - 1), std::memory_order_relaxed);
      SaveSettingsToDisk();
    };
    settingsHooks.gen.freq = [this]() { return (double)mGenFreq.load(std::memory_order_relaxed); };
    settingsHooks.gen.onFreq = [this, markGenSave](double f) {
      mGenFreq.store((float)std::clamp(f, 1.0, 20000.0), std::memory_order_relaxed);
      markGenSave();
    };
    settingsHooks.gen.level = [this]() { return (double)mGenLevel.load(std::memory_order_relaxed); };
    settingsHooks.gen.onLevel = [this, markGenSave](double db) {
      mGenLevel.store((float)std::clamp(db, -120.0, 0.0), std::memory_order_relaxed);
      markGenSave();
    };
    settingsHooks.gen.hold = [this]() { return mGenHold.load(std::memory_order_relaxed); };
    settingsHooks.gen.onHold = [this](bool on) {
      mGenHold.store(on, std::memory_order_relaxed);
      SaveSettingsToDisk();
    };
    settingsHooks.gen.toOutput = [this]() { return mGenToOutput.load(std::memory_order_relaxed); };
    settingsHooks.gen.onToOutput = [this](bool on) {
      mGenToOutput.store(on, std::memory_order_relaxed);
      SaveSettingsToDisk();
    };
    settingsHooks.gen.onRestart = [this]() { mGenRestartReq.store(true, std::memory_order_relaxed); };
    settingsHooks.gen.onReseed = [this]() {
      const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
      mGenSeedReq.store((int)(((uint32_t)ticks | 1u) & 0x7FFFFFFFu), std::memory_order_relaxed);
    };
#endif
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

    // 快捷键：F=帧间耗时显示，P=频谱绘制耗时 HUD
    pGraphics->SetKeyHandlerFunc([this, pGraphics](const IKeyPress &key, bool isUp) {
      if (isUp || key.C || key.A || key.S)
        return false;
      switch (key.utf8[0]) {
        case 'f':
        case 'F':
          pGraphics->ShowFPSDisplay(!pGraphics->ShowingFPSDisplay());
          return true;
        case 'p':
        case 'P':
          if (mSpectrumPad)
            mSpectrumPad->ToggleHud();
          return true;
        default:
          return false;
      }
    });

    ApplyLanguage();

    // 立即同步一次频谱配置：App 模式下 OnIdle 首帧可能早于控件 attach，
    // SendControlMsgFromDelegate 对不存在的控件会静默丢弃
    SendSpectrumConfig();
    mSentSampleRate = GetSampleRate();
    mSentFFTSize = CurrentFFTSize();
    mSentSpeed = (int)GetParam(kSpeed)->Value();
    mSentReleaseMode = (int)GetParam(kReleaseMode)->Value();
    mSentRelease = SpeedReleaseSec(mSentSpeed, mSentReleaseMode);
    mSentRange = (double)std::clamp(std::lround(GetParam(kRange)->Value()), 0L, 2L);
    mSentLfRes = GetParam(kLfRes)->Value();
    mSentSlope = EffectiveSlopeDb();
    mSentMode = (int)GetParam(kMode)->Value();
    mSentChanMode = (int)GetParam(kChannelMode)->Value();
    mSentWindowFFT = CurrentFFTWindow();
    mSentWindowVQT = CurrentVQTWindow();
    mUIOpen.store(true, std::memory_order_release);
  };
#endif
}

#if IPLUG_DSP
void ORMAnalyzer::ProcessBlock(sample **inputs, sample **outputs, int nFrames) {
  const uint64_t cpuT0 = ThreadCpuNs();
  nFrames = std::min(nFrames, kMaxBlock);

  const int nOuts = NOutChansConnected();
  const int nIns = NInChansConnected();

  // 音频直通（分析器不改变信号）
  if (nOuts >= 2 && nIns >= 2) {
    if (outputs[0] != inputs[0])
      std::memcpy(outputs[0], inputs[0], nFrames * sizeof(sample));
    if (outputs[1] != inputs[1])
      std::memcpy(outputs[1], inputs[1], nFrames * sizeof(sample));
    for (int c = 2; c < nOuts; ++c) {
      if (outputs[c] != inputs[c])
        std::memcpy(outputs[c], inputs[c], nFrames * sizeof(sample));
    }
  } else if (nOuts >= 2) {
    if (outputs[0] != inputs[0])
      std::memcpy(outputs[0], inputs[0], nFrames * sizeof(sample));
    std::memcpy(outputs[1], inputs[0], nFrames * sizeof(sample));
    for (int c = 2; c < nOuts; ++c) {
      if (outputs[c] != outputs[0])
        std::memcpy(outputs[c], outputs[0], nFrames * sizeof(sample));
    }
  } else {
    if (outputs[0] != inputs[0])
      std::memcpy(outputs[0], inputs[0], nFrames * sizeof(sample));
    for (int c = 1; c < nOuts; ++c) {
      if (outputs[c] != outputs[0])
        std::memcpy(outputs[c], outputs[0], nFrames * sizeof(sample));
    }
  }

  // UI 未打开时挂起所有分析
  if (!mUIOpen.load(std::memory_order_relaxed)) {
    const double processMs = (double)(ThreadCpuNs() - cpuT0) / 1e6;
    const double blockMs = (double)nFrames / std::max(GetSampleRate(), 1.0) * 1000.0;
    if (blockMs > 0.0)
      mCpuAudio += (processMs / blockMs - mCpuAudio) * 0.1;
    return;
  }

  const int mode = (int)GetParam(kMode)->Value();
  const bool frozen = GetParam(kFreeze)->Value() > 0.5;

#if ORM_ENABLE_TEST_GEN
  // 内置测试信号：用已知信号替换宿主输入，冻结+切引擎可做公平对比
  const bool genOn = mGenType.load(std::memory_order_relaxed) != orm::kGenOff;
  if (genOn) {
    if (mGenRestartReq.exchange(false, std::memory_order_relaxed))
      mTestGen.Restart();
    if (const int sd = mGenSeedReq.exchange(0, std::memory_order_relaxed))
      mTestGen.SetSeed((uint32_t)sd);
    orm::TestSignalGenerator::Config gc;
    gc.type = mGenType.load(std::memory_order_relaxed);
    gc.freqHz = (double)mGenFreq.load(std::memory_order_relaxed);
    gc.levelDb = (double)mGenLevel.load(std::memory_order_relaxed);
    gc.fftSize = mSpectrum.GetFFTSize();
    mTestGen.SetConfig(gc);
    const bool advance = !(frozen && mGenHold.load(std::memory_order_relaxed));
    mTestGen.Fill(mSpecInL.data(), mSpecInR.data(), nFrames, GetSampleRate(), advance);
    for (int s = 0; s < nFrames; ++s)
      mSpecInM[s] = (mSpecInL[s] + mSpecInR[s]) * 0.7071067811865475;
    if (mGenToOutput.load(std::memory_order_relaxed) && nOuts >= 1) {
      // 硬限幅 -1 dBFS，脉冲/白噪满量程直送监听危险
      constexpr sample kOutCeil = (sample)0.8912504381337891;
      for (int s = 0; s < nFrames; ++s) {
        outputs[0][s] = std::clamp(mSpecInL[s], -kOutCeil, kOutCeil);
        if (nOuts >= 2)
          outputs[1][s] = std::clamp(mSpecInR[s], -kOutCeil, kOutCeil);
      }
      for (int c = 2; c < nOuts; ++c)
        std::memcpy(outputs[c], outputs[0], nFrames * sizeof(sample));
    }
  }
#else
  const bool genOn = false;
#endif

  sample *spec[3] = {mSpecInL.data(), mSpecInR.data(), mSpecInM.data()};
  if (genOn) {
    // 已由发生器填充
  } else if (nIns >= 2) {
    std::memcpy(mSpecInL.data(), inputs[0], nFrames * sizeof(sample));
    std::memcpy(mSpecInR.data(), inputs[1], nFrames * sizeof(sample));
    for (int s = 0; s < nFrames; ++s)
      mSpecInM[s] = (mSpecInL[s] + mSpecInR[s]) * 0.7071067811865475;
  } else {
    std::memcpy(mSpecInL.data(), inputs[0], nFrames * sizeof(sample));
    std::memcpy(mSpecInR.data(), inputs[0], nFrames * sizeof(sample));
    for (int s = 0; s < nFrames; ++s)
      mSpecInM[s] = mSpecInL[s] * 0.7071067811865475;
  }
  if (!frozen) {
    // 冻结环形缓冲：常驻记录最近输入，freeze 后停止写入
    for (int s = 0; s < nFrames; ++s) {
      const int p = mFreezeRingPos.load(std::memory_order_relaxed);
      mFreezeRing[0][p] = mSpecInL[s];
      mFreezeRing[1][p] = mSpecInR[s];
      mFreezeRing[2][p] = mSpecInM[s];
      mFreezeRingPos.store((p + 1) & (kFreezeRingLen - 1), std::memory_order_relaxed);
    }
    if (mode == kModeVQT) {
      mVQT.ProcessBlock(spec, nFrames, kCtrlTagPad, 3);
      mEngineHopPhase[kModeVQT].store(mVQT.HopPhase(), std::memory_order_relaxed);
    } else if (mode == kModePBT) {
      mPBT.ProcessBlock(spec, nFrames, kCtrlTagPad, 3);
      mEngineHopPhase[kModePBT].store(mPBT.HopPhase(), std::memory_order_relaxed);
    } else if (mode == kModeRTA) {
      mRTA.ProcessBlock(spec, nFrames, kCtrlTagPad, 3);
      mEngineHopPhase[kModeRTA].store(mRTA.HopPhase(), std::memory_order_relaxed);
    } else {
      mSpectrum.ProcessBlock(spec, nFrames, kCtrlTagPad, 3);
      mEngineHopPhase[kModeFFT].store(mSpectrum.HopPhase(), std::memory_order_relaxed);
    }

    // 声像引擎常驻，与频谱模式无关
    sample *scopeIn[2] = {mSpecInL.data(), mSpecInR.data()};
    mScope.ProcessBlock(scopeIn, nFrames, kCtrlTagScope);
  }

  // 电平表 + 响度计（冻结中挂起测量，重置请求仍消费）
  auto publishLoudness = [this](const LoudnessMeter::Snapshot &ls) {
    mLoudM.store(ls.momentary, std::memory_order_relaxed);
    mLoudS.store(ls.shortTerm, std::memory_order_relaxed);
    mLoudI.store(ls.integrated, std::memory_order_relaxed);
    mLoudLra.store(ls.range, std::memory_order_relaxed);
    mLoudLraMin.store(ls.lraMin, std::memory_order_relaxed);
    mLoudLraMax.store(ls.lraMax, std::memory_order_relaxed);
    mLoudTp.store(ls.tpMax, std::memory_order_relaxed);
    mLoudIValid.store(ls.iValid, std::memory_order_relaxed);
    mLoudLraValid.store(ls.lraValid, std::memory_order_relaxed);
  };
  if (frozen) {
    if (mLevelResetHoldFlag.exchange(false, std::memory_order_relaxed))
      mLevelMeter.ResetHold();
    if (mLevelResetFlag.exchange(false))
      mLevelMeter.ResetHoldOver();
    if (mLevelResetPersistFlag.exchange(false, std::memory_order_relaxed))
      mLevelMeter.ResetPersist();
    if (mLevelResetMeterHoldFlag.exchange(false, std::memory_order_relaxed))
      mLevelMeter.ResetMeterHold();
    if (mLevelResetOverFlag.exchange(false, std::memory_order_relaxed))
      mLevelMeter.ResetOver();
    if (mLoudResetFlag.exchange(false, std::memory_order_relaxed)) {
      mLoudness.Reset();
      LoudnessMeter::Snapshot ls;
      mLoudness.Store(ls);
      publishLoudness(ls);
    }
  } else {
    const double newSR = mLevelSetSR.exchange(-1.0, std::memory_order_relaxed);
    if (newSR > 0.0)
      mLevelMeter.SetSampleRate(newSR);
    if (mLevelResetHoldFlag.exchange(false, std::memory_order_relaxed))
      mLevelMeter.ResetHold();
    if (mLevelResetFlag.exchange(false))
      mLevelMeter.ResetHoldOver();
    if (mLevelResetPersistFlag.exchange(false, std::memory_order_relaxed))
      mLevelMeter.ResetPersist();
    if (mLevelResetMeterHoldFlag.exchange(false, std::memory_order_relaxed))
      mLevelMeter.ResetMeterHold();
    if (mLevelResetOverFlag.exchange(false, std::memory_order_relaxed))
      mLevelMeter.ResetOver();
    mLevelMeter.Process(mSpecInL.data(), mSpecInR.data(), nFrames, (int)GetParam(kLevelMode)->Value(),
                        CurrentHoldSec());
    {
      LevelMeter::Snapshot s;
      mLevelMeter.Store(s);
      mPeakL.store(s.peakDbL, std::memory_order_relaxed);
      mPeakR.store(s.peakDbR, std::memory_order_relaxed);
      mTrueL.store(s.trueDbL, std::memory_order_relaxed);
      mTrueR.store(s.trueDbR, std::memory_order_relaxed);
      mRmsL.store(s.rmsDbL, std::memory_order_relaxed);
      mRmsR.store(s.rmsDbR, std::memory_order_relaxed);
      mVuL.store(s.vuDbL, std::memory_order_relaxed);
      mVuR.store(s.vuDbR, std::memory_order_relaxed);
      mVuHoldL.store(s.vuHoldDbL, std::memory_order_relaxed);
      mVuHoldR.store(s.vuHoldDbR, std::memory_order_relaxed);
      mPersistL.store(s.persistDbL, std::memory_order_relaxed);
      mPersistR.store(s.persistDbR, std::memory_order_relaxed);
      mHoldL.store(s.holdDbL, std::memory_order_relaxed);
      mHoldR.store(s.holdDbR, std::memory_order_relaxed);
      mHoldSec.store(s.holdSec, std::memory_order_relaxed);
      mOverL.store(s.overL, std::memory_order_relaxed);
      mOverR.store(s.overR, std::memory_order_relaxed);

      // 响度计：真峰值复用 LevelMeter 的 4x 过采样结果
      const double loudSR = mLoudSetSR.exchange(-1.0, std::memory_order_relaxed);
      if (loudSR > 0.0)
        mLoudness.SetSampleRate(loudSR);
      if (mLoudResetFlag.exchange(false, std::memory_order_relaxed))
        mLoudness.Reset();
      mLoudness.Process(mSpecInL.data(), mSpecInR.data(), nFrames);
      mLoudness.SetTruePeaks(s.trueDbL, s.trueDbR);
      LoudnessMeter::Snapshot ls;
      mLoudness.Store(ls);
      publishLoudness(ls);
    }
  }

  // 音频线程耗时（一阶平滑）
  {
    const double processMs = (double)(ThreadCpuNs() - cpuT0) / 1e6;
    const double blockMs = (double)nFrames / std::max(GetSampleRate(), 1.0) * 1000.0;
    if (blockMs > 0.0)
      mCpuAudio += (processMs / blockMs - mCpuAudio) * 0.1;
  }
}

void ORMAnalyzer::OnReset() {
  mSpectrum.SetFFTSizeAndOverlap(CurrentFFTSize(), FFTOverlapForSize(CurrentFFTSize()), CurrentFFTWindow());
  mVQT.SetSampleRate(GetSampleRate());
  mVQT.SetWindowType(CurrentVQTWindow());
  mVQT.SetGamma(CurrentVQTGamma());
  mVQT.SetBpo(kVQTBpo);
  mPBT.SetSampleRate(GetSampleRate());
  mPBT.SetLfWidth(CurrentPbtLfRes());
  mRTA.SetSampleRate(GetSampleRate());
  mRTA.SetOctaveMode(CurrentRtaOctave());
  mLevelSetSR.store(GetSampleRate(), std::memory_order_relaxed);
  mLoudSetSR.store(GetSampleRate(), std::memory_order_relaxed);
  mPeakL.store(0.f, std::memory_order_relaxed);
  mPeakR.store(0.f, std::memory_order_relaxed);
#if ORM_ENABLE_TEST_GEN
  mTestGen.Restart();
#endif
}

void ORMAnalyzer::SendSpectrumConfig() {
  const double sr = GetSampleRate();
  // 下发引擎实际生效的 FFT 尺寸，而非参数名义值
  const int fftSize = mSpectrum.GetFFTSize();
  const int speedIdx = (int)GetParam(kSpeed)->Value();
  const int releaseMode = (int)GetParam(kReleaseMode)->Value();
  const float release = (float)SpeedReleaseSec(speedIdx, releaseMode);
  const float range = CurrentRangeDb();
  const float slopeDb = (float)EffectiveSlopeDb();
  const int chanTri = (int)GetParam(kChannelMode)->Value();
  const int chanMode = (chanTri == kChanModeLR) ? 0 : 1;
  const int mergeAlgo = (chanTri == kChanModeSUM) ? 1 : 0;
  SendControlMsgFromDelegate(kCtrlTagPad, SpectrumPad::kMsgTagSampleRate, sizeof(double), &sr);
  SendControlMsgFromDelegate(kCtrlTagPad, SpectrumPad::kMsgTagFFTSize, sizeof(int), &fftSize);
  SendControlMsgFromDelegate(kCtrlTagPad, SpectrumPad::kMsgTagRelease, sizeof(float), &release);
  SendControlMsgFromDelegate(kCtrlTagPad, SpectrumPad::kMsgTagReleaseMode, sizeof(int), &releaseMode);
  SendControlMsgFromDelegate(kCtrlTagPad, SpectrumPad::kMsgTagRange, sizeof(float), &range);
  SendControlMsgFromDelegate(kCtrlTagPad, SpectrumPad::kMsgTagSlope, sizeof(float), &slopeDb);
  SendControlMsgFromDelegate(kCtrlTagPad, SpectrumPad::kMsgTagChanMode, sizeof(int), &chanMode);
  SendControlMsgFromDelegate(kCtrlTagPad, SpectrumPad::kMsgTagMergeAlgo, sizeof(int), &mergeAlgo);
  const int mode = (int)GetParam(kMode)->Value();
  SendControlMsgFromDelegate(kCtrlTagPad, SpectrumPad::kMsgTagMode, sizeof(int), &mode);
  if (mode == kModeVQT)
    SendVQTBandFreqs();
  else if (mode == kModePBT)
    SendPBTBandFreqs();
  else if (mode == kModeRTA)
    SendRTABandFreqs();

  // 声像面板弹道参数与频谱共用
  SendControlMsgFromDelegate(kCtrlTagScope, StereoFieldControl::kMsgTagSampleRate, sizeof(double), &sr);
  SendControlMsgFromDelegate(kCtrlTagScope, StereoFieldControl::kMsgTagRelease, sizeof(float), &release);
  SendControlMsgFromDelegate(kCtrlTagScope, StereoFieldControl::kMsgTagReleaseMode, sizeof(int), &releaseMode);
}

void ORMAnalyzer::SendVQTBandFreqs() {
  const auto &freqs = mVQT.BandFreqs();
  if (freqs.empty())
    return;
  std::vector<float> buf(freqs.begin(), freqs.end());
  SendControlMsgFromDelegate(kCtrlTagPad, SpectrumPad::kMsgTagVQTBands,
                             (int)(buf.size() * sizeof(float)), buf.data());
}

void ORMAnalyzer::SendPBTBandFreqs() {
  const auto &freqs = mPBT.BandFreqs();
  if (freqs.empty())
    return;
  std::vector<float> buf(freqs.begin(), freqs.end());
  SendControlMsgFromDelegate(kCtrlTagPad, SpectrumPad::kMsgTagPBTBands,
                             (int)(buf.size() * sizeof(float)), buf.data());
}

void ORMAnalyzer::SendRTABandFreqs() {
  const auto &freqs = mRTA.BandFreqs();
  if (freqs.empty())
    return;
  std::vector<float> buf(freqs.begin(), freqs.end());
  SendControlMsgFromDelegate(kCtrlTagPad, SpectrumPad::kMsgTagRTABands,
                             (int)(buf.size() * sizeof(float)), buf.data());
}

void ORMAnalyzer::SendResetToPad() {
  const int dummy = 0;
  SendControlMsgFromDelegate(kCtrlTagPad, SpectrumPad::kMsgTagReset, sizeof(int), &dummy);
}

void ORMAnalyzer::OnParamChange(int paramIdx, EParamSource source, int sampleOffset) {
  // 冻结中抑制 SendResetToPad，画面保持定格
  const bool frozen = GetParam(kFreeze)->Value() > 0.5;
  if (paramIdx == kRes)
    mSpectrum.SetFFTSizeAndOverlap(CurrentFFTSize(), FFTOverlapForSize(CurrentFFTSize()), CurrentFFTWindow());
  else if (paramIdx == kFFTWindow) {
    mSpectrum.SetWindowType(CurrentFFTWindow());
  } else if (paramIdx == kWindowVQT) {
    if (mVQT.SetWindowType(CurrentVQTWindow()) && !frozen)
      SendResetToPad();
  }
  else if (paramIdx == kLfRes) {
    if (GetParam(kMode)->Value() > 1.5 && GetParam(kMode)->Value() < 2.5) {
      if (mPBT.SetLfWidth(CurrentPbtLfRes()) && !frozen)
        SendResetToPad();
    }
  } else if (paramIdx == kVQTGamma) {
    if (GetParam(kMode)->Value() < 1.5) {
      if (mVQT.SetGamma(CurrentVQTGamma()) && !frozen)
        SendResetToPad();
    }
  } else if (paramIdx == kRtaOctave) {
    if (GetParam(kMode)->Value() > 2.5) {
      if (mRTA.SetOctaveMode(CurrentRtaOctave()) && !frozen)
        SendResetToPad();
    }
  } else if (paramIdx == kLevelMode) {
    // 模式切换清除峰值保持（过载锁存保留）
    mLevelResetHoldFlag.store(true, std::memory_order_relaxed);
  }
}

void ORMAnalyzer::OnParamChangeUI(int paramIdx, EParamSource source) {
  if (source == EParamSource::kUI) {
    MaybePushGestureUndo();
    if (paramIdx == kLevelHoldOn)
      SaveSettingsToDisk();
  }
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
  if (!mUIOpen.load(std::memory_order_relaxed))
    return;

  using namespace std::chrono;
  const auto wallNow = steady_clock::now();
  const double idleGapMs = duration<double, std::milli>(wallNow - mLastIdleTp).count();
  mLastIdleTp = wallNow;
  const uint64_t workT0 = ThreadCpuNs();

  // 按需重建频带配置
  mVQT.CheckRebuild();
  mPBT.CheckRebuild();
  mRTA.CheckRebuild();

  const bool frozen = GetParam(kFreeze)->Value() > 0.5;

  auto syncFreezeSnapshot = [this]() {
    mFreezeRes = (int)std::lround(GetParam(kRes)->Value());
    mFreezeWindowFFT = CurrentFFTWindow();
    mFreezeWindowVQT = CurrentVQTWindow();
    mFreezeLf = (int)std::lround(GetParam(kLfRes)->Value());
    mFreezeRtaOct = CurrentRtaOctave();
  };

  // 模式切换
  const int mode = (int)GetParam(kMode)->Value();
  if (mode != mSentMode) {
    mSentMode = mode;
    if (mResBtn && mPbtLfResBtn) {
      mResBtn->Hide(mode != kModeFFT);
      mPbtLfResBtn->Hide(mode != kModePBT);
      mGammaBtn->Hide(mode != kModeVQT);
      if (mRtaOctBtn)
        mRtaOctBtn->Hide(mode != kModeRTA);
    }
    if (mWindowBtn) {
      mWindowBtn->Hide(mode != kModeFFT && mode != kModeVQT);
      mWindowBtn->SetParamIdx(mode == kModeFFT ? kFFTWindow : kWindowVQT);
    }
    if (mSlopeBtn) {
      mSlopeBtn->SetParamIdx(SlopeParamForMode(mode));
      if (mode == kModeFFT)
        mSlopeBtn->SetLabels({"0 dB/Oct", "3 dB/Oct", "4.5 dB/Oct"});
      else
        mSlopeBtn->SetLabels({"-3 dB/Oct", "0 dB/Oct", "1.5 dB/Oct"});
    }
    const float slopeDb = (float)EffectiveSlopeDb();
    mSentSlope = slopeDb;
    SendControlMsgFromDelegate(kCtrlTagPad, SpectrumPad::kMsgTagSlope, sizeof(float), &slopeDb);
    SendControlMsgFromDelegate(kCtrlTagPad, SpectrumPad::kMsgTagMode, sizeof(int), &mode);
    if (mode == kModeVQT)
      SendVQTBandFreqs();
    else if (mode == kModePBT)
      SendPBTBandFreqs();
    else if (mode == kModeRTA)
      SendRTABandFreqs();
    if (frozen) {
      StartFreezeReplay();
      syncFreezeSnapshot();
    } else {
      SendResetToPad();
    }
  }

  // 窗/斜率按钮本地值与参数值同步（改绑后必须保持一致，否则下一次切换会被吞）
  if (mWindowBtn && mWindowBtn->GetParam())
    mWindowBtn->SetValueFromDelegate(mWindowBtn->GetParam()->GetNormalized(), 0);
  if (mSlopeBtn && mSlopeBtn->GetParam())
    mSlopeBtn->SetValueFromDelegate(mSlopeBtn->GetParam()->GetNormalized(), 0);

  // 声道模式
  const int chanTri = (int)GetParam(kChannelMode)->Value();
  mSpectrum.SetChannelMode(chanTri);
  mVQT.SetChannelMode(chanTri);
  mPBT.SetChannelMode(chanTri);
  mRTA.SetChannelMode(chanTri);
  if (chanTri != mSentChanMode) {
    mSentChanMode = chanTri;
    const int chanMode = (chanTri == kChanModeLR) ? 0 : 1;
    const int mergeAlgo = (chanTri == kChanModeSUM) ? 1 : 0;
    SendControlMsgFromDelegate(kCtrlTagPad, SpectrumPad::kMsgTagChanMode, sizeof(int), &chanMode);
    SendControlMsgFromDelegate(kCtrlTagPad, SpectrumPad::kMsgTagMergeAlgo, sizeof(int), &mergeAlgo);
    if (frozen)
      StartFreezeReplay();
  }

  // 参数变动去重下发
  {
    const double sr = GetSampleRate();
    const int fftSize = CurrentFFTSize();
    const int winFFT = CurrentFFTWindow();
    const int winVQT = CurrentVQTWindow();
    const int speed = (int)GetParam(kSpeed)->Value();
    const int releaseMode = (int)GetParam(kReleaseMode)->Value();
    const double release = SpeedReleaseSec(speed, releaseMode);
    const int rangeIdx = (int)std::clamp(std::lround(GetParam(kRange)->Value()), 0L, 2L);
    if (GetParam(kRange)->Value() != (double)rangeIdx)
      SetParamFromEditor(kRange, (double)rangeIdx);
    const double range = (double)rangeIdx;
    const double lfRes = GetParam(kLfRes)->Value();
    const double slope = EffectiveSlopeDb();
    const int rtaOct = CurrentRtaOctave();
    if (sr != mSentSampleRate || fftSize != mSentFFTSize || winFFT != mSentWindowFFT || winVQT != mSentWindowVQT ||
        speed != mSentSpeed || releaseMode != mSentReleaseMode || release != mSentRelease ||
        range != mSentRange ||
        lfRes != mSentLfRes || slope != mSentSlope || rtaOct != mSentRtaOct) {
      mSpectrum.SetWindowType(winFFT);
      mVQT.SetWindowType(winVQT);
      // 冻结中影响计算的参数变化需重启回放，Range/斜率等纯显示参数不重启
      const bool restartReplay =
          frozen && (sr != mSentSampleRate || release != mSentRelease || releaseMode != mSentReleaseMode ||
                     (mode == kModeFFT && winFFT != mSentWindowFFT) || (mode == kModeVQT && winVQT != mSentWindowVQT));
      mSentSampleRate = sr;
      mSentFFTSize = fftSize;
      mSentWindowFFT = winFFT;
      mSentWindowVQT = winVQT;
      mSentSpeed = speed;
      mSentReleaseMode = releaseMode;
      mSentRelease = release;
      mSentRange = range;
      mSentLfRes = lfRes;
      mSentSlope = slope;
      mSentRtaOct = rtaOct;
      SendSpectrumConfig();
      if (restartReplay)
        StartFreezeReplay();
    }
  }

  // 冻结中档位变化 -> 用冻结缓冲确定性回放
  if (frozen) {
    if (!mFreezeOn) {
      mFreezeOn = true;
      syncFreezeSnapshot();
    } else {
      const int resIdx = (int)std::lround(GetParam(kRes)->Value());
      const int winFFT = CurrentFFTWindow();
      const int winVQT = CurrentVQTWindow();
      const int lfIdx = (int)std::lround(GetParam(kLfRes)->Value());
      const int rtaOctIdx = CurrentRtaOctave();
      const bool cfgChanged =
          (mode == kModeFFT && (resIdx != mFreezeRes || winFFT != mFreezeWindowFFT)) ||
          (mode == kModeVQT &&
           (lfIdx != mFreezeLf || winVQT != mFreezeWindowVQT)) ||
          (mode == kModePBT && lfIdx != mFreezeLf) ||
          (mode == kModeRTA && rtaOctIdx != mFreezeRtaOct);
      if (cfgChanged) {
        StartFreezeReplay();
      }
      mFreezeRes = resIdx;
      mFreezeWindowFFT = winFFT;
      mFreezeWindowVQT = winVQT;
      mFreezeLf = lfIdx;
      mFreezeRtaOct = rtaOctIdx;
    }
    PumpFreezeReplay();
  } else {
    mFreezeOn = false;
    mReplayMode = -1;
  }

  // 分发引擎数据（冻结中跳过）
  if (!frozen) {
    if (mode == kModeVQT)
      mVQT.TransmitData(*this);
    else if (mode == kModePBT)
      mPBT.TransmitData(*this);
    else if (mode == kModeRTA)
      mRTA.TransmitData(*this);
    else
      mSpectrum.TransmitData(*this);
    mScope.TransmitData(*this);
  }

  // 示波器波形抽取与更新
  if (mOscilloscopeCtrl) {
    const float *rings[3] = {mFreezeRing[0].data(), mFreezeRing[1].data(), mFreezeRing[2].data()};
    mOscilloscopeCtrl->UpdateAudio(rings, kFreezeRingLen, mFreezeRingPos.load(std::memory_order_relaxed),
                                   GetSampleRate(), frozen);
  }

  // 转发电平表数据
  {
    LevelMeterUiData d;
    d.peakL = mPeakL.load(std::memory_order_relaxed);
    d.peakR = mPeakR.load(std::memory_order_relaxed);
    d.trueL = mTrueL.load(std::memory_order_relaxed);
    d.trueR = mTrueR.load(std::memory_order_relaxed);
    d.rmsL = mRmsL.load(std::memory_order_relaxed);
    d.rmsR = mRmsR.load(std::memory_order_relaxed);
    d.vuL = mVuL.load(std::memory_order_relaxed);
    d.vuR = mVuR.load(std::memory_order_relaxed);
    d.vuHoldL = mVuHoldL.load(std::memory_order_relaxed);
    d.vuHoldR = mVuHoldR.load(std::memory_order_relaxed);
    d.persistL = mPersistL.load(std::memory_order_relaxed);
    d.persistR = mPersistR.load(std::memory_order_relaxed);
    d.holdL = mHoldL.load(std::memory_order_relaxed);
    d.holdR = mHoldR.load(std::memory_order_relaxed);
    d.holdSec = mHoldSec.load(std::memory_order_relaxed);
    d.mode = (int)GetParam(kLevelMode)->Value();
    d.overL = mOverL.load(std::memory_order_relaxed);
    d.overR = mOverR.load(std::memory_order_relaxed);
    SendControlMsgFromDelegate(kCtrlTagPad, SpectrumPad::kMsgTagLevelMeter, sizeof(d), &d);
  }

  // dBTP/dBFS 幽灵文字信号态：有条体渐变时退回条轨灰，空轨时深灰可点
  if (mLevelModeBtn) {
    const bool dbtp = GetParam(kLevelMode)->Value() < 0.5;
    const float bottomDb = -CurrentRangeDb();
    const float lvlL = dbtp ? mTrueL.load(std::memory_order_relaxed) : mPeakL.load(std::memory_order_relaxed);
    const float lvlR = dbtp ? mTrueR.load(std::memory_order_relaxed) : mPeakR.load(std::memory_order_relaxed);
    mLevelModeBtn->SetGhostSignalActive(lvlL > bottomDb || lvlR > bottomDb);
  }

  // 转发响度计数据
  {
    LoudnessUiData d;
    d.momentary = mLoudM.load(std::memory_order_relaxed);
    d.shortTerm = mLoudS.load(std::memory_order_relaxed);
    d.integrated = mLoudI.load(std::memory_order_relaxed);
    d.range = mLoudLra.load(std::memory_order_relaxed);
    d.lraMin = mLoudLraMin.load(std::memory_order_relaxed);
    d.lraMax = mLoudLraMax.load(std::memory_order_relaxed);
    d.tpMax = mLoudTp.load(std::memory_order_relaxed);
    const int preset = (int)std::clamp(std::lround(GetParam(kLoudPreset)->Value()), 0L,
                                       (long)kNumLoudPresets - 1);
    d.preset = preset;
    d.target = (float)kLoudTargets[preset];
    const int scaleIdx = (int)std::clamp(std::lround(GetParam(kLoudScale)->Value()), 0L,
                                         (long)kNumLoudScaleOptions - 1);
    d.scaleOff = (int)kLoudScaleOffsets[scaleIdx];
    d.iValid = mLoudIValid.load(std::memory_order_relaxed) ? 1 : 0;
    d.lraValid = mLoudLraValid.load(std::memory_order_relaxed) ? 1 : 0;
    SendControlMsgFromDelegate(kCtrlTagPad, SpectrumPad::kMsgTagLoudness, sizeof(d), &d);
  }

  // UI 线程耗时 + 综合 CPU
  {
    const double uiMs = (double)(ThreadCpuNs() - workT0) / 1e6;
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

#if ORM_ENABLE_TEST_GEN
  // 发生器频率/电平稳定 0.5s 后落盘
  if (mGenSavePending.load(std::memory_order_relaxed) &&
      duration<double, std::milli>(steady_clock::now() - mGenSaveTp).count() > 500.0) {
    mGenSavePending.store(false, std::memory_order_relaxed);
    SaveSettingsToDisk();
  }
#endif

  const double now = duration<double>(steady_clock::now().time_since_epoch()).count();
  if (mGesturePending && now - mLastUIChangeTime > kGestureGapSec) {
    mStableSnapshot = Snapshot();
    mGesturePending = false;
  }
}

void ORMAnalyzer::OnUIOpen() {
  mUIOpen.store(true, std::memory_order_release);
#if IPLUG_EDITOR
  Plugin::OnUIOpen();
#endif
}

void ORMAnalyzer::OnUIClose() {
  mUIOpen.store(false, std::memory_order_release);
#if ORM_ENABLE_TEST_GEN
  if (mGenSavePending.exchange(false, std::memory_order_relaxed))
    SaveSettingsToDisk();
#endif
  mSpectrumPad = nullptr;
  mScopeCtrl = nullptr;
  mResBtn = nullptr;
  mPbtLfResBtn = nullptr;
  mGammaBtn = nullptr;
  mRtaOctBtn = nullptr;
  mRangeBtn = nullptr;
  mSpeedBtn = nullptr;
  mCpuMeter = nullptr;
  mModeBtn = nullptr;
  mChanModeBtn = nullptr;
  mLevelModeBtn = nullptr;
  mLevelResetBtn = nullptr;
  mLevelHoldBtn = nullptr;
  mLevelHoldTimeBtn = nullptr;
  mWindowBtn = nullptr;
  mFreezeBtn = nullptr;
  mSlopeBtn = nullptr;
  mScopeTrigBtn = nullptr;
  mScopeSrcBtn = nullptr;
  mScopeBtnL = nullptr;
  mScopeBtnR = nullptr;
  mScopeBtnM = nullptr;
  mScopeTimeBtn = nullptr;
  mScopeZoomBtn = nullptr;
  mSettingsPanel = nullptr;
  mTextBindings.clear();
  mTooltipBindings.clear();
  // 重置去重标记，重开 UI 后重发完整配置
  mSentSampleRate = 0.0;
  mSentFFTSize = 0;
  mSentWindowFFT = -1;
  mSentWindowVQT = -1;
  mSentRelease = -1.0;
  mSentMode = -1;
  mSentRange = -1.0;
  mSentLfRes = -1.0;
  mSentSlope = -1e9;
  mSentChanMode = -1;
  mSentRtaOct = -1;
  mFreezeOn = false;
  mFreezeRes = mFreezeWindowFFT = mFreezeWindowVQT = mFreezeLf = mFreezeRtaOct = -1;
  mReplayMode = -1;
}

// 冻结回放：display(cfg) = ballistics(replay(ring, cfg)) 为纯函数，
// 同样的环+配置永远得到同一画面。帧格与实时对齐：最新回放帧 = 冻结瞬间实时显示的最后一帧。
void ORMAnalyzer::StartFreezeReplay() {
  const int mode = (int)GetParam(kMode)->Value();
  constexpr int kHop = 1024;
  const int p = mFreezeRingPos.load(std::memory_order_relaxed);
  // 该引擎 pending 的 b 个样本尚未成帧，最新已显示帧终点在环尾前 b 个样本
  const int b = mEngineHopPhase[mode].load(std::memory_order_relaxed);
  mReplayLastStart = (p - b - kHop) & (kFreezeRingLen - 1);
  mReplayNFrames = (((mReplayLastStart - p + kFreezeRingLen) & (kFreezeRingLen - 1)) / kHop) + 1;
  mReplayFrame = 0;
  mReplayMode = mode;
  switch (mode) {
    case kModeVQT: mVQT.ResetRuntimeState(); break;
    case kModePBT: mPBT.ResetRuntimeState(); break;
    case kModeRTA: mRTA.ResetRuntimeState(); break;
    default: mSpectrum.ResetRuntimeState(); break;
  }
  SendResetToPad();
  const int scopeDummy = 0;
  SendControlMsgFromDelegate(kCtrlTagScope, StereoFieldControl::kMsgTagReset, sizeof(int), &scopeDummy);
}

// 每 OnIdle 泵送一批回放帧，走实时同款 kUpdateMessage 通道
void ORMAnalyzer::PumpFreezeReplay() {
  if (mReplayMode < 0)
    return;
  constexpr int kHop = 1024;
  const int mask = kFreezeRingLen - 1;
  using FPkt = std::array<float, 8192>;
  ISenderData<3, FPkt> d;
  d.ctrlTag = kCtrlTagPad;
  d.nChans = 3;
  d.chanOffset = 0;
  const int end = std::min(mReplayFrame + kReplayFramesPerTick, mReplayNFrames);
  for (; mReplayFrame < end; ++mReplayFrame) {
    // 帧序 0=最旧 -> nFrames-1=最新
    const int start = (mReplayLastStart - (mReplayNFrames - 1 - mReplayFrame) * kHop) & mask;
    for (int c = 0; c < 3; ++c) {
      const float *src = mFreezeRing[c].data();
      float *dst = d.vals[c].data();
      if (start + kHop <= kFreezeRingLen) {
        std::memcpy(dst, src + start, kHop * sizeof(float));
      } else {
        const int n1 = kFreezeRingLen - start;
        std::memcpy(dst, src + start, n1 * sizeof(float));
        std::memcpy(dst + n1, src, (kHop - n1) * sizeof(float));
      }
    }
    switch (mReplayMode) {
      case kModeVQT: mVQT.PrepareFrameUI(d); break;
      case kModePBT: mPBT.PrepareFrameUI(d); break;
      case kModeRTA: mRTA.PrepareFrameUI(d); break;
      default: mSpectrum.PrepareFrameUI(d); break;
    }
    SendControlMsgFromDelegate(kCtrlTagPad, ISender<>::kUpdateMessage,
                               sizeof(ISenderData<3, FPkt>), &d);
    // 同帧喂声像面板
    StereoScope<>::Data sd;
    sd.ctrlTag = kCtrlTagScope;
    sd.nChans = 2;
    sd.chanOffset = 0;
    std::memcpy(sd.vals[0].data(), d.vals[0].data(), kHop * sizeof(float));
    std::memcpy(sd.vals[1].data(), d.vals[1].data(), kHop * sizeof(float));
    SendControlMsgFromDelegate(kCtrlTagScope, ISender<>::kUpdateMessage, sizeof(StereoScope<>::Data), &sd);
  }
  if (mReplayFrame >= mReplayNFrames)
    mReplayMode = -1;
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

  if ((int)data.values.size() != kNumParams) {
    err = "Parameter count mismatch (expected " + std::to_string(kNumParams) + ")";
    return;
  }

  ParamSnapshot s{};
  std::copy(data.values.begin(), data.values.end(), s.begin());

  PushUndo();
  ApplySnapshot(s);
  err.clear();
}

// 示波器声道开关：把 L/R/M 三个按钮的按下状态合成掩码下发给示波器
void ORMAnalyzer::SyncScopeChanMask() {
  if (!mOscilloscopeCtrl)
    return;
  int mask = 0;
  if (mScopeBtnL && mScopeBtnL->GetValue() > 0.5)
    mask |= OscilloscopeControl::kChanBitL;
  if (mScopeBtnR && mScopeBtnR->GetValue() > 0.5)
    mask |= OscilloscopeControl::kChanBitR;
  if (mScopeBtnM && mScopeBtnM->GetValue() > 0.5)
    mask |= OscilloscopeControl::kChanBitM;
  mOscilloscopeCtrl->SetChanMask(mask);
}

void ORMAnalyzer::ApplyLanguage() {
  for (auto &binding : mTextBindings)
    if (binding.second)
      binding.second(orm::Tr(binding.first, orm::UILang()));
  if (mResBtn) {
    mResBtn->SetLabels({orm::Tr(orm::kTxtLfLow, orm::UILang()),
                        orm::Tr(orm::kTxtLfMid, orm::UILang()),
                        orm::Tr(orm::kTxtLfHigh, orm::UILang())});
  }
  if (mRtaOctBtn) {
    mRtaOctBtn->SetLabels({orm::Tr(orm::kTxtLfLow, orm::UILang()),
                           orm::Tr(orm::kTxtLfMid, orm::UILang()),
                           orm::Tr(orm::kTxtLfHigh, orm::UILang())});
  }
  if (mPbtLfResBtn) {
    mPbtLfResBtn->SetLabels({orm::Tr(orm::kTxtLfLow, orm::UILang()),
                             orm::Tr(orm::kTxtLfMid, orm::UILang()),
                             orm::Tr(orm::kTxtLfHigh, orm::UILang())});
  }
  if (mGammaBtn) {
    mGammaBtn->SetLabels({orm::Tr(orm::kTxtLfLow, orm::UILang()),
                          orm::Tr(orm::kTxtLfMid, orm::UILang()),
                          orm::Tr(orm::kTxtLfHigh, orm::UILang())});
  }
  if (mWindowBtn) {
    mWindowBtn->SetLabels({orm::Tr(orm::kTxtWinSharp, orm::UILang()),
                           orm::Tr(orm::kTxtWinClean, orm::UILang())});
  }
  if (mChanModeBtn) {
    mChanModeBtn->SetLabels({orm::Tr(orm::kTxtChanPWR, orm::UILang()),
                             orm::Tr(orm::kTxtChanLR, orm::UILang()),
                             orm::Tr(orm::kTxtChanSUM, orm::UILang())});
  }
  if (mReleaseModeBtn) {
    mReleaseModeBtn->SetLabels({orm::Tr(orm::kTxtRelLog, orm::UILang()),
                                orm::Tr(orm::kTxtRelLin, orm::UILang())});
  }
  if (mSpeedBtn) {
    mSpeedBtn->SetLabels({orm::Tr(orm::kTxtSpeedMin, orm::UILang()),
                          orm::Tr(orm::kTxtSpeedSlow, orm::UILang()),
                          orm::Tr(orm::kTxtSpeedMed, orm::UILang()),
                          orm::Tr(orm::kTxtSpeedFast, orm::UILang()),
                          orm::Tr(orm::kTxtSpeedMax, orm::UILang())});
  }
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
    // 通道色随色相/饱和度变化，同步示波器声道开关按钮的按下色
    IColor cL, cR, cM;
    GetChannelColors(cL, cR, cM);
    if (mScopeBtnL)
      mScopeBtnL->SetColor(cL);
    if (mScopeBtnR)
      mScopeBtnR->SetColor(cR);
    if (mScopeBtnM)
      mScopeBtnM->SetColor(cM);
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
  s.holdOn = GetParam(kLevelHoldOn)->Value() > 0.5 ? 1 : 0;
#if ORM_ENABLE_TEST_GEN
  s.genType = mGenType.load(std::memory_order_relaxed);
  s.genFreq = (double)mGenFreq.load(std::memory_order_relaxed);
  s.genLevel = (double)mGenLevel.load(std::memory_order_relaxed);
  s.genHold = mGenHold.load(std::memory_order_relaxed) ? 1 : 0;
  s.genToOutput = mGenToOutput.load(std::memory_order_relaxed) ? 1 : 0;
#endif
  SaveSettings(s);
}
