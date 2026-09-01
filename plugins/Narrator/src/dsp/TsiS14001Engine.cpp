// TsiS14001Engine.cpp — TSI/SSi S14001A 状态机忠实移植 (MAME, BSD-3-Clause)
//
// 移植对照 MAME src/devices/sound/s14001a.cpp:
//   - phases: Clock() 每调用一次在外时钟两个相位间切换 (phase1 逻辑 / phase2
//     传值 + carry 派生)。DAC 输出每个值保持 2 个采样 (与源注释 "digital out
//     valid every other clock cycle" 一致)。
//   - PLAY 状态读的 P2 寄存器在本实现里由单一寄存器 + phase2 入口派生的
//     carry 标志等价表示。
#include "TsiS14001Engine.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>

namespace orm
{

namespace
{

// 芯片状态机 (匿名命名空间, 仅供本文件使用)
struct Chip
{
  const unsigned char* rom = nullptr;
  int romLen = 0;

  // 相位与状态 (0..7: IDLE WORDWAIT CWARMSB CWARLSB DARMSB CTRLBITS PLAY DELAY)
  bool phase1 = false;
  uint8_t state = 0;

  // 寄存器
  uint16_t dar13 = 0;      // 9 MSBs, delta 地址
  uint16_t dar04 = 0;      // 5 LSBs
  uint16_t cwar = 0;       // 12-bit 控制字地址
  bool stop = false;
  bool voiced = false;
  bool silence = false;
  uint8_t length = 0;      // 7-bit 音高周期长度计数
  uint8_t xrepeat = 0;     // 2-bit 重复计数
  uint8_t deltaOld = 0;    // 上一 delta
  uint8_t output = 7;      // 4-bit 输出电平
  uint16_t romAddr = 0;
  bool busy = false;
  bool start = false;
  uint8_t word = 0;

  // phase2 派生的进位标志 (PLAY 状态使用)
  bool cDar04 = false;
  bool cPpq = false;
  bool cRep = false;
  bool cLen = false;

  uint8_t Read(uint16_t off) const
  {
    off &= 0x0FFF; // 芯片内部 12-bit
    if ((int) off >= romLen)
      return 0; // 窗口内未加载字节按 0 (MAME region 零填充)
    return rom[off];
  }

