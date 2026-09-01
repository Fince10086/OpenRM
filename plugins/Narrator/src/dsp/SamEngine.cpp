#include "SamEngine.h"

// SAM 内核以 C 编译 (见插件 CMakeLists), C++ 侧必须按 C 链接声明
extern "C" {
#include "sam/reciter.h"
#include "sam/sam.h"
}

#include <cctype>

namespace orm
{

// SAM 内核 (sam.c/render.c/reciter.c) 全部是全局变量状态, 同进程内多个插件实例
// 共享同一份代码与状态, 渲染必须互斥串行。渲染一条短语在毫秒级, 串行的代价
// 可以忽略;换来的是完全不做内核重构 (3400 行 C 保持原样, 便于对照上游)。
static std::mutex gSamMutex;

bool SamEngine::Render(const std::string& text, bool phonetic,
                       const SamSettings& settings, std::vector<float>& out)
{
  out.clear();
  if (text.empty())
    return false;

  std::lock_guard<std::mutex> lock(gSamMutex);

  // Reciter 的内部缓冲限 255 字节, 预留终止符
  char inputBuf[256] = {};
  const size_t maxLen = 250;
  const size_t n = text.size() < maxLen ? text.size() : maxLen;
  for (size_t i = 0; i < n; i++)
    inputBuf[i] = (char) std::toupper((unsigned char) text[i]);

  if (phonetic)
  {
    // 音素直通: \x9b 是 SAM 的终止标记
    inputBuf[n] = '\x9b';
  }
  else
  {
    inputBuf[n] = '[';
    // Reciter 原地改写 inputBuf 为音素串, 返回 0 表示规则解析失败
    if (!TextToPhonemes((unsigned char*) inputBuf))
      return false;
  }

  SetInput(inputBuf);
  SetPitch((unsigned char) std::min(std::max(settings.pitch, 0), 255));
  SetSpeed((unsigned char) std::min(std::max(settings.speed, 1), 255));
  SetMouth((unsigned char) std::min(std::max(settings.mouth, 0), 255));
  SetThroat((unsigned char) std::min(std::max(settings.throat, 0), 255));

  if (!SAMMain())
    return false;

  // GetBufferLength() 以 1/50 采样为计数单位, /50 还原真实采样数。
  // 缓冲按 10 秒分配, 此处防御性钳制防止越界。
  const int maxSamples = 22050 * 10;
  int numSamples = GetBufferLength() / 50;
  if (numSamples <= 0)
    return false;
  if (numSamples > maxSamples)
    numSamples = maxSamples;

  const unsigned char* raw = (const unsigned char*) GetBuffer();
  out.resize((size_t) numSamples);

  float peak = 0.f;
  for (int i = 0; i < numSamples; i++)
  {
    const float s = ((float) raw[i] - 128.f) / 128.f;
    out[(size_t) i] = s;
    const float a = std::abs(s);
    if (a > peak)
      peak = a;
  }

  // 短语级峰值归一 (原版电平随内容波动很大); 静音内容视为失败
  if (peak < 0.02f)
  {
    out.clear();
    return false;
  }
  const float gain = 0.85f / peak;
  if (gain < 0.999f || gain > 1.001f)
    for (auto& s : out)
      s *= gain;

  // 尾部 8ms 线性淡出, 避免短语自然结束时的爆音
  const int fadeLen = std::min(numSamples, (int) (kSampleRate * 0.008));
  for (int i = 0; i < fadeLen; i++)
  {
    const float g = 1.f - (float) i / (float) fadeLen;
    out[(size_t) (numSamples - 1 - i)] *= g;
  }

  return true;
}

} // namespace orm
