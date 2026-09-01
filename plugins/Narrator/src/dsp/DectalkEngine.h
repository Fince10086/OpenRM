// DectalkEngine.h — DECtalk 4.x (DTC01 软件版) 引擎封装
//
// 核心代码: dectalk/ 目录 (DECtalkMini 移植, 原 DECtalk 4.x 源码; 来源与
// 许可说明见 dectalk/README.md 与 docs/Narrator-Design.md §1)。
//
// 本封装把嵌入式 TTS API (epsonapi.h 的 TextToSpeech*) 改造成可嵌入音频插件
// 的离线渲染器: 文本(或 DECtalk 音素串) → 单声道 float 缓冲, 11025 Hz。
// DECtalk 内核含文件级全局状态, 渲染必须串行 (见 .cpp 的进程级互斥)。
#pragma once

#include <string>
#include <vector>

namespace orm
{

// DECTalk 音色索引 (与 DECtalk 4.x 内置音色一一对应, TextToSpeechChangeVoice
// 的 np/nb/... 代码)。默认音色 Paul。
enum
{
  kDectalkPaul = 0,
  kDectalkBetty,
  kDectalkHarry,
  kDectalkFrank,
  kDectalkDennis,
  kDectalkKit,
  kDectalkUrsula,
  kDectalkRita,
  kDectalkWendy,
  kNumDectalkVoices
};

struct DectalkSettings
{
  int voice = kDectalkPaul; // 音色 (见上枚举, 0..8)
  int rate = 180;           // 说话速率 75..600 (DECtalk 定义; 实测引擎原生 ≈ 180)
  int pitch = 0;            // 平均音高 AP (Hz, DECtalk "ap" 参数); 0 = 音色原生
};

class DectalkEngine
{
public:
  static constexpr double kSampleRate = 11025.0;

  // text: 英语普通文本 (phonetic=false, DECtalk 全量词典/拼写规则 + 韵律) 或
  //       DECtalk 音素串 (phonetic=true, 如 "HX EH L OW", 大写空格分隔)。
  // out:  单声道 float, 峰值归一到 0.85。渲染失败/静音返回 false。
  static bool Render(const std::string& text, bool phonetic,
                     const DectalkSettings& settings, std::vector<float>& out);
};

} // namespace orm