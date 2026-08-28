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
#include <mach/mach.h>
#elif defined(OS_WIN)
#include <windows.h>
#elif defined(__linux__)
#include <ctime>
#endif

// 线程 CPU 时间 (纳秒): 只累计本线程实际执行时间, 排除被抢占/调度延迟。
// CPU 占用测量用它替代墙钟, 避免系统瞬时负载 (其他进程抢占) 导致读数虚高。
// 非 mac/win/linux 平台回退墙钟 (等效旧行为)。
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

// 手势撤销的时间窗: 两次 UI 改动间隔超过该值时, 下一次改动前推一次撤销快照
static constexpr double kGestureGapSec = 0.4;

// FFT 档位 overlap 规则: hop 恒 1024 (2048/2, 4096/4, 8192/8), 与其余引擎 kHop 一致 ——
// 四引擎帧进给同格, pad 平滑时间常数跨档位同源, 冻结回放帧格对齐无需按档位分支
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

ORMAnalyzer::ORMAnalyzer(const InstanceInfo &info) : Plugin(info, MakeConfig(kNumParams, 1)) {
  // 读取全局 UI 偏好设置 (语言与主题)
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
  // 初始化参数（默认值、范围与步长）
  GetParam(kRelease)->InitDouble("Release", 0.2, 0.05, 0.5, 0.01, "s");
  GetParam(kRange)->InitInt("Range", 1, 0, 2, ""); // 档位索引: 0=80, 1=100, 2=120 (刻度底部 dB), 默认 100
  GetParam(kAttack)->InitDouble("Attack", 0.05, 0.001, 0.1, 0.001, "s");
  GetParam(kRes)->InitInt("Res", 1, 0, kNumResOptions - 1, ""); // 默认 MID (4096)
  GetParam(kLfRes)->InitInt("LfRes", 0, 0, kNumLfResOptions - 1, "");
  // kBpo 槽位已废弃 (MR-FFT 固定 BPO=24, 滑块已移除), 保留下标兼容旧状态文件, 不再初始化参数
  GetParam(kMode)->InitInt("Mode", kModeFFT, 0, kNumModes - 1, "");
  GetParam(kChannelMode)->InitInt("ChanMode", kChanModeLR, 0, kNumChanModes - 1, "");
  GetParam(kLevelMode)->InitInt("LevelMode", kLevelModeDBTP, 0, kNumLevelModes - 1, "");
  GetParam(kLevelHold)->InitInt("LevelHold", 1, 0, kNumHoldTimeOptions - 1, ""); // 档位: 0=0.5s 1=2s 2=KEEP
  GetParam(kLevelHoldOn)->InitBool("LevelHoldOn", false); // 默认关闭; 用户开关状态持久化于全局设置文件
  if (s.holdOn >= 0)
    GetParam(kLevelHoldOn)->Set(s.holdOn > 0 ? 1.0 : 0.0); // 恢复用户上次的保持开关状态
  // kPazAlgo 槽位已废弃 (PAZ 固定滤波器组算法), 保留下标兼容旧状态文件, 不再初始化参数
  GetParam(kFreeze)->InitBool("Freeze", false); // 0=实时, 1=FREEZE 定格
  GetParam(kPyramidDecim)->InitInt("Pyramid", 0, 0, 1, ""); // VQT 金字塔档位: 0=LIN 1=MIN
  // 频谱斜率档位 (各引擎独立保存; 默认档 1: STFT = 3 dB/oct, 逐 band 引擎 = 0 dB/oct)
  GetParam(kSlopeFFT)->InitInt("SlopeFFT", 1, 0, kNumSlopeOptions - 1, "");
  GetParam(kSlopeVQT)->InitInt("SlopeVQT", 1, 0, kNumSlopeOptions - 1, "");
  GetParam(kSlopePAZ)->InitInt("SlopePAZ", 1, 0, kNumSlopeOptions - 1, "");
  GetParam(kSlopeMRFFT)->InitInt("SlopeMRFFT", 1, 0, kNumSlopeOptions - 1, "");
  // 窗函数档位 STFT 与 VQT 各自独立 (kFFTWindow / kWindowVQT), 由同一按钮按模式改绑
  GetParam(kFFTWindow)->InitInt("WindowFFT", kFFTWindowHann, 0, kNumFFTWindows - 1, "");
  GetParam(kWindowVQT)->InitInt("WindowVQT", kFFTWindowHann, 0, kNumFFTWindows - 1, "");
  GetParam(kPazKernel)->InitInt("PazKernel", 4, 0, kNumPazKernelLenOptions - 1, ""); // 默认 k=5.0 (隔离悬崖上最后一档)

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
    IVStyle toggleStyle = btnStyle; // 反色开关 (HOLD): 隐藏标签/值, 绘制全由控件自绘接管
    toggleStyle.showLabel = false;
    toggleStyle.showValue = false;

    mTextBindings.clear();
    mTooltipBindings.clear();
    auto bindText = [this](int id, std::function<void(const char *)> apply) {
      mTextBindings.push_back({id, std::move(apply)});
    };
    auto bindTip = [this](IControl *c, int id) { mTooltipBindings.push_back({c, id}); };

    // 右栏锚定窗口右下 (窗口 960x720): 右缘 kPanelR=940 距右 20, 底部标题区到底 36。
    // 按钮无绘制内缩 (mRECT = 实际显示尺寸), 并排按钮间用 kBtnGap 补偿原内缩间距。
    constexpr float kCol1X = 784.f;
    constexpr float kBtnW = 76.f;  // 按钮显示宽度
    constexpr float kBtnGap = 4.f; // 并排按钮间隙 (替代原 BLOCK_GAP 内缩间距)
    constexpr float kBtnH = 30.f;
    constexpr float kCol2X = kCol1X + kBtnW + kBtnGap; // 右列按钮左缘
    constexpr float kPanelR = kCol2X + kBtnW;

    // 顶部三按钮 (LR / FFT·VQT·PAZ / RES) 统一尺寸: 宽 62 高 26, 容纳 HIGH/PWR 文字 + 基础内边距
    constexpr float kTopBtnW = 62.f;
    constexpr float kTopBtnH = 26.f;
    constexpr float kTopBtnY = 22.f;
    constexpr float kTopBtnGap = 6.f; // LR 与 FFT 之间留空隙, FFT 与 RES 紧贴

    // 三通道色块图例 (L / R / M, 颜色跟随主题) + 电平表读数, 与频谱图形区左缘对齐
    pGraphics->AttachControl(new ChannelLegendControl(IRECT(20, 32, 668, 54)), kCtrlTagLegend);

    // 声道显示模式循环按钮 (三态: LR / PWR / SUM).
    // L/R 样式: 左半 L 色右半 R 色; PWR/SUM 样式: 整块 M 色 (Merge 色) + 居中标签。
    mChanModeBtn = new FlatCycleButton(IRECT(20.f, kTopBtnY, 20.f + kTopBtnW, kTopBtnY + kTopBtnH),
                                       kChannelMode, {"L/R", "PWR", "SUM"}, btnStyle, true);
    pGraphics->AttachControl(mChanModeBtn);
    bindTip(mChanModeBtn, orm::kTxtTipChanMode);

    // 分析引擎切换按钮 (STFT / VQT / PAZ / MR-FFT) — LR 右侧, 留 kTopBtnGap 空隙
    constexpr float kModeX = 20.f + kTopBtnW + kTopBtnGap;
    mModeBtn = new FlatCycleButton(IRECT(kModeX, kTopBtnY, kModeX + kTopBtnW, kTopBtnY + kTopBtnH),
                                   kMode, {"STFT", "VQT", "PAZ", "MR-FFT"}, btnStyle);
    pGraphics->AttachControl(mModeBtn);
    bindTip(mModeBtn, orm::kTxtTipMode);

    // FFT 分辨率循环按钮 (LOW/MID/HIGH = 2048/4096/8192) — 紧贴 Mode 按钮右侧, 仅 FFT 模式可见
    constexpr float kResX = kModeX + kTopBtnW;
    mResBtn = new FlatCycleButton(IRECT(kResX, kTopBtnY, kResX + kTopBtnW, kTopBtnY + kTopBtnH), kRes,
                                  {"LOW", "MID", "HIGH"}, btnStyle);
    pGraphics->AttachControl(mResBtn);
    bindTip(mResBtn, orm::kTxtTipRes);

    // PAZ 低频分辨率循环按钮 (40/20/10 Hz) — 同位置, 仅 PAZ 模式可见
    mPazLfResBtn = new FlatCycleButton(IRECT(kResX, kTopBtnY, kResX + kTopBtnW, kTopBtnY + kTopBtnH), kLfRes,
                                       {"40Hz", "20Hz", "10Hz"}, btnStyle);
    pGraphics->AttachControl(mPazLfResBtn);
    bindTip(mPazLfResBtn, orm::kTxtTipLfRes);
    mPazLfResBtn->Hide(true);

    // VQT 金字塔算法循环按钮 (LIN / MIN) — 与 STFT 分辨率/PAZ 低频档同槽位, 按模式互斥可见
    mPyramidBtn = new FlatCycleButton(IRECT(kResX, kTopBtnY, kResX + kTopBtnW, kTopBtnY + kTopBtnH),
                                      kPyramidDecim, {"LIN", "MIN"}, btnStyle);
    pGraphics->AttachControl(mPyramidBtn);
    bindTip(mPyramidBtn, orm::kTxtTipPyramid);
    mPyramidBtn->Hide(true);

    // 窗函数循环按钮 (SHARP/CLEAN) — RES 分辨率按钮右侧 (即原 VQT 金字塔按钮位 B1 的槽位,
    // 尺寸 62x26 同 PWR/STFT, 间距同 PWR→STFT 的 kTopBtnGap)。STFT 与 VQT 各自独立保存
    // 档位 (kFFTWindow / kWindowVQT), 按当前模式改绑参数 (见 OnIdle 模式块)。
    constexpr float kWinX = kResX + kTopBtnW + kTopBtnGap;
    const int initMode = (int)GetParam(kMode)->Value();
    mWindowBtn = new FlatCycleButton(IRECT(kWinX, kTopBtnY, kWinX + kTopBtnW, kTopBtnY + kTopBtnH),
                                     initMode == kModeFFT ? kFFTWindow : kWindowVQT, {"SHARP", "CLEAN"}, btnStyle);
    pGraphics->AttachControl(mWindowBtn);
    bindTip(mWindowBtn, orm::kTxtTipWindow);
    mWindowBtn->Hide(initMode != kModeFFT && initMode != kModeVQT);

    // PAZ 解调核长系数循环按钮 (K3.0~K8.0) — 窗函数按钮槽位 (PAZ 模式下该槽空闲), 仅 PAZ 可见。
    // 核长系数 = T·bw: 越小延迟越低/带间读数越平, 越大邻带隔离越深; 用于与原版 PAZ 对比调校。
    mPazKernelBtn = new FlatCycleButton(IRECT(kWinX, kTopBtnY, kWinX + kTopBtnW, kTopBtnY + kTopBtnH),
                                        kPazKernel,
                                        {"3.0", "3.5", "4.0", "4.5", "5.0", "5.5", "6.0", "7.0", "8.0"},
                                        btnStyle);
    pGraphics->AttachControl(mPazKernelBtn);
    mPazKernelBtn->Hide(initMode != kModePAZ);

    // 主频谱绘制区域
    mSpectrumPad = new SpectrumPad(IRECT(20, 58, 668, 328));
    pGraphics->AttachControl(mSpectrumPad, kCtrlTagPad);

    // 动态范围循环按钮: 位于频谱图底部右缘 (电平表竖条左侧), 顶替最底部刻度标签
    // (DrawDbGrid 跳过底部一条的文字)。右下角与频谱图右下对齐不留缝, 文字样式/位置
    // 与刻度文字完全一致 (刻度样式), 仅多一个背景方块。点击循环切换 80/100/120 dB。
    // attach 在 pad 之后 → 覆盖于频谱之上, 命中测试优先。
    constexpr float kRangeBtnW = 38.f; // 收紧方块: 仅比文字 (-120 @14px ≈ 31px) 多出少量左右 padding
    constexpr float kRangeBtnH = 19.f; // 贴住文字行高 (14px 字 ≈ 17px 高)
    const float plotR = 668.f - 2.f * kGainBarW; // 频谱图形区右缘 (与 pad 内几何一致)
    const float plotB = 328.f;
    mRangeBtn = new FlatCycleButton(IRECT(plotR - kRangeBtnW, plotB - kRangeBtnH, plotR, plotB), kRange,
                                    {"-80", "-100", "-120"}, btnStyle);
    pGraphics->AttachControl(mRangeBtn);
    mRangeBtn->SetScaleLabelStyle(true);
    bindTip(mRangeBtn, orm::kTxtTipRange);

    // CPU 占用率显示
    mCpuMeter = new CpuMeterControl(IRECT(kCol1X, 30, kPanelR, 60));
    pGraphics->AttachControl(mCpuMeter, kCtrlTagCpu);

    // 上升响应时间滑块 (s): 原 BPO 滑块已删除, 上移填补空位
    mAttackSlider =
        new ORMSlider(IRECT(kCol1X, 69, kPanelR, 111), kAttack, "ATTACK", style, EDirection::Horizontal);
    pGraphics->AttachControl(mAttackSlider);
    bindText(orm::kTxtAttack, [this](const char *s) { mAttackSlider->SetHeaderLabel(s); });
    bindTip(mAttackSlider, orm::kTxtTipAttack);

    // 释放衰减时间滑块 (s)
    mReleaseSlider =
        new ORMSlider(IRECT(kCol1X, 120, kPanelR, 162), kRelease, "RELEASE", style, EDirection::Horizontal);
    pGraphics->AttachControl(mReleaseSlider);
    bindText(orm::kTxtRelease, [this](const char *s) { mReleaseSlider->SetHeaderLabel(s); });
    bindTip(mReleaseSlider, orm::kTxtTipRelease);

    // 频谱斜率循环按钮: 位于右栏 UNDO 上方整行 (与 HOLD 时长按钮同宽 kPanelR-kCol1X, 高 kBtnH,
    // 行距与下方按钮行一致)。标签带单位: FFT 0/3/4.5 dB/Oct, 其余 -3/0/1.5 dB/Oct;
    // 各引擎独立保存档位, 切引擎时改绑参数并换标签 (见 OnIdle 模式块)。
    // (BPO 滑块已删除, 下方整堆上移 51px 填补空位)
    constexpr float kSlopeBtnY = 183.f; // 与下方 UNDO 行 (y=222) 保持 39px 行距
    mSlopeBtn = new FlatCycleButton(IRECT(kCol1X, kSlopeBtnY, kPanelR, kSlopeBtnY + kBtnH), kSlopeFFT,
                                    {"0 dB/Oct", "3 dB/Oct", "4.5 dB/Oct"}, btnStyle);
    pGraphics->AttachControl(mSlopeBtn);
    mSlopeBtn->SetParamIdx(kSlopeFFT + (int)GetParam(kMode)->Value());
    bindTip(mSlopeBtn, orm::kTxtTipSlope);

    IVButtonControl *undoBtn =
        MakeMomentary(IRECT(kCol1X, 222, kCol1X + kBtnW, 252), [this](IControl *) { Undo(); }, "UNDO", btnStyle);
    pGraphics->AttachControl(undoBtn);
    bindText(orm::kTxtUndo, [undoBtn](const char *s) {
      undoBtn->SetLabelStr(s);
      undoBtn->SetDirty(false);
    });
    IVButtonControl *redoBtn =
        MakeMomentary(IRECT(kCol2X, 222, kPanelR, 252), [this](IControl *) { Redo(); }, "REDO", btnStyle);
    pGraphics->AttachControl(redoBtn);
    bindText(orm::kTxtRedo, [redoBtn](const char *s) {
      redoBtn->SetLabelStr(s);
      redoBtn->SetDirty(false);
    });
    IVButtonControl *saveBtn =
        MakeMomentary(IRECT(kCol1X, 261, kCol1X + kBtnW, 291), [this](IControl *) { SaveFile(); }, "SAVE", btnStyle);
    pGraphics->AttachControl(saveBtn);
    bindText(orm::kTxtSave, [saveBtn](const char *s) {
      saveBtn->SetLabelStr(s);
      saveBtn->SetDirty(false);
    });
    IVButtonControl *loadBtn =
        MakeMomentary(IRECT(kCol2X, 261, kPanelR, 291), [this](IControl *) { LoadFile(); }, "LOAD", btnStyle);
    pGraphics->AttachControl(loadBtn);
    bindText(orm::kTxtLoad, [loadBtn](const char *s) {
      loadBtn->SetLabelStr(s);
      loadBtn->SetDirty(false);
    });

    // 电平表模式循环按钮 (dBTP -> dBFS -> VU): 移入电平条内部底部, 显示覆盖在两条电平条之上,
    // 宽度 = 两条电平条总宽 (2 × kGainBarW), 高度与左侧 Range 循环按钮一致 (kRangeBtnH)。
    // 本按钮 attach 在 SpectrumPad 之后 → 绘制于电平条上方且命中测试优先;
    // 底座窄, 缩小文字字号以容纳 "dBTP"/"dBFS"。
    mLevelModeBtn =
        new FlatCycleButton(IRECT(plotR, plotB - kRangeBtnH, plotR + 2.f * kGainBarW, plotB), kLevelMode,
                            {"dBTP", "dBFS", "VU"}, btnStyle);
    mLevelModeBtn->SetTextSize(12.f);
    pGraphics->AttachControl(mLevelModeBtn);
    bindTip(mLevelModeBtn, orm::kTxtTipLevelMode);

    // 电平表 RESET (清除峰值保持与过载锁存; 频谱 hold 曲线同步清空, 与电平表保持联动)
    mLevelResetBtn =
        MakeMomentary(IRECT(kCol2X, 300, kPanelR, 330), [this](IControl *) {
          mLevelResetFlag.store(true);
          if (mSpectrumPad)
            mSpectrumPad->ClearPeakHold();
        }, "RESET", btnStyle);
    pGraphics->AttachControl(mLevelResetBtn);
    bindText(orm::kTxtReset, [this](const char *s) {
      if (mLevelResetBtn) {
        mLevelResetBtn->SetLabelStr(s);
        mLevelResetBtn->SetDirty(false);
      }
    });
    bindTip(mLevelResetBtn, orm::kTxtTipReset);

    // Freeze 冻结开关 (RESET 左侧空位): 反色开关样式, 与 HOLD 开关一致——始终显示冻结文字
    // (英文 FREEZE / 中文 冻结, 随界面语言), 开启时按钮底色变黑 + 文字反白。
    // 冻结时画面完全定格 (UI 停止消费引擎数据), 切换引擎/PAZ 算法时用冻结时刻的输入缓冲
    // 在新算法下重算并继续定格, 解冻后从定格画面续接实时。
    mFreezeBtn = new FlatToggleControl(IRECT(kCol1X, 300, kCol1X + kBtnW, 330), kFreeze, " ", toggleStyle,
                                       "FREEZE", "FREEZE");
    pGraphics->AttachControl(mFreezeBtn);
    bindText(orm::kTxtFreeze, [this](const char *s) {
      if (mFreezeBtn) {
        mFreezeBtn->SetOnText(s);
        mFreezeBtn->SetOffText(s);
      }
    });
    bindTip(mFreezeBtn, orm::kTxtTipFreeze);

    // 峰值保持开关 + 时长循环按钮 (替代原 HOLD 滑块): 开关为 BandPass LINK 同款反色开关,
    // 时长按钮移至 HOLD 右侧空位 (尺寸缩小为 kBtnW), 点击循环 0.5s / 2s / KEEP (持久)。
    // 两者同时控制电平表 hold 亮线与频谱 hold 曲线; RESET 按钮清除已积累的保持。
    mLevelHoldBtn = new FlatToggleControl(IRECT(kCol1X, 339, kCol1X + kBtnW, 369), kLevelHoldOn, " ", toggleStyle,
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
        new FlatCycleButton(IRECT(kCol2X, 339, kPanelR, 369), kLevelHold, {"0.5s", "2s", "KEEP"}, btnStyle);
    pGraphics->AttachControl(mLevelHoldTimeBtn);
    bindTip(mLevelHoldTimeBtn, orm::kTxtTipLevelHoldTime);

    // 底部标题栏：ORM 标识、设置齿轮与版本号
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
    // 内置测试信号发生器 (开发者工具): 只改原子标量, 音频线程下一 block 生效。
    // 频率/电平拖动不逐像素写盘 —— 打 pending 标记, 由 OnIdle 稳定 0.5 s 后落盘。
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
      // 换种子: 每次取一个新的非零值, 音频线程下一 block 生效
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
    ApplyLanguage();

    // UI 控件已全部就绪: 立即同步一次频谱配置并更新去重标记。
    // App 模式下 OnIdle 首帧可能早于控件 attach 运行, SendControlMsgFromDelegate
    // 对不存在的控件会静默丢弃消息 (不排队), 频谱将停留在默认范围 (如 -90)
    // 直到参数变化才重发——这里保证 UI 打开即同步, VST/App 行为一致。
    SendSpectrumConfig();
    mSentSampleRate = GetSampleRate();
    mSentFFTSize = CurrentFFTSize();
    mSentRelease = GetParam(kRelease)->Value();
    mSentRange = (double)std::clamp(std::lround(GetParam(kRange)->Value()), 0L, 2L);
    mSentAttack = GetParam(kAttack)->Value();
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
  const uint64_t cpuT0 = ThreadCpuNs(); // 线程实际 CPU 时间基线 (不含被抢占)
  nFrames = std::min(nFrames, kMaxBlock);

  const int nOuts = NOutChansConnected();
  const int nIns = NInChansConnected();

  // 音频直通（分析器不改变音频信号）
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

  // 若 UI 未打开 (或处于后台休眠状态), 挂起所有高开销分析 (FFT/VQT/PAZ/LevelMeter), 耗时直接归零
  if (!mUIOpen.load(std::memory_order_relaxed)) {
    const double processMs = (double)(ThreadCpuNs() - cpuT0) / 1e6;
    const double blockMs = (double)nFrames / std::max(GetSampleRate(), 1.0) * 1000.0;
    if (blockMs > 0.0)
      mCpuAudio += (processMs / blockMs - mCpuAudio) * 0.1;
    return;
  }

  // 采集输入数据到当前分析引擎
  const int mode = (int)GetParam(kMode)->Value();
  const bool frozen = GetParam(kFreeze)->Value() > 0.5; // Freeze 激活: 挂起采集, 画面定格

#if ORM_ENABLE_TEST_GEN
  // ── 内置测试信号发生器 (开发者工具) ──────────────────────────────────
  // 开启时用内部生成的已知信号替换宿主输入:
  //   · 固定 seed → 逐样本可复现;
  //   · 与引擎共用同一 nFrames → 脉冲/扫频的时间对齐是样本级精确的;
  //   · 正弦可吸附到 bin 中心或 bin 之间 → scalloping loss 成为可测量的量;
  //   · 生成的信号同样写进冻结环形缓冲 —— 于是"冻结 + 切引擎"就是拿同一段
  //     信号跑不同算法, 这是外部音源给不了的公平对比。
  // 冻结 + HOLD 时停止推进样本索引, 保证重算的始终是同一段信号。
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
    gc.fftSize = mSpectrum.GetFFTSize(); // bin 吸附网格跟随 FFT 引擎实际尺寸
    mTestGen.SetConfig(gc);
    const bool advance = !(frozen && mGenHold.load(std::memory_order_relaxed));
    mTestGen.Fill(mSpecInL.data(), mSpecInR.data(), nFrames, GetSampleRate(), advance);
    for (int s = 0; s < nFrames; ++s)
      mSpecInM[s] = (mSpecInL[s] + mSpecInR[s]) * 0.7071067811865475;
    if (mGenToOutput.load(std::memory_order_relaxed) && nOuts >= 1) {
      // 输出路由 (默认关闭): 便于外录或与第三方分析器交叉验证。
      // 硬限幅 -1 dBFS —— 脉冲与白噪以满量程直送监听是危险的。
      constexpr sample kOutCeil = (sample)0.8912504381337891; // -1 dBFS
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
    // 输入已由发生器填充 (mSpecInL/R/M 就绪)
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
    // 冻结环形缓冲: 常驻记录最近 kFreezeRingLen 样本; freeze on 后停止写入, 即冻结时刻快照。
    // (UI 线程 Read + 音频线程 Write 由 kFreeze 参数 (原子) 协调: 写入先停, 读取后才开始,
    // 边缘至多混入冻结生效前最后一块输入, 无碍。)
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
    } else if (mode == kModePAZ) {
      mPAZ.ProcessBlock(spec, nFrames, kCtrlTagPad, 3);
      mEngineHopPhase[kModePAZ].store(mPAZ.HopPhase(), std::memory_order_relaxed);
    } else if (mode == kModeMRFFT) {
      mMRFFT.ProcessBlock(spec, nFrames, kCtrlTagPad, 3);
      mEngineHopPhase[kModeMRFFT].store(mMRFFT.HopPhase(), std::memory_order_relaxed);
    } else {
      mSpectrum.ProcessBlock(spec, nFrames, kCtrlTagPad, 3);
      mEngineHopPhase[kModeFFT].store(mSpectrum.HopPhase(), std::memory_order_relaxed);
    }
  }

  // 电平表测量 (冻结中挂起: 保持最后快照, 画面随频谱一起定格; 重置请求仍被消费, 避免解冻后误触发)
  if (frozen) {
    if (mLevelResetHoldFlag.exchange(false, std::memory_order_relaxed))
      mLevelMeter.ResetHold();
    if (mLevelResetFlag.exchange(false))
      mLevelMeter.ResetHoldOver();
  } else {
    const double newSR = mLevelSetSR.exchange(-1.0, std::memory_order_relaxed);
    if (newSR > 0.0)
      mLevelMeter.SetSampleRate(newSR);
    if (mLevelResetHoldFlag.exchange(false, std::memory_order_relaxed))
      mLevelMeter.ResetHold();
    if (mLevelResetFlag.exchange(false))
      mLevelMeter.ResetHoldOver();
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
      mHoldL.store(s.holdDbL, std::memory_order_relaxed);
      mHoldR.store(s.holdDbR, std::memory_order_relaxed);
      mHoldSec.store(s.holdSec, std::memory_order_relaxed);
      mOverL.store(s.overL, std::memory_order_relaxed);
      mOverR.store(s.overR, std::memory_order_relaxed);
    }
  }

  // 统计音频线程耗时（一阶平滑）: 用线程 CPU 时间差值, 抢占/调度延迟不纳入
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
  mVQT.SetGamma(kVQTGammaHz); // VQT 固定 γ = HIGH (5 Hz)
  mVQT.SetBpo(kVQTBpo);       // VQT 固定 BPO = 24
  mPAZ.SetSampleRate(GetSampleRate());
  mPAZ.SetLfWidth(CurrentPazLfRes());
  mMRFFT.SetSampleRate(GetSampleRate());
  mMRFFT.SetBpo(kMRFFTBpo); // MR-FFT 固定 BPO = 24 (滑块已移除)
  mLevelSetSR.store(GetSampleRate(), std::memory_order_relaxed); // 音频线程下一 block 执行 SetSampleRate+Reset
  mPeakL.store(0.f, std::memory_order_relaxed);
  mPeakR.store(0.f, std::memory_order_relaxed);
