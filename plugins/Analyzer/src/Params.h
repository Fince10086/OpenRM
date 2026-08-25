#pragma once

// 参数枚举与全局常量。
// 独立成头的原因: UI 控件 (controls/*) 与插件本体 (Analyzer.h) 都要引用 EParams,
// 若留在 Analyzer.h 会导致控件头反向依赖插件头; 这里只依赖 <array>, 双方安全包含。
//
// Analyzer 是从 BandPass 减出来的第一步: mix 参数 (撤销/重做、保存/读取的载体),
// release/attack 参数 (频谱显示释放/上升时间), range 参数 (频谱显示下限 dBFS 幅度),
// res/lfRes/bpo 参数 (FFT 尺寸 / VQT 低频带宽下限 γ / VQT bins-per-octave 档位),
// mode 参数 (分析引擎选择)。

#include <array>

enum EParams {
  kMix = 0,
  kRelease,
  kRange,
  kAttack,
  kRes,
  kLfRes,
  kBpo,
  kMode,
  kNumParams
};

using ParamSnapshot = std::array<double, kNumParams>;

// 分析档位: 参数存档位索引, 经下表映射为实际值 (默认 = 最高档)
constexpr int kResOptions[] = {1024, 2048, 4096};
constexpr int kNumResOptions = 3;
constexpr int kLfResOptions[] = {40, 20, 10};
constexpr int kNumLfResOptions = 3;
constexpr int kBpoOptions[] = {12, 24};
constexpr int kNumBpoOptions = 2;

// 分析引擎
enum EAnalyzerMode { kModeFFT = 0, kModeVQT = 1 };

enum EControlTags { kCtrlTagPad = 100, kCtrlTagCpu = 101 };
