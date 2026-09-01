// Sp0256Engine.cpp — GI SP0256 微序控制器 + 12 阶滤波器忠实移植
//
// 移植对照 MAME src/devices/sound/sp0256.cpp (BSD-3-Clause):
//   - getb(): 位流 LSB-first 取字段, PC 为位地址 (每字节高 4 位为 immed4, 低 4
//     位为 opcode, 与 MAME 的 bit-reversed ROM 布局一致);
//   - micro(): RTS/SETPAGE/JMP/JSR/SETMODE + 13 种参数载入指令, 每条按
//     sp0256_datafmt 控制字序列更新 16 个编码寄存器后 regdec() 解码;
//   - filter: 浊音 = 每周期 impulse, 清音 = LFSR +/- 噪声; 6 个二阶节按
//     b*Z^-2 + f*Z^-1 递归, 输出 limit()<<2 (HIGH_QUALITY 路径, ±8192 限幅)。
// 渲染按 MAME sound_stream_update 的连续漏斗语义: 命令装在 ALD 上等 micro()
// 采用 (同时清寄存器), 词尾 RTS->0 停机后滤波器带残响自由振铃一段尾。
#include "Sp0256Engine.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace orm
{

namespace
{

// ---- MAME 常量 ----
constexpr int kPerPause = 64;   // PAUSE 指令的等效基频周期
constexpr int kPerNoise = 64;   // 清音噪声状态刷新周期
constexpr uint32_t kFifoAddr = 0x1800 << 3; // SPB640 FIFO 地址 (引擎无 FIFO, 仅判定用)
constexpr int kTailSamples = 800; // 词尾滤波器振铃尾 (采样数, 门限包络收尾用)
constexpr int kMaxSamples = 40000; // 单次渲染上限 (4 秒 @10kHz), 防跑飞

// 参数序号 (MAME sp0256.cpp enum)
enum { AM = 0, PR, B0, F0, B1, F1, B2, F2, B3, F3, B4, F4, B5, F5, IA, IP };

// ---- 指令格式控制字表 (MAME sp0256_datafmt) ----
// 每条打包: len(4) | shf(4) | param(4) | delta(1) | field(1) | clr5(1) | clra(1)
static const uint16_t sp0256_datafmt[] = {
  0x8000, 0x8008, 0x0108, 0x0208, 0x0308, 0x0408, 0x0508, 0x0608,
  0x0708, 0x0808, 0x0908, 0x0A08, 0x0B08, 0x0C08, 0x0D08, 0x0E08,
  0x0F08, 0x8026, 0x0108, 0x0834, 0x0926, 0x0A17, 0x0B26, 0x0C08,
  0x0D08, 0x8026, 0x0108, 0x0816, 0x0917, 0x0A08, 0x0B08, 0x0C08,
  0x0D08, 0x4000, 0x0026, 0x2926, 0x2B26, 0x2D08, 0x4000, 0x0026,
  0x2917, 0x2B08, 0x2D08, 0x0000, 0x0000, 0x1024, 0x1105, 0x1243,
  0x1333, 0x1443, 0x1533, 0x1643, 0x1733, 0x1833, 0x1924, 0x1A14,
  0x1B24, 0x1C05, 0x1D05, 0x1024, 0x1105, 0x1214, 0x1324, 0x1414,
  0x1524, 0x1614, 0x1724, 0x1814, 0x1915, 0x1A05, 0x1B05, 0x1C05,
  0x1D05, 0x4000, 0x0026, 0x2335, 0x2535, 0x2735, 0x4000, 0x0026,
  0x2326, 0x2526, 0x2726, 0x8026, 0x0108, 0x0243, 0x0335, 0x0443,
  0x0535, 0x0643, 0x0735, 0x0834, 0x0926, 0x0A17, 0x0B26, 0x0E05,
  0x0F05, 0x8026, 0x0108, 0x0216, 0x0326, 0x0416, 0x0526, 0x0616,
  0x0726, 0x0816, 0x0917, 0x0A08, 0x0B08, 0x0E05, 0x0F05, 0x1024,
  0x1105, 0x1833, 0x1924, 0x1A14, 0x1B24, 0x1C05, 0x1D05, 0x1024,
  0x1105, 0x1814, 0x1915, 0x1A05, 0x1B05, 0x1C05, 0x1D05, 0x0026,
  0x0108, 0x8026, 0x0108, 0x0243, 0x0335, 0x0443, 0x0535, 0x0643,
  0x0735, 0x0834, 0x0926, 0x0A17, 0x0B26, 0x0C08, 0x0D08, 0x0E05,
  0x0F05, 0x8026, 0x0108, 0x0216, 0x0326, 0x0416, 0x0526, 0x0616,
  0x0726, 0x0816, 0x0917, 0x0A08, 0x0B08, 0x0C08, 0x0D08, 0x0E05,
  0x0F05, 0x4000, 0x0026, 0x0108, 0x2335, 0x2535, 0x2735, 0x0E05,
  0x0F05, 0x4000, 0x0026, 0x0108, 0x2326, 0x2526, 0x2726, 0x0E05,
  0x0F05,
};

// 每个 (opcode, mode&6) 在 sp0256_datafmt 内的字段区间 [idx0, idx1]
static const int16_t sp0256_df_idx[16 * 8] = {
  -1, -1, -1, -1, -1, -1, -1, -1,
  -1, -1, -1, -1, -1, -1, -1, -1,
  17, 22, 17, 24, 25, 30, 25, 32,
  83, 94, 129, 142, 97, 108, 145, 158,
  83, 96, 129, 144, 97, 110, 145, 160,
  73, 77, 74, 77, 78, 82, 79, 82,
  33, 36, 34, 37, 38, 41, 39, 42,
  127, 128, 127, 128, 127, 128, 127, 128,
  1, 14, 1, 16, 1, 14, 1, 16,
  45, 56, 45, 58, 59, 70, 59, 72,
  161, 166, 162, 166, 169, 174, 170, 174,
  111, 116, 111, 118, 119, 124, 119, 126,
  161, 168, 162, 168, 169, 176, 170, 176,
  -1, -1, -1, -1, -1, -1, -1, -1,
  -1, -1, -1, -1, -1, -1, -1, -1,
  0, 0, 0, 0, 0, 0, 0, 0,
};

// 系数编码表 (SP0250 datasheet; 亦适用于 SP0256), ±511 范围
static const int16_t qtbl[128] = {
  0, 9, 17, 25, 33, 41, 49, 57,
  65, 73, 81, 89, 97, 105, 113, 121,
  129, 137, 145, 153, 161, 169, 177, 185,
  193, 201, 209, 217, 225, 233, 241, 249,
  257, 265, 273, 281, 289, 297, 301, 305,
  309, 313, 317, 321, 325, 329, 333, 337,
  341, 345, 349, 353, 357, 361, 365, 369,
  373, 377, 381, 385, 389, 393, 397, 401,
  405, 409, 413, 417, 421, 425, 427, 429,
  431, 433, 435, 437, 439, 441, 443, 445,
  447, 449, 451, 453, 455, 457, 459, 461,
  463, 465, 467, 469, 471, 473, 475, 477,
  479, 481, 482, 483, 484, 485, 486, 487,
  488, 489, 490, 491, 492, 493, 494, 495,
  496, 497, 498, 499, 500, 501, 502, 503,
  504, 505, 506, 507, 508, 509, 510, 511,
};

uint32_t BitRev32(uint32_t val)
{
  val = ((val & 0xFFFF0000u) >> 16) | ((val & 0x0000FFFFu) << 16);
  val = ((val & 0xFF00FF00u) >>  8) | ((val & 0x00FF00FFu) <<  8);
  val = ((val & 0xF0F0F0F0u) >>  4) | ((val & 0x0F0F0F0Fu) <<  4);
  val = ((val & 0xCCCCCCCCu) >>  2) | ((val & 0x33333333u) <<  2);
  val = ((val & 0xAAAAAAAAu) >>  1) | ((val & 0x55555555u) <<  1);
  return val;
}

// AL2 档预设英语短语 (手写 allophone 串; 标签表之外的常用词)
struct Phrase
{
  const char* name;
  const char* allophones;
};
const Phrase kPhrases[] = {
  {"HELLO",  "HH1 EH L OW"},
  {"THANK",  "TH AE NG"},
  {"YOU",    "YY UW1"},
  {"GOOD",   "GG1 UH D"},
  {"BYE",    "BB1 AY"},
  {"WORLD",  "W ER1 L D"},
  {"SPEECH", "SS PP IY CH"},
  {"TEST",   "TT1 EH SS T"},
  {"ONE",    "W AH N"},
  {"TWO",    "TT UW1"},
  {"THREE",  "TH RR1 IY"},
  {"ZERO",   "ZZ IY R OW"},
};
constexpr int kNumPhrases = (int)(sizeof(kPhrases) / sizeof(kPhrases[0]));

// ---- 12 阶滤波器 (MAME lpc12_t) ----
struct Filter
{
  int rpt = -1;         // 重复计数 (每条参数指令的帧数)
  int cnt = 0;          // 周期递减计数
  uint32_t per = 0;     // 基频周期 (采样数); 0 = 清音噪声
  uint32_t rng = 1;     // 清音 LFSR
  int amp = 0;          // 激励幅度
  int16_t f_coef[6];    // F0..F5
  int16_t b_coef[6];    // B0..B5
  int16_t z_data[6][2]; // 二阶节延时状态
  uint8_t r[16];        // 编码寄存器组
  int interp = 0;       // 帧间幅度/基频插值

  int16_t Limit(int16_t s) const
  {
    if (s > 8191)
      return 8191;
    if (s < -8192)
      return -8192;
    return s;
  }

  // 解码寄存器组 (MAME regdec): 幅度/周期 + 系数量化 + 插值使能
  void Regdec()
  {
    amp = (r[0] & 0x1F) << ((r[0] & 0xE0) >> 5);
    cnt = 0;
    per = r[1];
    auto IQ = [](uint8_t x) { return (x & 0x80) ? qtbl[0x7F & -x] : (int16_t) -qtbl[x]; };
    for (int i = 0; i < 6; i++)
    {
      b_coef[i] = IQ(r[2 + 2 * i]);
      f_coef[i] = IQ(r[3 + 2 * i]);
    }
    interp = (r[14] != 0) || (r[15] != 0);
  }

  // 生成 num_samp 个采样, 写入 out[oidx..]; 返回实际生成数 (重复计数耗尽提前返回)
  int Update(int num_samp, int16_t* out, int& oidx)
  {
    int i;
    for (i = 0; i < num_samp; i++)
    {
      bool do_int = false;
      uint16_t samp = 0;
      if (per)
      {
        if (cnt <= 0)
        {
          cnt += (int) per;
          samp = (uint16_t) amp;
          rpt--;
          do_int = interp != 0;
          for (int j = 0; j < 6; j++)
            z_data[j][1] = z_data[j][0] = 0;
        }
        else
        {
          samp = 0;
          cnt--;
        }
      }
      else
      {
        if (--cnt <= 0)
        {
          do_int = interp != 0;
          cnt = kPerNoise;
          rpt--;
          for (int j = 0; j < 6; j++)
            z_data[j][0] = z_data[j][1] = 0;
        }
        const bool bit = (rng & 1) != 0;
        rng = (rng >> 1) ^ (bit ? 0x4001u : 0u);
        samp = bit ? (uint16_t) amp : (uint16_t) -amp;
      }

      // 帧间插值: 幅度/基频寄存器沿增量轴滑动
      if (do_int)
      {
        r[0] = (uint8_t) (r[0] + r[14]);
        r[1] = (uint8_t) (r[1] + r[15]);
        amp = (r[0] & 0x1F) << ((r[0] & 0xE0) >> 5);
        per = r[1];
      }

      if (rpt <= 0)
        break;

      // 6 个二阶节 (MAME 第一种实现形式, B/2F 前馈-反馈)
      for (int j = 0; j < 6; j++)
      {
        samp = (uint16_t) (samp + ((int(b_coef[j]) * int(z_data[j][1])) >> 9));
        samp = (uint16_t) (samp + ((int(f_coef[j]) * int(z_data[j][0])) >> 8));
        z_data[j][1] = z_data[j][0];
        z_data[j][0] = (int16_t) samp;
      }

      out[oidx++] = (int16_t) (Limit((int16_t) samp) << 2);
    }
    return i;
  }
};

// ---- 芯片 (微序控制器 + 滤波器) ----
struct Chip
{
  const unsigned char* rom = nullptr; // 4KB 页 (仅第 1 页有效)

  uint32_t pc = 0;
  uint32_t stack = 0;
  uint32_t page = 0x1000 << 3; // 初始页 = 第 1 页 (位地址)
  int mode = 0;
  int ald = 0;       // 挂起的命令 (code << 4)
  int lrq = 1;       // = 0 表示可接受新命令
  int halted = 1;
  int fifo_sel = 0;  // 恒 0: 引擎无 SPB640 FIFO
  int silent = 1;

  Filter filt;

  void Reset()
  {
    pc = 0;
    stack = 0;
    page = 0x1000 << 3;
    mode = 0;
    ald = 0;
    lrq = 1;
    halted = 1;
    fifo_sel = 0;
    silent = 1;
    memset(&filt, 0, sizeof(filt));
    filt.rpt = -1;
    filt.rng = 1;
  }

  // ROM 读取: 8KB 页空间, 第 0 页空 (返 0), 第 1 页 = 4KB 掩膜 ROM
  uint8_t ReadRom(int off) const
  {
    off &= 0x1FFF;
    if (off < 0x1000)
      return 0;
    return rom[off - 0x1000];
  }

  // 位流取 len 位 (LSB-first, PC 为位地址; MAME getb 的 ROM 分支)
  uint32_t GetBits(int len)
  {
    const int idx0 = (int) (pc >> 3);
    const int idx1 = (int) ((pc + 8) >> 3);
    const uint8_t d0 = ReadRom(idx0);
    const uint8_t d1 = ReadRom(idx1);
    uint32_t data = ((uint32_t) (d1 << 8) | d0) >> (pc & 7);
    pc += (uint32_t) len;
    return data & ((1u << len) - 1);
  }

  // 装载一条命令 (ald_w 语义): 芯片停机时写入, micro() 内采
  void Start(uint8_t code)
  {
    lrq = 0;
    ald = ((int) code) << 4;
  }

  // 执行微指令直到滤波器忙 (MAME micro)
  void Micro()
  {
    while (filt.rpt <= 0)
    {
      // 停机且有挂起命令: 采用 (清全部编码寄存器)
      if (halted && !lrq)
      {
        pc = ((uint32_t) ald) | (0x1000u << 3);
        fifo_sel = 0;
        halted = 0;
        lrq = 1;
        ald = 0;
        for (int i = 0; i < 16; i++)
          filt.r[i] = 0;
      }

      // 无命令: 保持停机 (rpt 置 1 让外层继续走滤波)
      if (halted)
      {
        filt.rpt = 1;
        lrq = 1;
        ald = 0;
        for (int i = 0; i < 16; i++)
          filt.r[i] = 0;
        return;
      }

      const uint8_t immed4 = (uint8_t) GetBits(4);
      const uint8_t opcode = (uint8_t) GetBits(4);
      int repeat = 0;
      int ctrl_xfer = 0;

      switch (opcode)
      {
        // RTS / SETPAGE (immed4 != 0 为 SETPAGE, 否则 RTS->HLT)
        case 0x0:
          if (immed4)
          {
            page = BitRev32(immed4) >> 13;
          }
          else
          {
            const uint32_t btrg = stack;
            stack = 0;
            if (!btrg)
            {
              halted = 1;
              pc = 0;
              ctrl_xfer = 1;
            }
            else
            {
              pc = btrg;
              ctrl_xfer = 1;
            }
          }
          break;

        // JMP / JSR (12-bit 页内绝对地址)
        case 0xE:
        case 0xD:
        {
          const uint32_t btrg = page | (BitRev32(immed4) >> 17) | (BitRev32(GetBits(8)) >> 21);
          ctrl_xfer = 1;
          if (opcode == 0xD)
            stack = (pc + 7) & ~7u;
          pc = btrg;
          break;
        }

        // SETMODE (重复计数 MSBs + 数据块模式位)
        case 0x1:
          mode = ((immed4 & 8) >> 2) | (immed4 & 4) | ((immed4 & 3) << 4);
          break;

        // 数据块指令: 重复计数 = immed4 | 模式高 2 位
        default:
          repeat = immed4 | (mode & 0x30);
          break;
      }
      if (opcode != 1)
        mode &= 0xF;

      if (ctrl_xfer)
      {
        fifo_sel = (pc == kFifoAddr) ? 1 : 0;
        if (fifo_sel)
          return; // 无 FIFO 支持 (正常 ROM 不会跳入 FIFO 地址)
        continue;
      }

      if (!repeat)
        continue;

      filt.rpt = repeat + 1;

      int i = (opcode << 3) | (mode & 6);
      const int idx0 = sp0256_df_idx[i];
      const int idx1 = sp0256_df_idx[i + 1];

      // 按格式表逐字段更新寄存器
      for (i = idx0; i <= idx1; i++)
      {
        const uint16_t cr = sp0256_datafmt[i];
        const int len = cr & 15;
        const int shf = (cr >> 4) & 15;
        const int prm = (cr >> 8) & 15;
        const bool clra = (cr & 0x8000u) != 0;
        const bool clr5 = (cr & 0x4000u) != 0;
        const bool delta = (cr & 0x1000u) != 0;
        const bool field = (cr & 0x2000u) != 0;

        int value = 0;
        if (clra)
        {
          for (int j = 0; j < 16; j++)
            filt.r[j] = 0;
          silent = 1;
        }
        if (clr5)
          filt.r[B5] = filt.r[F5] = 0;

        if (!len)
          continue;

        value = (int) GetBits(len);
        if (delta && (value & (1 << (len - 1))))
          value |= -1 << len; // 增量更新: 符号扩展
        if (shf)
          value <<= shf;

        silent = 0;

        if (field)
        {
          filt.r[prm] = (uint8_t) (filt.r[prm] & ~(~0 << shf));
          filt.r[prm] = (uint8_t) (filt.r[prm] | value);
          continue;
        }
        if (delta)
        {
          filt.r[prm] = (uint8_t) (filt.r[prm] + value);
          continue;
        }
        filt.r[prm] = (uint8_t) value;
      }

      // PAUSE: 等效周期 + 静音
      if (opcode == 0xF)
      {
        silent = 1;
        filt.r[1] = (uint8_t) kPerPause;
      }

      filt.Regdec();
      break;
    }
  }
};

// 大小写不敏感标签查码 (-1 = 未收录)
int CodeForLabel(const sp0256::Variant& V, const std::string& upper)
{
  for (int i = 0; i < V.nLabels; i++)
  {
    std::string label = V.labels[i];
    for (char& c : label)
      c = (char) std::toupper((unsigned char) c);
    if (label == upper)
      return i;
  }
  return -1;
}

// 连续渲染一组 (ROM, 码): 同源码连续渲染 (滤波器跨词持续), 换源时重置芯片
// 起新语音段 (混合 AL2 音素与 012 单词时自然衔接); 词尾滤波器振铃尾。
bool RenderItems(const std::vector<std::pair<const sp0256::Variant*, int>>& items,
                 std::vector<float>& out)
{
  Chip chip;
  chip.rom = nullptr;
  chip.Reset();

  out.clear();
  out.reserve(kMaxSamples);

  size_t tok = 0;
  bool tail = false;
  int tailLeft = 0;

  for (int n = 0; n < kMaxSamples; n++)
  {
    if (chip.filt.rpt <= 0)
      chip.Micro();

    if (chip.halted && chip.lrq) // 真正停机 (无挂起命令)
    {
      if (tok < items.size())
      {
        const sp0256::Variant* V = items[tok].first;
        if (chip.rom != V->data)
        {
          chip.rom = V->data;
          chip.Reset(); // 跨 ROM: 新语音段重新起振
        }
        chip.Start((uint8_t) items[tok].second);
        ++tok;
        continue; // 下一轮 micro() 采用命令
      }
      if (!tail)
      {
        tail = true;
        tailLeft = kTailSamples;
      }
      if (--tailLeft <= 0)
        break;
    }

    int16_t s = 0;
    if (chip.silent && chip.filt.rpt <= 0)
    {
      s = 0; // MAME 静默漏斗: 不推进滤波器
    }
    else
    {
      int oidx = 0;
      chip.filt.Update(1, &s, oidx);
    }
    out.push_back((float) s * (1.0f / 32768.0f));
  }
  return !out.empty();
}

} // namespace

double Sp0256Engine::ClockFor(float speedScale)
{
  return kDefaultClock * (double) std::clamp(speedScale, 0.25f, 4.0f);
}

double Sp0256Engine::RateFor(float speedScale)
{
  return ClockFor(speedScale) / 312.0;
}

bool Sp0256Engine::Render(const std::string& text, int variant, float speedScale,
                          std::vector<float>& out)
{
  out.clear();
  // 采样数与时标无关 (芯片周期数以采样计); speedScale 只经 RateFor 影响时长/音高
  (void) speedScale;

  std::vector<std::pair<const sp0256::Variant*, int>> items;

  if (variant == kPhonemeVariant)
  {
    // 音素模式: 标签输入 (AL2 allophone / 012 单词 / 预设短语), 不接受数字码;
    // 按标签来源记录 (ROM, 码), 未收录 token 与数字码一样跳过。
    const sp0256::Variant& VAl2 = sp0256::kVariants[kAl2Rom];
    const sp0256::Variant& V012 = sp0256::kVariants[kIntellivRom];

    std::string token;
    for (size_t i = 0; i <= text.size(); ++i)
    {
      const bool sep = (i == text.size()) || std::isspace((unsigned char) text[i]);
      if (sep)
      {
        if (!token.empty())
        {
          std::string up = token;
          token.clear();
          for (char& c : up)
            c = (char) std::toupper((unsigned char) c);

          int code = CodeForLabel(VAl2, up);
          if (code >= 0)
          {
            items.push_back({&VAl2, code});
          }
          else if ((code = CodeForLabel(V012, up)) >= 0)
          {
            items.push_back({&V012, code});
          }
          else
          {
            // AL2 预设英语短语展开为 allophone 串 (不覆盖既有标签)
            for (int p = 0; p < kNumPhrases; p++)
            {
              if (up == kPhrases[p].name)
              {
                std::string ph = kPhrases[p].allophones, sub;
                for (size_t j = 0; j <= ph.size(); ++j)
                {
                  const bool s2 = (j == ph.size()) || std::isspace((unsigned char) ph[j]);
                  if (s2)
                  {
                    if (!sub.empty())
                    {
                      const int subCode = CodeForLabel(VAl2, sub);
                      if (subCode >= 0)
                        items.push_back({&VAl2, subCode});
                      sub.clear();
                    }
                  }
                  else
                  {
                    sub.push_back((char) std::toupper((unsigned char) ph[j]));
                  }
                }
                break;
              }
            }
          }
        }
        continue;
      }
      token.push_back((char) std::toupper((unsigned char) text[i]));
    }
    if (items.empty())
      return false;
  }
  else if (variant == kTextVariant)
  {
    // 文本模式: 英文文本 -> CTS256A-AL2 控制器转 allophone 码, 用 AL2 ROM 渲染
    std::vector<int> codes;
    if (!Cts256aEngine::Convert(text, codes))
      return false;
    const sp0256::Variant& VAl2 = sp0256::kVariants[kAl2Rom];
    for (int c : codes)
      items.push_back({&VAl2, c});
  }
  else
  {
    return false;
  }

  std::vector<float> raw;
  if (!RenderItems(items, raw) || raw.empty())
    return false;

  out.swap(raw);

  // 峰值归一到 0.85 (与其他引擎一致); 纯静音视为无可渲染内容
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