#if ORM_ENABLE_TEST_GEN
  mTestGen.Restart(); // 采样率/缓冲变化后信号从头开始, 保证可复现
#endif
}

void ORMAnalyzer::SendSpectrumConfig() {
  const double sr = GetSampleRate();
  // 下发引擎实际生效的 FFT 尺寸 (而非参数名义值): 若将来钳制逻辑变化,
  // pad 的 bin 数与平滑更新周期必须始终跟随引擎真实尺寸, 否则高频段空白 + 时间常数偏差。
  const int fftSize = mSpectrum.GetFFTSize();
  const float release = (float)GetParam(kRelease)->Value();
  const float range = CurrentRangeDb();
  const float attack = (float)GetParam(kAttack)->Value();
  const float slopeDb = (float)EffectiveSlopeDb(); // 当前模式生效斜率 (档值随模式)
  const int chanTri = (int)GetParam(kChannelMode)->Value();
  const int chanMode = (chanTri == kChanModeLR) ? 0 : 1;
  const int mergeAlgo = (chanTri == kChanModeSUM) ? 1 : 0;
  SendControlMsgFromDelegate(kCtrlTagPad, SpectrumPad::kMsgTagSampleRate, sizeof(double), &sr);
  SendControlMsgFromDelegate(kCtrlTagPad, SpectrumPad::kMsgTagFFTSize, sizeof(int), &fftSize);
  SendControlMsgFromDelegate(kCtrlTagPad, SpectrumPad::kMsgTagRelease, sizeof(float), &release);
  SendControlMsgFromDelegate(kCtrlTagPad, SpectrumPad::kMsgTagRange, sizeof(float), &range);
  SendControlMsgFromDelegate(kCtrlTagPad, SpectrumPad::kMsgTagAttack, sizeof(float), &attack);
  SendControlMsgFromDelegate(kCtrlTagPad, SpectrumPad::kMsgTagSlope, sizeof(float), &slopeDb);
  SendControlMsgFromDelegate(kCtrlTagPad, SpectrumPad::kMsgTagChanMode, sizeof(int), &chanMode);
  SendControlMsgFromDelegate(kCtrlTagPad, SpectrumPad::kMsgTagMergeAlgo, sizeof(int), &mergeAlgo);
  const int mode = (int)GetParam(kMode)->Value();
  SendControlMsgFromDelegate(kCtrlTagPad, SpectrumPad::kMsgTagMode, sizeof(int), &mode);
  if (mode == kModeVQT)
    SendVQTBandFreqs();
  else if (mode == kModePAZ)
    SendPAZBandFreqs();
  else if (mode == kModeMRFFT)
    SendMRFFTBandFreqs();
}

