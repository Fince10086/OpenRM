#include "DectalkEngine.h"

// DECtalk 内核以 C 编译 (dectalk/ 目录), 头文件自带 __cplusplus 链接保护,
// C++ 侧直接包含即可; 两个子系统释放函数仅以 extern 声明在 epsonapi.c 中,
// 这里按 C 链接补声明。
// 注意: 必须用相对路径包含 — dectalk/include 里自带 config.h, 若把它加进插件
// 的全局 include 路径会遮蔽 iPlug2 依赖的插件 config.h。
extern "C" {
#include "dectalk/include/epsonapi.h" // TextToSpeech* 嵌入 API
}

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <mutex>

namespace orm
{

// DECtalk 内核 (epsonapi.c) 含文件级全局状态 (cur_packet_number 等), 同进程
// 内多个插件实例共享同一份代码与状态, 渲染必须互斥串行。渲染一条短语在
// 毫秒级, 串行代价可忽略。
static std::mutex gDectalkMutex;

// 回调累积缓冲: 引擎按块回调 16-bit 样本, 回调签名不含用户上下文指针,
// 串行渲染下用静态缓冲承接 (与 SamEngine 的全局态互斥同一思路)。
static std::vector<short> gPcm;

static short* DectalkCallback(short* buf, long n, int /*phoneme*/)
{
  if (n > 0)
    gPcm.insert(gPcm.end(), buf, buf + n);
  return buf;
}

// 音色经 DECtalk 文本命令 [:name X] 切换 (TextToSpeechChangeVoice 的 usevoice
// 路径在 HLSYN/CHANGES_AFTER_V43 关断时不生效, 实测只有解析器路径有效)
static const char* const kVoiceNames[kNumDectalkVoices] = {
    "paul", "betty", "harry", "frank", "dennis",
    "kit", "ursula", "rita", "wendy"};

// define_options 中的音色参数名: "ap" = 平均音高 (Hz)
static const char kVoiceParamAP[] = "ap";

bool DectalkEngine::Render(const std::string& text, bool phonetic,
                           const DectalkSettings& settings, std::vector<float>& out)
{
  out.clear();
  if (text.empty())
    return false;

  std::lock_guard<std::mutex> lock(gDectalkMutex);

  gPcm.clear();
  void* tts = TextToSpeechAllocate();
  if (!tts)
    return false;

  const bool inited = TextToSpeechInitEx(tts, DectalkCallback, nullptr, nullptr) == ERR_NOERROR;
  if (!inited)
  {
    TextToSpeechFree(tts);
    return false;
  }

  TextToSpeechSetRateEx(tts, std::clamp(settings.rate, 75, 600));
  if (settings.pitch != 0) // AP 平均音高 (Hz): setparam 路径, 保韵律
    TextToSpeechSetVoiceParamEx(tts, kVoiceParamAP, std::clamp(settings.pitch, 50, 500));

  // 音色/音素模式用 DECtalk 命令前缀注入 (语法 [:cmd ...]); 音素模式开启后其
  // 后的文本全部按音素读取, 实例随渲染销毁即自动复位。注意: 命令必须是词典里
  // 真实存在的名字, 未知命令会走错误路径产生冗长的停顿。
  std::string input;
  const int voice = std::clamp(settings.voice, 0, kNumDectalkVoices - 1);
  if (voice != kDectalkPaul)
  {
    input += "[:name ";
    input += kVoiceNames[voice];
    input += "] ";
  }
  if (phonetic)
    input = "[:phoneme on] " + input;
  input += text;

  TextToSpeechStartEx(tts, input.data(), nullptr, WAVE_FORMAT_1M16);
  TextToSpeechSyncEx(tts);

  // 释放句柄 (内部已完整释放各子系统线程数据与共享数据)
  TextToSpeechFree(tts);

  // 防御性上限: 60 秒 (极长文本/异常输入防抖), 超出截断
  const size_t maxSamples = (size_t)(kSampleRate * 60.0);
  if (gPcm.size() > maxSamples)
    gPcm.resize(maxSamples);

  const size_t n = gPcm.size();
  if (n == 0)
    return false;

  out.resize(n);
  float peak = 0.f;
  for (size_t i = 0; i < n; i++)
  {
    const float s = (float) gPcm[i] / 32768.f;
    out[i] = s;
    const float a = std::fabs(s);
    if (a > peak)
      peak = a;
  }

  // 短语级峰值归一 (DECtalk 输出电平稳定, 但与其余引擎保持同一约定);
  // 静音内容视为失败
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
  const size_t fadeLen = std::min(n, (size_t) (kSampleRate * 0.008));
  for (size_t i = 0; i < fadeLen; i++)
  {
    const float g = 1.f - (float) i / (float) fadeLen;
    out[n - 1 - i] *= g;
  }

  return true;
}

} // namespace orm