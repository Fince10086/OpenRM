// Tms7000Cpu.h — TMS7000 8 位微控制器核 (CTS256A-AL2 配套 CPU)
//
// 逐行移植自 GmEsoft SP0256_CTS256A-AL2 的 TMS7000CPU.cpp (Michel Bernard,
// GPL-3.0-or-later)。保留参考实现的所有状态与怪癖 (DECD 的指针步进、EINT 置
// 0xF0、MOVP 的 PN 特判等), 以保证 CTS256A ROM 程序行为逐字节一致。
// 依赖一个外部内存/IO 接口 (Tms7000Io): 0x000-0x0FF 是芯片内部 RAM (本核内),
// 0x100-0x1FF 空读 0xFF, 其余地址(含取指/数据)经 Tms7000Io 转发。
#pragma once

#include <cstdint>

namespace orm
{
namespace cts256a
{

// 外部内存与 IO 回调 (由 CTS256A 系统层实现)
class Tms7000Io
{
public:
  virtual ~Tms7000Io() = default;
  virtual uint8_t ReadData(uint16_t addr) = 0;   // 外存/ROM 读 (可能触发芯片中断)
  virtual void WriteData(uint16_t addr, uint8_t data) = 0;
  virtual uint8_t InPort(uint8_t addr) = 0;      // 外设口读
  virtual void OutPort(uint8_t addr, uint8_t data) = 0;
};

// PSW 位域 (与参考实现 st_t 一致, LSB-first: b0..b3 / I 位4 / Z 位5 / N 位6 / C 位7)
union Psw
{
  uint8_t raw;
  struct
  {
    unsigned b0 : 1, b1 : 1, b2 : 1, b3 : 1, i : 1, z : 1, n : 1, c : 1;
  } f;
};

class Tms7000Cpu
{
public:
  explicit Tms7000Cpu(Tms7000Io* io = nullptr);

  void SetIo(Tms7000Io* io) { io_ = io; }
  Tms7000Io* GetIo() const { return io_; }

  // 芯片复位: 状态清 0, 栈指针 >01, PC = 0xFFFE/0xFFFF 复位向量
  void Reset();

  // 执行一条指令
  void Step();

  // 挂起中断 (位掩码: 0x02 = IRQ1 输出, 0x08 = IRQ3 输入)
  void TrigIRQ(uint8_t irq) { irqPending_ |= irq; }

  // 通用内存读 (内部 RAM / 外部转发) — 系统层查 CPU 内部单元 (如 data[7]/[9])
  uint8_t MemRead(uint16_t addr);
  uint16_t PC() const { return pc_; }

  // 遇到未实现/非法指令 (参考实现走 stop()) 时置位
  bool Stopped() const { return stopped_; }
  uint16_t StopPc() const { return stopPc_; }   // 调试用
  uint8_t StopOp() const { return stopOp_; }

private:
  void stop()
  {
    stopPc_ = pc_;
    stopOp_ = (uint8_t) Read(pc_);
    stopped_ = true;
  }
  Tms7000Io* io_;
  uint8_t pad0_ = 0;      // data_ 前垫字节: DECD/MOVD 对 data[0] 反向一字节的访问落在这里
  uint8_t data_[256];    // 内部 RAM (A=0 B=1 为寄存器, 栈在高端)
  uint8_t* a_;           // = &data_[0]
  uint8_t* b_;           // = &data_[1]
  uint8_t sp_ = 1;       // 栈指针
  Psw st_{};             // PSW
  uint16_t pc_ = 0;
  uint8_t iocnt0_ = 0;   // P0: 中断控制
  uint8_t iocnt1_ = 0;   // P16
  uint8_t irqPending_ = 0;
  bool stopped_ = false;
  uint16_t stopPc_ = 0;  // 调试: stop() 触发时的 PC
  uint8_t stopOp_ = 0;   // 调试: 触发停止的 opcode

  uint8_t& ByteBefore(uint8_t* p) { return (p > data_) ? *(p - 1) : pad0_; }

  uint8_t ReadCode() { return MemRead(pc_++); }
  void Write(uint16_t addr, uint8_t data);
  uint8_t Read(uint16_t addr);
  uint8_t In(uint8_t addr);
  uint8_t Out(uint8_t addr, uint8_t data);

  void SimIntDetect();
  void SimIntProcess();
  void SimOp(uint8_t opcode);
  // 注意: 取数必须分语句, 不能写 pc_ + ReadCode() 之类 — C++ 运算符求值序不确定,
  // 先读 pc_ 会让偏移量加在取数前的地址上 (参考实现以独立语句保证顺序)
  uint16_t Ladr()
  {
    const uint8_t hi = ReadCode();
    return (uint16_t) ((hi << 8) | ReadCode());
  }
  uint16_t Sadr()
  {
    const int8_t d = (int8_t) ReadCode();
    return (uint16_t) (pc_ + d);
  }
};

} // namespace cts256a
} // namespace orm