void ORMAnalyzer::SendVQTBandFreqs() {
  const auto &freqs = mVQT.BandFreqs();
  if (freqs.empty())
    return;
  std::vector<float> buf(freqs.begin(), freqs.end());
  SendControlMsgFromDelegate(kCtrlTagPad, SpectrumPad::kMsgTagVQTBands,
                             (int)(buf.size() * sizeof(float)), buf.data());
}

void ORMAnalyzer::SendPAZBandFreqs() {
  const auto &freqs = mPAZ.BandFreqs();
  if (freqs.empty())
    return;
  std::vector<float> buf(freqs.begin(), freqs.end());
  SendControlMsgFromDelegate(kCtrlTagPad, SpectrumPad::kMsgTagPAZBands,
                             (int)(buf.size() * sizeof(float)), buf.data());
}

void ORMAnalyzer::SendMRFFTBandFreqs() {
  const auto &freqs = mMRFFT.BandFreqs();
  if (freqs.empty())
    return;
  std::vector<float> buf(freqs.begin(), freqs.end());
  SendControlMsgFromDelegate(kCtrlTagPad, SpectrumPad::kMsgTagMRFFTBands,
                             (int)(buf.size() * sizeof(float)), buf.data());
}

void ORMAnalyzer::SendResetToPad() {
  const int dummy = 0;
  SendControlMsgFromDelegate(kCtrlTagPad, SpectrumPad::kMsgTagReset, sizeof(int), &dummy);
}

