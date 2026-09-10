// SamEngine.h — SAM (Software Automatic Mouth, 1979/1982) 引擎封装
//
// 核心代码: s-macke/SAM (对 SoftVoice SAM 的逆向工程, 无许可证, abandonware)。
// Narrator 整体以 GPL-3.0-or-later 分发, 但 SAM 部分未获授权 —— 禁止公开发布,
// 详见 THIRD_PARTY_NOTICES.md。
//
// 本封装把原命令行程序改造成可嵌入音频插件的离线渲染器:
//   文本(或 SAM 音素串) → 单声道 float 缓冲, 22050 Hz。
// SAM 内核为全局变量状态且不可重入, 渲染必须串行 (见 .cpp 的进程级互斥)。
#pragma once

#include <mutex>
#include <string>
#include <vector>

namespace orm
{

struct SamSettings
{
  int pitch = 64;   // 0..255, SAM 原生音高 (默认 64)
  int speed = 72;   // 1..255, SAM 原生语速 (默认 72, 值越大越快)
  int mouth = 128;  // 0..255, 口腔形变 (默认 128)
  int throat = 128; // 0..255, 喉腔形变 (默认 128)
};

class SamEngine
{
public:
  static constexpr double kSampleRate = 22050.0;

  // text: 英语普通文本 (phonetic=false, 走 Reciter 逐字规则) 或 SAM 音素串
  //       (phonetic=true, 如 "/HAALOW2 /HEH3LOW2")。
  // out:  单声道 float, 峰值归一到 0.85 (原 8-bit 输出电平随短语波动大,
  //       乐器场景需要一致的响度)。渲染失败/无输出返回 false。
  static bool Render(const std::string& text, bool phonetic,
                     const SamSettings& settings, std::vector<float>& out);
};

} // namespace orm