  void Clock();
};

// delta→增量 PLA 表 (MAME CalculateIncrement 的 uIncrements)
const uint8_t kIncrements[4][4] = {
    {3, 3, 1, 1}, // 00
    {1, 1, 0, 0}, // 01
    {0, 0, 1, 1}, // 10
    {1, 1, 3, 3}, // 11
};

// 从 ROM 字节取 2-bit delta (大端序, 镜像时反向)
uint8_t Mux8To2(bool voiced, uint8_t ppqtr, uint8_t deltaAddr, uint8_t data)
{
  if (voiced && (ppqtr & 0x01))
    deltaAddr ^= 0x03; // 镜像: 倒数
  return (uint8_t)((data >> ((~deltaAddr << 1) & 0x06)) & 0x03);
}

// 由 delta 算增量 (0/1/3) 与加/减方向, 同时更新 deltaOld
void CalcIncrement(bool voiced, uint8_t ppqtr, bool ppqStart, uint8_t delta,
                   uint8_t deltaOld, uint8_t& incOut, bool& addOut,
                   uint8_t& deltaOldOut)
{
  if (ppqtr == 0 && ppqStart)
    deltaOld = 2; // 每个音高周期起点重置

  uint8_t inc;
  bool add;
  if (!voiced || !(ppqtr & 0x01))
  {
    inc = kIncrements[delta][deltaOld];
    add = delta >= 0x02;
  }
  else
  {
    inc = kIncrements[deltaOld][delta];
    add = deltaOld < 0x02;
  }
  deltaOldOut = delta;
  if (voiced && ppqStart && (ppqtr & 0x01))
    inc = 0; // 镜像刚开始时不变化
  incOut = inc;
  addOut = add;
}

// 4-bit 输出积分器 (中心 7, 限幅 0..15; silence 或浊音静音半周期输出 7)
uint8_t CalcOutput(bool voiced, bool xSilence, uint8_t ppqtr, bool ppqStart,
                   uint8_t lOutput, uint8_t inc, bool add)
{
  if (xSilence || (voiced && (ppqtr & 0x02)))
    return 7;

  if (ppqtr == 0 && ppqStart)
    lOutput = 7;

  uint8_t tmp = lOutput;
  if (!add)
    tmp ^= 0x0F; // 减法转加法 (1 补码)
  tmp += inc;
  if (tmp > 15)
    tmp = 15;
  if (!add)
    tmp ^= 0x0F;
  return tmp;
}

void Chip::Clock()
{
  // 外时钟一个周期 = 内部两个相位各一次 (MAME Clock() 每次调用切换一个相位,
  // sound_stream_update 每采样调一次, 故 D/A 输出每个值保持 2 个采样)
  if (phase1)
  {
    // -> phase2: P1 寄存器传给 P2, 由 P2 值派生进位标志
    phase1 = false;
    cDar04 = (dar04 == 0x1F);
    cPpq = cDar04 && ((length & 0x03) == 0x03);       // 音高周期季度末
    cRep = cPpq && ((length & 0x0C) == 0x0C);         // 重复结束
    cLen = cRep && (length == 0x7F);                  // 长度计满
    return;
  }
  phase1 = true;

  switch (state)
  {
    case 0: // IDLE
      output = 7;
      if (start)
        state = 1;
      busy = false;
      break;
    case 1: // WORDWAIT: 词号锁入 delta 地址寄存器
      dar13 = (uint16_t)((word & 0x3C) >> 2);
      dar04 = (uint16_t)((word & 0x03) << 3);
      romAddr = (uint16_t)((dar13 << 3) | (dar04 >> 2));
      output = 7;
      state = start ? 1 : 2;
      busy = true;
      break;
    case 2: // CWARMSB: 读 12-bit 控制字地址高 8 位
      cwar = (uint16_t)(Read(romAddr) << 4);
      dar04 += 4;
      if (dar04 >= 32)
        dar04 = 0;
      romAddr = (uint16_t)((dar13 << 3) | (dar04 >> 2));
      output = 7;
      state = start ? 1 : 3;
      break;
    case 3: // CWARLSB: 控制字地址低 4 位 (第二字节 d7-d4)
      cwar = (uint16_t)((cwar & 0xFF0) | (Read(romAddr) >> 4));
      romAddr = cwar;
      output = 7;
      state = start ? 1 : 4;
      break;
    case 4: // DARMSB: 读首字节作 delta 地址 (DAR = byte<<1, 块在 byte*16)
      dar13 = (uint16_t)(Read(romAddr) << 1);
      dar04 = 0;
      cwar++;
      romAddr = cwar;
      output = 7;
      state = start ? 1 : 5;
      break;
    case 5: // CTRLBITS: stop/voiced/silence/length/repeat
    {
      const uint8_t d = Read(romAddr);
      stop = (d & 0x80) != 0;
      voiced = (d & 0x40) != 0;
      silence = (d & 0x20) != 0;
      xrepeat = d & 0x03;
      length = (uint8_t)((d & 0x1F) << 2);
      dar04 = 0;
      cwar++;
      romAddr = (uint16_t)((dar13 << 3) | (dar04 >> 2));
      output = 7;
      state = start ? 1 : 6;
      break;
    }
    case 6: // PLAY
    {
      const uint8_t ppqtr = length & 0x03; // 音高周期季度计数 (低 2 位)
      const bool ppqStart = (dar04 == 0);
      const uint8_t delta =
          Mux8To2(voiced, ppqtr, (uint8_t)(dar04 & 0x03), Read(romAddr));
      uint8_t inc = 0;
      bool add = false;
      CalcIncrement(voiced, ppqtr, ppqStart, delta, deltaOld, inc, add, deltaOld);
      output = CalcOutput(voiced, silence, ppqtr, ppqStart, output, inc, add);

      dar04++;
      if (cDar04) // 季度末 (32 时钟): 进位到长度计数
      {
        dar04 = 0;
        length++;
        if (length >= 0x80)
          length = 0;
      }
      if (voiced && cRep) // 浊音重复结束: 重载 repeat, delta 块前进 8 字节
      {
        length &= 0x70;
        length |= (uint8_t)(xrepeat << 2);
        dar13++;
        if (dar13 >= 0x200)
          dar13 = 0;
      }
      if (!voiced && cDar04) // 清音每季度前进一块
      {
        dar13++;
        if (dar13 >= 0x200)
          dar13 = 0;
      }

      uint16_t a = dar04;
      if (voiced && (length & 0x01))
        a ^= 0x1F; // 浊音镜像半周期: 倒序
      romAddr = (uint16_t)((dar13 << 3) | (a >> 2));

      if (start)
        state = 1;
      else if (stop && cLen) // 词结束
        state = 7;
      else if (cLen)
      {
        state = 4; // 下一控制字
        romAddr = cwar;
      }
      else
        state = 6;
      break;
    }
    case 7: // DELAY
      output = 7;
      state = start ? 1 : 0;
      break;
    default:
      state = 0;
      break;
  }
}

// 渲染单个词: 重置芯片, START 脉冲, 逐时钟取样到回 IDLE (或跑飞上限)
// 返回 false 表示该词不可用 (跑飞/全静音), 调用方跳过。
bool RenderWord(const s14001::Set& S, int word, double clock, std::vector<float>& out)
{
  Chip chip;
  chip.rom = S.data;
  chip.romLen = S.len;
  chip.word = (uint8_t)(word & 0x3F);
  // START 脉冲 (data_w 已存入 word): start_w(1) 置 WORDWAIT, start_w(0)
  chip.state = 1; // start_w(1): if (!start) state = WORDWAIT
  chip.start = false; // start_w(0)

  const int cap = (int)(clock * 4.0); // 上限 4 秒 (防无效索引跑飞)
  out.clear();
  out.reserve((size_t) cap);
  bool capped = false;
  for (int i = 0; i < cap; ++i)
  {
    chip.Clock();
    out.push_back((float)(((int) chip.output) - 7) / 8.0f);
    if (chip.state == 0 && !chip.busy) // 回 IDLE 且忙线释放
    {
      capped = false;
      break;
    }
    capped = (i + 1 >= cap);
  }
  if (capped)
    return false;

  // 静音词 (无实际内容) 视为未收录
  size_t active = 0;
  for (float s : out)
    if (s != 0.f)
      active++;
  return active >= 8;
}

} // namespace

double TsiS14001Engine::RateForSet(int set, float rateScale)
{
  if (set < 0 || set >= s14001::kNumSets)
    return s14001::kSets[0].clock;
  return s14001::kSets[set].clock * (double) std::clamp(rateScale, 0.25f, 4.0f);
}

bool TsiS14001Engine::Render(const std::string& text, int set, float rateScale,
                             std::vector<float>& out)
{
  out.clear();
  if (set < 0 || set >= s14001::kNumSets)
    return false;

  rateScale = std::clamp(rateScale, 0.25f, 4.0f);
  const s14001::Set& S = s14001::kSets[set];
  const double clock = S.clock * (double) rateScale;

  // 分词: 词索引 (Wnn / n; 0..63), 其余跳过
  std::vector<int> words;
  std::string token;
  for (size_t i = 0; i <= text.size(); ++i)
  {
    const bool sep = (i == text.size()) || std::isspace((unsigned char) text[i]);
    if (sep)
    {
      if (!token.empty())
      {
        std::string t = token;
        token.clear();
        if (!t.empty() && (t[0] == 'W' || t[0] == 'w'))
          t = t.substr(1);
        bool digits = !t.empty();
        for (char c : t)
          if (!std::isdigit((unsigned char) c))
          {
            digits = false;
            break;
          }
        if (digits)
        {
          const int idx = std::atoi(t.c_str());
          if (idx >= 0 && idx < 64)
            words.push_back(idx);
        }
      }
      continue;
    }
    token.push_back((char) std::toupper((unsigned char) text[i]));
  }
  if (words.empty())
    return false;

  std::vector<float> wordBuf;
  bool any = false;
  for (int w : words)
  {
    if (RenderWord(S, w, clock, wordBuf))
    {
      out.insert(out.end(), wordBuf.begin(), wordBuf.end());
      any = true;
    }
  }
  if (!any)
  {
    out.clear();
    return false;
  }

  // 峰值归一 0.85 (与其他引擎一致)
  float peak = 0.f;
  for (float s : out)
    peak = std::max(peak, std::abs(s));
  if (peak <= 0.f)
  {
    out.clear();
    return false;
  }
  const float scale = 0.85f / peak;
  for (float& s : out)
    s *= scale;
  return true;
}

} // namespace orm