void ORMAnalyzer::OnParamChange(int paramIdx, EParamSource source, int sampleOffset) {
  // 冻结中抑制 SendResetToPad: 画面保持定格; 引擎配置 (PAZ 低频档等) 照常更新,
  // 解冻后由 OnIdle 防抖重发完整配置与 band 表。
  const bool frozen = GetParam(kFreeze)->Value() > 0.5;
  // 参数变化时更新分析引擎配置
  if (paramIdx == kRes)
    mSpectrum.SetFFTSizeAndOverlap(CurrentFFTSize(), FFTOverlapForSize(CurrentFFTSize()), CurrentFFTWindow());
  else if (paramIdx == kFFTWindow) {
    mSpectrum.SetWindowType(CurrentFFTWindow());
  } else if (paramIdx == kWindowVQT) {
    if (mVQT.SetWindowType(CurrentVQTWindow()) && !frozen)
      SendResetToPad();
  }
  else if (paramIdx == kLfRes) {
    if (GetParam(kMode)->Value() > 1.5 && GetParam(kMode)->Value() < 2.5) { // PAZ 模式
      if (mPAZ.SetLfWidth(CurrentPazLfRes()) && !frozen)
        SendResetToPad();
    }
  } else if (paramIdx == kPazKernel) {
    if (GetParam(kMode)->Value() > 1.5 && GetParam(kMode)->Value() < 2.5) { // PAZ 模式
      if (mPAZ.SetKernelLen(CurrentPazKernelLen()) && !frozen)
        SendResetToPad();
    }
  } else if (paramIdx == kPyramidDecim) {
    if (GetParam(kMode)->Value() < 1.5) { // VQT 模式
      if (mVQT.SetPyramidMode((int)std::lround(GetParam(kPyramidDecim)->Value())) && !frozen)
        SendResetToPad();
    }
  } else if (paramIdx == kLevelMode) {
    // 电平表模式切换: 清除峰值保持 (过载锁存保留, 直到手动 RESET); 由音频线程执行
    mLevelResetHoldFlag.store(true, std::memory_order_relaxed);
  }
}

