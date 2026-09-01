#include "Tms5220Engine.h"

#include "tms/TmsVocab.h"
#include "tms/VocabTI99.h"
#include "tms/VocabAcorn.h"
#include "tms/VocabClock.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>

namespace orm
{

namespace
{

// ---- TMS5220 量化表 (TalkieLPC.h, GPL; 默认 16-bit K1/K2 变体) ----

const uint8_t kEnergy[0x10] = {0, 2, 3, 4, 5, 7, 10, 15, 20, 32, 41, 57, 81, 114, 161, 255};
const uint8_t kPeriod[0x40] = {0, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32,
    33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 45, 47, 49, 51, 53, 54, 57, 59, 61, 63, 66, 69, 71, 73,
    77, 79, 81, 85, 87, 92, 95, 99, 102, 106, 110, 115, 119, 123, 128, 133, 138, 143, 149, 154, 160};

const int16_t kK1[0x20] = {-32064, -31872, -31808, -31680, -31552, -31424, -31232, -30848, -30592,
    -30336, -30016, -29696, -29376, -28928, -28480, -27968, -26368, -24256, -21632, -18368, -14528,
    -10048, -5184, 0, 5184, 10048, 14528, 18368, 21632, 24256, 26368, 27968};
const int16_t kK2[0x20] = {-20992, -19328, -17536, -15552, -13440, -11200, -8768, -6272, -3712, -1088,
    1536, 4160, 6720, 9216, 11584, 13824, 15936, 17856, 19648, 21248, 22656, 24000, 25152, 26176,
    27072, 27840, 28544, 29120, 29632, 30080, 30464, 32384};
const int8_t kK3[0x10] = {-110, -97, -83, -70, -56, -43, -29, -16, -2, 11, 25, 38, 52, 65, 79, 92};
const int8_t kK4[0x10] = {-82, -68, -54, -40, -26, -12, 1, 15, 29, 43, 57, 71, 85, 99, 113, 126};
const int8_t kK5[0x10] = {-82, -70, -59, -47, -35, -24, -12, -1, 11, 23, 34, 46, 57, 69, 81, 92};
const int8_t kK6[0x10] = {-64, -53, -42, -31, -20, -9, 3, 14, 25, 36, 47, 58, 69, 80, 91, 102};
const int8_t kK7[0x10] = {-77, -65, -53, -41, -29, -17, -5, 7, 19, 31, 43, 55, 67, 79, 90, 102};
const int8_t kK8[0x08] = {-64, -40, -16, 7, 31, 55, 79, 102};
const int8_t kK9[0x08] = {-64, -44, -24, -4, 16, 37, 57, 77};
const int8_t kK10[0x08] = {-51, -33, -15, 4, 22, 32, 59, 77};

// 激励脉冲 (TI2802 之后芯片所用 chirp 表)
const int8_t kChirp[] = {0x00, 0x03, 0x0f, 0x28, 0x4c, 0x6c, 0x71, 0x50, 0x25, 0x26, 0x4c, 0x44,
    0x1a, 0x32, 0x3b, 0x13, 0x37, 0x1a, 0x25, 0x1f, 0x1d};

// Talkie 词汇数据的位序: 字节内比特反排 (LSB-first), 逐位 MSB-first 取出
inline uint8_t RevByte(uint8_t a)
{
  a = (a >> 4) | (a << 4);
  a = ((a & 0xcc) >> 2) | ((a & 0x33) << 2);
  a = ((a & 0xaa) >> 1) | ((a & 0x55) << 1);
  return a;
}

struct BitReader
{
  const unsigned char* data;
  int len;
  int pos = 0;    // 字节游标
  int bitPos = 0; // 字节内位游标

