// Cts256aEngine.cpp — CTS256A-AL2 TTS 控制器仿真
//
// 移植对照 GmEsoft SP0256_CTS256A-AL2 的 CTS256A_AL2.cpp (GPL-3.0-or-later):
//   - 内存映射/触发逻辑/补丁逐条对应 (见头文件注释);
//   - 输入输出从 istream/ostream 换成文本缓冲 + 码集合;
//   - 停止条件: eof 后 eofctr (199999 次读) 清零, 外加指令数硬上限兜底。
#include "Cts256aEngine.h"

#include "cts256a/Cts256aRoms.h"
#include "cts256a/Tms7000Cpu.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <vector>

namespace orm
{

namespace
{

// 参考仿真常量
constexpr uint8_t kInitCounter = 6;      // 开机 O.K. 的输出数 (无输出静默抑制)
constexpr uint32_t kEofReload = 199999;  // 文本耗尽后剩余的读计数
constexpr long kMaxSteps = 3000000;      // 指令数硬上限 (防输错/跑飞)
constexpr int kMaxCodes = 4096;          // 输出码数硬上限

// 芯片系统 (Tms7000Io 实现): 内存映射 + IO + 文本/输出缓冲
struct CtsSystem : public cts256a::Tms7000Io
{
  cts256a::Tms7000Cpu cpu{nullptr};

  std::vector<uint8_t> text;   // 大写化后的输入文本
  size_t textPos = 0;
  uint8_t ram[0x800] = {0};    // 0x3000-0x37FF
  uint8_t bport = 0;           // BPORT (DSR/BUSY)
  uint8_t initctr = kInitCounter;
  bool eof = false;
  uint32_t eofctr = 0;
  bool exit = false;           // 停止计时器到点
  std::vector<int> codes;      // 收集的 SP0256 allophone 码

  CtsSystem() { cpu.SetIo(this); }

  uint8_t ReadData(uint16_t addr) override
  {
    // 参考: 每次外存读触发 IRQ1 (输出中断)
    cpu.TrigIRQ(0x02);

    if (!eof)
    {
      // POLL/ENDPOL 且输出缓冲空 -> 触发 IRQ3 (输入中断) 拉下一个字符
      if (addr == 0xF105 || addr == 0xF10C || addr == 0xF11C || addr == 0xF12F)
      {
        if (!initctr && (bport & 0x01) && cpu.MemRead(7) == cpu.MemRead(9))
          cpu.TrigIRQ(0x08);
      }
    }
    else if (!--eofctr)
    {
      exit = true;
    }

    // 输出缓冲高水位补丁: 强制每个 allophone 即时送出
    if (addr == 0xF33E)
      return 0;

    if (addr >= 0xF000) // 4KB 掩膜 ROM
      return cts256a::kRom[addr & 0x0FFF];

    if (addr < 0x1000) // 并行数据 (文本) 读
    {
      if (textPos < text.size())
        return text[textPos++];
      eof = true;
      eofctr = kEofReload;
      return 0x0D; // EOF -> CR 分隔符
    }

    if (addr < 0x2000) // UART 参数 (并行模式)
      return 0;

    if (addr < 0x3000) // SP0256 输出口 (读)
      return 0xFF;

    if (addr < 0x3800) // 内部 RAM
      return ram[addr & 0x07FF];

    return 0xFF;
  }

  void WriteData(uint16_t addr, uint8_t data) override
  {
    if (addr >= 0x2000 && addr < 0x3000)
    {
      // SP0256 输出: 抑制开机 O.K. (前 kInitCounter 个), 其余收集为 allophone 码
      if (initctr)
        --initctr;
      else if ((int) codes.size() < kMaxCodes)
        codes.push_back(data & 0x3F);
      return;
    }
    if (addr >= 0x3000 && addr < 0x3800)
    {
      ram[addr & 0x07FF] = data;
      return;
    }
  }

  uint8_t InPort(uint8_t addr) override
  {
    switch (addr)
    {
      case 0x04: // APORT: 内部 RAM 缓冲 + 并行模式
        return 0x10;
      case 0x06: // BPORT
        return 0xFF;
      default:
        return 0xFF;
    }
  }

  void OutPort(uint8_t addr, uint8_t data) override
  {
    if (addr == 0x06)
      bport = data;
  }
};

} // namespace

bool Cts256aEngine::Convert(const std::string& text, std::vector<int>& codes)
{
  codes.clear();

  CtsSystem sys;

  // 字符处理: 大写化, \r/\n 归一为空格 (参考仿真逐字符 toupper)
  sys.text.reserve(text.size());
  for (char c : text)
  {
    if (c == '\r' || c == '\n')
      c = ' ';
    sys.text.push_back((uint8_t) std::toupper((unsigned char) c));
  }

  sys.cpu.Reset();
  for (long step = 0; step < kMaxSteps && !sys.exit && !sys.cpu.Stopped(); ++step)
    sys.cpu.Step();

  codes = std::move(sys.codes);
  return !codes.empty();
}

} // namespace orm