void ORMAnalyzer::OnParamChangeUI(int paramIdx, EParamSource source) {
  if (source == EParamSource::kUI) {
    MaybePushGestureUndo();
    if (paramIdx == kLevelHoldOn)
      SaveSettingsToDisk(); // 用户切换保持开关: 写入全局设置文件, 重启后保持用户状态
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
  const uint64_t workT0 = ThreadCpuNs(); // UI 线程实际 CPU 时间基线 (不含被抢占)

  // 按需重建频带配置
  mVQT.CheckRebuild();
  mPAZ.CheckRebuild();
  mMRFFT.CheckRebuild();

  const bool frozen = GetParam(kFreeze)->Value() > 0.5; // Freeze 激活: 画面定格

  // 冻结档位快照同步 (冻结中已重算的档位索引)
  auto syncFreezeSnapshot = [this]() {
    mFreezeRes = (int)std::lround(GetParam(kRes)->Value());
    mFreezeWindowFFT = CurrentFFTWindow();
    mFreezeWindowVQT = CurrentVQTWindow();
    mFreezeLf = (int)std::lround(GetParam(kLfRes)->Value());
    mFreezePyramid = (int)std::lround(GetParam(kPyramidDecim)->Value());
    mFreezeKernel = (int)std::lround(GetParam(kPazKernel)->Value());
  };

  // 模式切换 (冻结中: 跳过 reset 不清屏, 改用冻结缓冲在新算法下重算后直显定格)
  const int mode = (int)GetParam(kMode)->Value();
  if (mode != mSentMode) {
    mSentMode = mode;
    if (mResBtn && mPazLfResBtn) {
      mResBtn->Hide(mode != kModeFFT);
      mPazLfResBtn->Hide(mode != kModePAZ);
      mPyramidBtn->Hide(mode != kModeVQT);
      if (mPazKernelBtn)
        mPazKernelBtn->Hide(mode != kModePAZ);
    }
    // 窗函数按钮: STFT 与 VQT 各自独立档位, 按模式改绑参数 (仅两引擎模式可见)
    if (mWindowBtn) {
      mWindowBtn->Hide(mode != kModeFFT && mode != kModeVQT);
      mWindowBtn->SetParamIdx(mode == kModeFFT ? kFFTWindow : kWindowVQT);
    }
    // 斜率按钮: 档位随引擎不同 (FFT 0/3/4.5, 其余 -3/0/1.5, 单位 dB/Oct);
    // 改绑当前引擎参数并换标签
    if (mSlopeBtn) {
      mSlopeBtn->SetParamIdx(kSlopeFFT + mode);
      if (mode == kModeFFT)
        mSlopeBtn->SetLabels({"0 dB/Oct", "3 dB/Oct", "4.5 dB/Oct"});
      else
        mSlopeBtn->SetLabels({"-3 dB/Oct", "0 dB/Oct", "1.5 dB/Oct"});
    }
    // 模式切换会改变生效斜率 (档值表不同), 与 mode 一起立即下发
    const float slopeDb = (float)EffectiveSlopeDb();
    mSentSlope = slopeDb;
    SendControlMsgFromDelegate(kCtrlTagPad, SpectrumPad::kMsgTagSlope, sizeof(float), &slopeDb);
    SendControlMsgFromDelegate(kCtrlTagPad, SpectrumPad::kMsgTagMode, sizeof(int), &mode);
    if (mode == kModeVQT)
      SendVQTBandFreqs();
    else if (mode == kModePAZ)
      SendPAZBandFreqs();
    else if (mode == kModeMRFFT)
      SendMRFFTBandFreqs();
    if (frozen) {
      // 冻结中: 用冻结输入快照在 (新) 引擎与档位下确定性回放, 画面随分 tick 泵送
      // 收敛到 "冻结音频 × 当前配置" 并继续定格。同步档位快照, 避免同 tick 重复触发。
      StartFreezeReplay();
      syncFreezeSnapshot();
    } else {
      SendResetToPad();
    }
  }

  // 声道显示模式
  const int chanTri = (int)GetParam(kChannelMode)->Value();
  mSpectrum.SetChannelMode(chanTri);
  mVQT.SetChannelMode(chanTri);
  mPAZ.SetChannelMode(chanTri);
  mMRFFT.SetChannelMode(chanTri);
  if (chanTri != mSentChanMode) {
    mSentChanMode = chanTri;
    const int chanMode = (chanTri == kChanModeLR) ? 0 : 1;
    const int mergeAlgo = (chanTri == kChanModeSUM) ? 1 : 0;
    SendControlMsgFromDelegate(kCtrlTagPad, SpectrumPad::kMsgTagChanMode, sizeof(int), &chanMode);
    SendControlMsgFromDelegate(kCtrlTagPad, SpectrumPad::kMsgTagMergeAlgo, sizeof(int), &mergeAlgo);
    if (frozen)
      StartFreezeReplay(); // 声道模式影响分析通道选择: 冻结画面按新声道回放
  }

  // 检查并下发参数变动 (冻结中照常: Range 等显示参数即时生效, band 表随档位变化重发;
  // SendResetToPad 已被冻结抑制, 画面不会被清空)
  {
    const double sr = GetSampleRate();
    const int fftSize = CurrentFFTSize();
    const int winFFT = CurrentFFTWindow();
    const int winVQT = CurrentVQTWindow();
    const double release = GetParam(kRelease)->Value();
    const int rangeIdx = (int)std::clamp(std::lround(GetParam(kRange)->Value()), 0L, 2L);
    if (GetParam(kRange)->Value() != (double)rangeIdx)
      SetParamFromEditor(kRange, (double)rangeIdx);
    const double range = (double)rangeIdx;
    const double attack = GetParam(kAttack)->Value();
    const double lfRes = GetParam(kLfRes)->Value();
    const double slope = EffectiveSlopeDb(); // 斜率档位变化时重发 (冻结中照常: 纯显示参数)
    if (sr != mSentSampleRate || fftSize != mSentFFTSize || winFFT != mSentWindowFFT || winVQT != mSentWindowVQT ||
        release != mSentRelease || range != mSentRange || attack != mSentAttack || lfRes != mSentLfRes ||
        slope != mSentSlope) {
      mSpectrum.SetWindowType(winFFT);
      mVQT.SetWindowType(winVQT);
      // 冻结中 attack/release (回放弹道) 或采样率/窗函数变化需重启回放, 保证确定性;
      // Range/斜率纯显示参数不参与计算, 不重启。窗函数按引擎各查各的档位。
      const bool restartReplay =
          frozen && (sr != mSentSampleRate || release != mSentRelease || attack != mSentAttack ||
                     (mode == kModeFFT && winFFT != mSentWindowFFT) || (mode == kModeVQT && winVQT != mSentWindowVQT));
      mSentSampleRate = sr;
      mSentFFTSize = fftSize;
      mSentWindowFFT = winFFT;
      mSentWindowVQT = winVQT;
      mSentRelease = release;
      mSentRange = range;
      mSentAttack = attack;
      mSentLfRes = lfRes;
      mSentSlope = slope;
      SendSpectrumConfig();
      if (restartReplay)
        StartFreezeReplay();
    }
  }

  // 冻结中: 同一算法内档位变化 (STFT 尺寸 / 窗函数 / VQT 金字塔 / PAZ LF)
  // → 引擎配置已在音频线程更新 (OnParamChange), band 表已由上方防抖重发 (先于回放帧),
  // 这里用冻结缓冲确定性回放, 画面 = 冻结音频 × 当前档位。刚冻结 (mFreezeOn 边沿) 只同步
  // 快照不回放: 画面保持按下瞬间的实时定格, 避免不必要的跳变与回放开销。
  if (frozen) {
    if (!mFreezeOn) {
      mFreezeOn = true;
      syncFreezeSnapshot();
    } else {
      const int resIdx = (int)std::lround(GetParam(kRes)->Value());
      const int winFFT = CurrentFFTWindow();
      const int winVQT = CurrentVQTWindow();
      const int lfIdx = (int)std::lround(GetParam(kLfRes)->Value());
      const int pyrIdx = (int)std::lround(GetParam(kPyramidDecim)->Value());
      const int kernIdx = (int)std::lround(GetParam(kPazKernel)->Value());
      const bool cfgChanged =
          (mode == kModeFFT && (resIdx != mFreezeRes || winFFT != mFreezeWindowFFT)) ||
          (mode == kModeVQT &&
           (lfIdx != mFreezeLf || pyrIdx != mFreezePyramid || winVQT != mFreezeWindowVQT)) ||
          (mode == kModePAZ && (lfIdx != mFreezeLf || kernIdx != mFreezeKernel));
      if (cfgChanged) {
        StartFreezeReplay();
      }
      mFreezeRes = resIdx;
      mFreezeWindowFFT = winFFT;
      mFreezeWindowVQT = winVQT;
      mFreezeLf = lfIdx;
      mFreezePyramid = pyrIdx;
      mFreezeKernel = kernIdx;
    }
    // 冻结回放泵送: 每 tick 回放一批帧 (kUpdateMessage → pad 实时平滑), 收敛后定格
    PumpFreezeReplay();
  } else {
    mFreezeOn = false;
    mReplayMode = -1; // 解冻中止未完成的回放, 引擎带部分预热历史续接实时 (仅分析侧)
  }

  // 分发激活引擎数据 (冻结中跳过: 画面定格; 引擎队列保持, 解冻后继续消费续接实时)
  if (!frozen) {
    if (mode == kModeVQT)
      mVQT.TransmitData(*this);
    else if (mode == kModePAZ)
      mPAZ.TransmitData(*this);
    else if (mode == kModeMRFFT)
      mMRFFT.TransmitData(*this);
    else
      mSpectrum.TransmitData(*this);
  }

  // 转发电平表数据给表头区 (LevelMeterUiData, 含模式/保持时长/过载锁存)
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
    d.holdL = mHoldL.load(std::memory_order_relaxed);
    d.holdR = mHoldR.load(std::memory_order_relaxed);
    d.holdSec = mHoldSec.load(std::memory_order_relaxed);
    d.mode = (int)GetParam(kLevelMode)->Value();
    d.overL = mOverL.load(std::memory_order_relaxed);
    d.overR = mOverR.load(std::memory_order_relaxed);
    SendControlMsgFromDelegate(kCtrlTagPad, SpectrumPad::kMsgTagLevelMeter, sizeof(d), &d);
    SendControlMsgFromDelegate(kCtrlTagLegend, ChannelLegendControl::kMsgTagLevelReadout, sizeof(d), &d);
  }

  // 统计 UI 线程耗时并计算综合 CPU 占用率: 工作量为线程 CPU 时间差值 (抢占不计), 分母为墙钟窗口
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
  // 发生器频率/电平拖动稳定 0.5 s 后落盘 (避免拖动过程中逐像素写文件)
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
    SaveSettingsToDisk(); // 关 UI 前补写未落盘的发生器设置
#endif
  mSpectrumPad = nullptr;
  mResBtn = nullptr;
  mPazLfResBtn = nullptr;
  mPazKernelBtn = nullptr;
  mRangeBtn = nullptr;
  mAttackSlider = nullptr;
  mReleaseSlider = nullptr;
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
  mSettingsPanel = nullptr;
  mTextBindings.clear();
  mTooltipBindings.clear();
  // 重开 UI 后控件是新的, 重置去重标记让下一次 OnIdle 重发完整配置与模式同步
  mSentSampleRate = 0.0;
  mSentFFTSize = 0;
  mSentWindowFFT = -1;
  mSentWindowVQT = -1;
  mSentRelease = -1.0;
  mSentMode = -1;
  mSentRange = -1.0;
  mSentAttack = -1.0;
  mSentLfRes = -1.0;
  mSentSlope = -1e9;
  mSentChanMode = -1;
  // 冻结档位快照复位: 重开 UI 后冻结画面与档位重算按新控件状态重新建立
  mFreezeOn = false;
  mFreezeRes = mFreezeWindowFFT = mFreezeWindowVQT = mFreezeLf = mFreezePyramid = -1;
  mReplayMode = -1;
}

