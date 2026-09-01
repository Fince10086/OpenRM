#include "Tms5110Engine.h"

#include "tms/TmsWords.h"
#include "tms/VocabSspell.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>

namespace orm
{

namespace
{

// ---- TMC0281 (1978 Speak & Spell) 参数表, MAME tms5110r.hxx: T0280B_0281A_coeff ----

// E: energy 索引表 (索引 15 = 停止帧)
const uint8_t kEnergy[0x10] = {0, 0, 1, 1, 2, 3, 5, 7, 10, 15, 21, 30, 43, 61, 86, 0};
// P: pitch 周期表 (5 位索引, 32 项)
const uint8_t kPeriod[0x20] = {0, 41, 43, 45, 47, 49, 51, 53, 55, 58, 60, 63, 66, 70,
    73, 76, 79, 83, 87, 90, 94, 99, 103, 107, 112, 118, 123, 129, 134, 140, 147, 153};

// K1..K10: 专利 LPC 反射系数表 (10 位带符号)
const int16_t kK1[0x20] = {-501, -497, -493, -488, -480, -471, -460, -446, -427, -405,
    -378, -344, -305, -259, -206, -148, -86, -21, 45, 110, 171, 227, 277, 320, 357, 388,
    413, 434, 451, 464, 474, 498};
const int16_t kK2[0x20] = {-349, -328, -305, -280, -252, -223, -192, -158, -124, -88,
    -51, -14, 23, 60, 97, 133, 167, 199, 230, 259, 286, 310, 333, 354, 372, 389, 404,
    417, 429, 439, 449, 506};
const int16_t kK3[0x10] = {-397, -365, -327, -282, -229, -170, -104, -36, 35, 104, 169,
    228, 281, 326, 364, 396};
const int16_t kK4[0x10] = {-369, -334, -293, -245, -191, -131, -67, -1, 64, 128, 188,
    243, 291, 332, 367, 397};
const int16_t kK5[0x10] = {-319, -286, -250, -211, -168, -122, -74, -25, 24, 73, 121,
    167, 210, 249, 285, 318};
const int16_t kK6[0x10] = {-290, -252, -209, -163, -114, -62, -9, 44, 97, 147, 194,
    238, 278, 313, 344, 371};
const int16_t kK7[0x10] = {-291, -256, -216, -174, -128, -80, -31, 19, 69, 117, 163,
    206, 246, 283, 316, 345};
const int16_t kK8[0x08] = {-218, -133, -38, 59, 152, 235, 305, 361};
const int16_t kK9[0x08] = {-226, -157, -82, -3, 76, 151, 220, 280};
const int16_t kK10[0x08] = {-179, -122, -61, 1, 62, 123, 179, 231};

// 激励脉冲 (专利 chirp, 52 项, int8 有符号; 与 MAME 的 (int8_t) 解析一致)
const int8_t kChirp[] = {0, 42, -44, 50, -78, 18, 37, 20, 2, -31, -59, 2, 95, 90, 5, 15,
    38, -4, -91, -91, -42, -35, -36, -4, 37, 43, 34, 33, 15, -1, -8, -18, -19, -17, -9,
    -10, -6, 0, 3, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

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
  int pos = 0;
  int bitPos = 0;

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

// MAME tms5110 的 matrix_multiply: a 钳到 10 位 (K 系数), b 钳到 14 位 (格型状态)
inline int32_t MatrixMul(int32_t a, int32_t b)
{
  while (a > 511) a -= 1024;
  while (a < -512) a += 1024;
  while (b > 16383) b -= 32768;
  while (b < -16384) b += 32768;
  return (a * b) >> 9;
}

// 每帧 200 采样 @8kHz = 40Hz 帧率
constexpr int kFrameSamples = 200;
constexpr int kMaxFrames = 800;

} // namespace

bool Tms5110Engine::Render(const std::string& text, float speedScale, std::vector<float>& out)
{
  out.clear();

  struct Stream { const unsigned char* data; int len; };
  std::vector<Stream> streams;

  std::string token;
  for (size_t i = 0; i <= text.size(); ++i)
  {
    const bool sep = (i == text.size()) || std::isspace((unsigned char) text[i]);
    if (sep)
    {
      if (!token.empty())
      {
        for (const auto& w : tms::sspell::kWords)
        {
          if (token == w.name)
          {
            streams.push_back({w.data, w.len});
            break;
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
  bool wordDone = false;

  // 帧参数
  int32_t currentEnergy = 0;
  int32_t currentPitch = 0; // 0 = 清音
  int32_t k[10] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

  // 格型状态 (matrix_multiply 用 14 位中间量)
  int32_t x[10] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  uint16_t synthRand = 1;
  int periodCounter = 0;

  speedScale = std::clamp(speedScale, 0.25f, 4.0f);
  const int frameSamples = std::max(40, (int) std::lround(kFrameSamples * speedScale));

  out.reserve(4096);
  for (int frame = 0; frame < kMaxFrames; frame++)
  {
    if (wordDone)
    {
      if (++cur >= streams.size())
        break;
      br = BitReader{streams[cur].data, streams[cur].len};
      wordDone = false;
    }
    if (br.pos >= br.len)
    {
      wordDone = true;
      continue;
    }

    // TMS5110: energy(4) — 0/15 为静音/停止帧
    const uint8_t energyIdx = br.GetBits(4);
    if (energyIdx == 0xF)
    {
      currentEnergy = 0;
      for (int i = 0; i < 10; i++)
        k[i] = 0;
      wordDone = true;
    }
    else if (energyIdx == 0)
    {
      currentEnergy = 0;
    }
    else
    {
      currentEnergy = kEnergy[energyIdx];
      const uint8_t repeat = br.GetBits(1);
      currentPitch = kPeriod[br.GetBits(5)]; // 5 位 pitch! (5220 为 6 位)
      if (!repeat)
      {
        k[0] = kK1[br.GetBits(5)];
        k[1] = kK2[br.GetBits(5)];
        k[2] = kK3[br.GetBits(4)];
        k[3] = kK4[br.GetBits(4)];
        if (currentPitch)
        {
          // 浊音帧才使用后 6 个系数
          k[4] = kK5[br.GetBits(4)];
          k[5] = kK6[br.GetBits(4)];
          k[6] = kK7[br.GetBits(4)];
          k[7] = kK8[br.GetBits(3)];
          k[8] = kK9[br.GetBits(3)];
          k[9] = kK10[br.GetBits(3)];
        }
      }
    }

    for (int s = 0; s < frameSamples; s++)
    {
      int32_t exc;
      if (currentPitch)
      {
        if (periodCounter < currentPitch)
          periodCounter++;
        else
          periodCounter = 0;
        exc = (periodCounter < (int) sizeof(kChirp)) ? (int32_t) kChirp[periodCounter] : 0;
      }
      else
      {
        synthRand = (uint16_t)((synthRand >> 1) ^ ((synthRand & 1) ? 0xB800 : 0));
        exc = (synthRand & 1) ? 0x40 : -0x40;
      }

      // 激励: Y(11) = energy * (exc << 6); 再做 10 级格型 (MAME 顺序)
      int32_t u[11];
      u[10] = MatrixMul(currentEnergy, exc << 6);
      u[9] = u[10] - MatrixMul(k[9], x[9]);
      u[8] = u[9] - MatrixMul(k[8], x[8]);
      u[7] = u[8] - MatrixMul(k[7], x[7]);
      u[6] = u[7] - MatrixMul(k[6], x[6]);
      u[5] = u[6] - MatrixMul(k[5], x[5]);
      u[4] = u[5] - MatrixMul(k[4], x[4]);
      u[3] = u[4] - MatrixMul(k[3], x[3]);
      u[2] = u[3] - MatrixMul(k[2], x[2]);
      u[1] = u[2] - MatrixMul(k[1], x[1]);
      u[0] = u[1] - MatrixMul(k[0], x[0]);

      x[9] = x[8] + MatrixMul(k[8], u[8]);
      x[8] = x[7] + MatrixMul(k[7], u[7]);
      x[7] = x[6] + MatrixMul(k[6], u[6]);
      x[6] = x[5] + MatrixMul(k[5], u[5]);
      x[5] = x[4] + MatrixMul(k[4], u[4]);
      x[4] = x[3] + MatrixMul(k[3], u[3]);
      x[3] = x[2] + MatrixMul(k[2], u[2]);
      x[2] = x[1] + MatrixMul(k[1], u[1]);
      x[1] = x[0] + MatrixMul(k[0], u[0]);
      x[0] = u[0];

      out.push_back((float) u[0] / 16384.0f);
    }
  }

  // 峰值归一到 0.85 (与 SAM/TMS5220 一致的乐器响度策略)
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