  uint8_t GetBits(int bits)
  {
    uint16_t window = (uint16_t)(RevByte(data[pos]) << 8);
    if (bitPos + bits > 8 && pos + 1 < len)
      window |= RevByte(data[pos + 1]);
    window <<= bitPos;
    const uint8_t value = (uint8_t)(window >> (16 - bits));
    bitPos += bits;
    if (bitPos >= 8)
    {
      bitPos -= 8;
      pos++;
    }
    return value;
  }
};

// 每帧 200 采样 @8kHz = 40Hz 帧率 (Talkie: ISR_CALLS_UNTIL_NEXT_DATA)
constexpr int kFrameSamples = 200;
constexpr int kMaxFrames = 800; // 防御上限: 20 秒 (多词短语)

// 按音色/词库选择词表 (bank 3 / TMS5110 由上层路由到 Tms5110Engine)
const tms::Word* SelectTable(int bank, int& n)
{
  switch (bank)
  {
    case 1: n = tms::ti99::kNumWords; return tms::ti99::kWords;
    case 2: n = tms::acorn::kNumWords; return tms::acorn::kWords;
    case 4: n = tms::clock::kNumWords; return tms::clock::kWords;
    case 0:
    default: n = tms::military::kNumWords; return tms::military::kWords;
  }
}

} // namespace

bool Tms5220Engine::Render(const std::string& text, int bank, float speedScale, std::vector<float>& out)
{
  out.clear();

  // 逐词查表: 按空格拆分, 大写规范化, 当前音色未收录的词跳过
  struct Stream { const unsigned char* data; int len; };
  std::vector<Stream> streams;

  const tms::Word* kTable = nullptr;
  int kCount = 0;
  if (bank == 0 || bank == 1 || bank == 2 || bank == 4)
    kTable = SelectTable(bank, kCount);

  std::string token;
  for (size_t i = 0; i <= text.size(); ++i)
  {
    const bool sep = (i == text.size()) || std::isspace((unsigned char) text[i]);
    if (sep)
    {
      if (!token.empty())
      {
        if (kTable)
        {
          for (int w = 0; w < kCount; ++w)
          {
            if (token == kTable[w].name)
            {
              streams.push_back({kTable[w].data, kTable[w].len});
              break;
            }
          }
        }
        token.clear();
      }
      continue;
    }
    token.push_back((char) std::toupper((unsigned char) text[i]));
  }
  if (streams.empty())
    return false;

  BitReader br{streams[0].data, streams[0].len};
  size_t cur = 0;
  bool wordDone = false; // 首帧前无需推进 (br 已指向第一个词)

  // 帧参数 (帧间保持)
  uint8_t synthPeriod = 0;
  int16_t synthEnergy = 0;
  int32_t k1 = 0, k2 = 0, k3 = 0, k4 = 0, k5 = 0, k6 = 0, k7 = 0, k8 = 0, k9 = 0, k10 = 0;

  // 格型滤波器状态
  int16_t x0 = 0, x1 = 0, x2 = 0, x3 = 0, x4 = 0, x5 = 0, x6 = 0, x7 = 0, x8 = 0, x9 = 0;
  uint16_t synthRand = 1;
  uint8_t periodCounter = 0;

  speedScale = std::clamp(speedScale, 0.25f, 4.0f);
  const int frameSamples = std::max(40, (int) std::lround(kFrameSamples * speedScale));

  out.reserve(4096);
  for (int frame = 0; frame < kMaxFrames; frame++)
  {
    if (wordDone)
    {
      // Talkie 的停止帧行为: FIFOPopFront() 接下一个词; 词队列耗尽则结束
      if (++cur >= streams.size())
        break;
      br = BitReader{streams[cur].data, streams[cur].len};
      wordDone = false;
    }
    if (br.pos >= br.len)
    {
      wordDone = true; // 位流提前耗尽 (无停止帧), 接下一个词
      continue;
    }

    const uint8_t energyIdx = br.GetBits(4);
    if (energyIdx == 0xF)
    {
      // 停止帧: 系数清零 (格型惯性仍输出一帧衰减), 随后接下一个词
      synthEnergy = 0;
      k1 = k2 = k3 = k4 = k5 = k6 = k7 = k8 = k9 = k10 = 0;
      wordDone = true;
    }
    else if (energyIdx == 0)
    {
      synthEnergy = 0; // 静音帧: 保持格型惯性输出衰减
    }
    else
    {
      synthEnergy = kEnergy[energyIdx];
      const uint8_t repeat = br.GetBits(1);
      synthPeriod = kPeriod[br.GetBits(6)];
      if (!repeat)
      {
        k1 = kK1[br.GetBits(5)];
        k2 = kK2[br.GetBits(5)];
        k3 = kK3[br.GetBits(4)];
        k4 = kK4[br.GetBits(4)];
        if (synthPeriod)
        {
          // 浊音帧才使用后 6 个系数
          k5 = kK5[br.GetBits(4)];
          k6 = kK6[br.GetBits(4)];
          k7 = kK7[br.GetBits(4)];
          k8 = kK8[br.GetBits(3)];
          k9 = kK9[br.GetBits(3)];
          k10 = kK10[br.GetBits(3)];
        }
      }
    }

    for (int s = 0; s < frameSamples; s++)
    {
      int16_t u10;
      if (synthPeriod)
      {
        // 浊音源: chirp 脉冲
        if (periodCounter < synthPeriod)
          periodCounter++;
        else
          periodCounter = 0;
        u10 = (periodCounter < (int) sizeof(kChirp))
                  ? (int16_t)(((int32_t) kChirp[periodCounter] * synthEnergy) >> 8)
                  : 0;
      }
      else
      {
        // 清音源: 伪随机白噪声
        synthRand = (uint16_t)((synthRand >> 1) ^ ((synthRand & 1) ? 0xB800 : 0));
        u10 = (synthRand & 1) ? (int16_t) synthEnergy : (int16_t) -synthEnergy;
      }

      // 格型滤波正向路径
      const int16_t u9 = (int16_t)(u10 - ((((int32_t) k10 * x9) >> 7)));
      const int16_t u8 = (int16_t)(u9 - ((((int32_t) k9 * x8) >> 7)));
      const int16_t u7 = (int16_t)(u8 - ((((int32_t) k8 * x7) >> 7)));
      const int16_t u6 = (int16_t)(u7 - ((((int32_t) k7 * x6) >> 7)));
      const int16_t u5 = (int16_t)(u6 - ((((int32_t) k6 * x5) >> 7)));
      const int16_t u4 = (int16_t)(u5 - ((((int32_t) k5 * x4) >> 7)));
      const int16_t u3 = (int16_t)(u4 - ((((int32_t) k4 * x3) >> 7)));
      const int16_t u2 = (int16_t)(u3 - ((((int32_t) k3 * x2) >> 7)));
      const int16_t u1 = (int16_t)(u2 - ((((int32_t) k2 * x1) << 1) >> 16));
      const int16_t u0 = (int16_t)(u1 - ((((int32_t) k1 * x0) << 1) >> 16));

      // 反向路径: 更新状态
      x9 = (int16_t)(x8 + ((((int32_t) k9 * u8) >> 7)));
      x8 = (int16_t)(x7 + ((((int32_t) k8 * u7) >> 7)));
      x7 = (int16_t)(x6 + ((((int32_t) k7 * u6) >> 7)));
      x6 = (int16_t)(x5 + ((((int32_t) k6 * u5) >> 7)));
      x5 = (int16_t)(x4 + ((((int32_t) k5 * u4) >> 7)));
      x4 = (int16_t)(x3 + ((((int32_t) k4 * u3) >> 7)));
      x3 = (int16_t)(x2 + ((((int32_t) k3 * u2) >> 7)));
      x2 = (int16_t)(x1 + ((((int32_t) k2 * u1) << 1) >> 16));
      x1 = (int16_t)(x0 + ((((int32_t) k1 * u0) << 1) >> 16));
      x0 = u0;

      out.push_back((float) u0 / 511.0f);
    }
  }

  // 峰值归一 (与 SamEngine 一致的乐器响度策略)
  float peak = 0.f;
  for (float s : out)
    peak = std::max(peak, std::abs(s));
  if (peak < 0.01f)
  {
    out.clear();
    return false;
  }
  const float gain = 0.85f / peak;
  for (auto& s : out)
    s *= gain;

  // 尾部 8ms 淡出
  const int fadeLen = std::min((int) out.size(), (int) (kSampleRate * 0.008));
  for (int i = 0; i < fadeLen; i++)
    out[(size_t)(out.size() - 1 - i)] *= 1.f - (float) i / (float) fadeLen;

  return true;
}

} // namespace orm