// 冻结回放 (确定性): "冻结音频 × 当前算法/档位" 的谱由纯函数计算——
//   display(cfg) = ballistics( replay( ring, cfg ) )
// 启动时引擎运行态复位 (等价冷启动)、pad 平滑缓冲清零, 因此同样的 (环, 配置) 永远
// 得到同一画面: 冻结中切走再切回, 结果逐字节一致。帧格与实时对齐: 最新回放帧起点 =
// 环尾 - hop 相位 - hop, 即冻结瞬间实时显示的最后一帧; 从它向最旧方向铺满整圈
// (1<<18 环 = 255/256 帧)。帧经 kUpdateMessage 下发, pad 侧攻击/释放平滑照常生效
// (与实时同一弹道学、单一来源), 画面随回放逐 tick 收敛定格。
// 注: hop 恒 1024 由 FFT overlap 规则 (2048/2, 4096/4, 8192/8) 与各引擎 kHop 保证。
void ORMAnalyzer::StartFreezeReplay() {
  const int mode = (int)GetParam(kMode)->Value();
  constexpr int kHop = 1024;
  const int p = mFreezeRingPos.load(std::memory_order_relaxed); // 下一个写入位置 (= 最旧样本)
  // 冻结时该引擎输入侧 pending 的 b 个样本尚未成帧 (实时同样未显示), 故最新"已显示"
  // 帧终点在环尾前 b 个样本处, 起点再退一个 hop
  const int b = mEngineHopPhase[mode].load(std::memory_order_relaxed);
  mReplayLastStart = (p - b - kHop) & (kFreezeRingLen - 1);
  mReplayNFrames = (((mReplayLastStart - p + kFreezeRingLen) & (kFreezeRingLen - 1)) / kHop) + 1;
  mReplayFrame = 0;
  mReplayMode = mode;
  switch (mode) {
    case kModeVQT: mVQT.ResetRuntimeState(); break;
    case kModePAZ: mPAZ.ResetRuntimeState(); break;
    case kModeMRFFT: mMRFFT.ResetRuntimeState(); break;
    default: mSpectrum.ResetRuntimeState(); break;
  }
  // pad 平滑缓冲确定性清零: 回放从零收敛, 显示与切换历史无关
  SendResetToPad();
}

// 每 OnIdle 泵送一批回放帧 (kReplayFramesPerTick): 引擎逐帧分析后走实时同款
// kUpdateMessage 通道 (pad 攻击/释放平滑生效), 收敛完成后回放结束、画面定格。
void ORMAnalyzer::PumpFreezeReplay() {
  if (mReplayMode < 0)
    return;
  constexpr int kHop = 1024;
  const int mask = kFreezeRingLen - 1;
  using FPkt = std::array<float, 8192>; // 与四引擎 Data / pad TDataPacket 同构
  ISenderData<3, FPkt> d;
  d.ctrlTag = kCtrlTagPad;
  d.nChans = 3;
  d.chanOffset = 0;
  const int end = std::min(mReplayFrame + kReplayFramesPerTick, mReplayNFrames);
  for (; mReplayFrame < end; ++mReplayFrame) {
    // 帧序 0 = 最旧 → nFrames-1 = 最新 (实时定格帧)
    const int start = (mReplayLastStart - (mReplayNFrames - 1 - mReplayFrame) * kHop) & mask;
    for (int c = 0; c < 3; ++c) {
      const float *src = mFreezeRing[c].data();
      float *dst = d.vals[c].data();
      if (start + kHop <= kFreezeRingLen) {
        std::memcpy(dst, src + start, kHop * sizeof(float));
      } else {
        const int n1 = kFreezeRingLen - start; // 跨环尾部分两段拷贝
        std::memcpy(dst, src + start, n1 * sizeof(float));
        std::memcpy(dst + n1, src, (kHop - n1) * sizeof(float));
      }
    }
    switch (mReplayMode) {
      case kModeVQT: mVQT.PrepareFrameUI(d); break;
      case kModePAZ: mPAZ.PrepareFrameUI(d); break;
      case kModeMRFFT: mMRFFT.PrepareFrameUI(d); break;
      default: mSpectrum.PrepareFrameUI(d); break;
    }
    SendControlMsgFromDelegate(kCtrlTagPad, ISender<>::kUpdateMessage,
                               sizeof(ISenderData<3, FPkt>), &d);
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
  s.holdOn = GetParam(kLevelHoldOn)->Value() > 0.5 ? 1 : 0; // 峰值保持开关状态持久化
#if ORM_ENABLE_TEST_GEN
  s.genType = mGenType.load(std::memory_order_relaxed);
  s.genFreq = (double)mGenFreq.load(std::memory_order_relaxed);
  s.genLevel = (double)mGenLevel.load(std::memory_order_relaxed);
  s.genHold = mGenHold.load(std::memory_order_relaxed) ? 1 : 0;
  s.genToOutput = mGenToOutput.load(std::memory_order_relaxed) ? 1 : 0;
#endif
  SaveSettings(s);
}
