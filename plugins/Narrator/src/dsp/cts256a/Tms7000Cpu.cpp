// Tms7000Cpu.cpp — TMS7000 微控制器核 (CTS256A-AL2 配套 CPU) 忠实移植
//
// 对照 GmEsoft SP0256_CTS256A-AL2 TMS7000CPU.cpp (GPL-3.0-or-later):
//   - 寄存器 A/B 即内部 RAM data[0]/data[1]; PSW 位域 LSB-first (c=bit7);
//   - 内存统一编址: 取指与数据读同一路径 (<0x100 内部 RAM, >=0x200 外部);
//   - 中断: IRQ1/IRQ3 经 IOCNT0 引脚位挂起, PSW.I 使能时取向量
//     0xFFFE-2*trap; 定时器为参考实现中的空桩 (CTS256A 并行模式用不到);
//   - DECD 的指针步进、EINT 的 st|=0xF0、MOVP 的 PN 特判等与参考逐字节一致;
//     DECD 对 data[0] 反向一字节的访问在参考里落在 irq 字段上, 这里用前垫
//     零字节等价 (该路径几乎恒读 0)。
#include "Tms7000Cpu.h"

#include <cstring>
#include <cstdio>

namespace orm
{
namespace cts256a
{

namespace
{

// 指令助记符枚举 + 表中唯一的编码单元 (与参考 instr_t 一致)
struct TosInstr
{
  int mnemon;
  int opn1;
  int opn2;
};

enum
{
  kAdc = 0, kAdd, kAnd, kAndp, kBtjo, kBtjop, kBtjz, kBtjzp,
  kBr, kCall, kClr, kClrc, kCmp, kCmpa, kDac, kDec,
  kDecd, kDint, kDjnz, kDsb, kEint, kIdle, kInc, kInv,
  kJmp, kJn, kJz, kJc, kJp, kJpz, kJnz, kJnc,
  kLda, kLdsp, kMov, kMovd, kMovp, kMpy, kNop, kOr,
  kOrp, kPop, kPush, kReti, kRets, kRl, kRlc, kRr,
  kRrc, kSbb, kSetc, kSta, kStsp, kSub, kSwap, kTrap,
  kTsta, kTstb, kXchb, kXor, kXorp, kDb,
};

enum
{
  kOpNone = 0, kOpA, kOpB, kOpRn, kOpPn, kOpByte, kOpWord,
  kOpWordB, kOpOfst, kOpAddr, kOpAddrB, kOpAtrn, kOpSt,
  kOpN, kOpNtrap, kOpOpcode,
};

// 处理器指令表 (0x00..0xFF 逐字节; 未实现条目在参考实现里走 stop())
const TosInstr kInstrTable[] = {
  {kNop, 0, 0}, {kIdle, 0, 0}, {kDb, kOpOpcode, 0}, {kDb, kOpOpcode, 0},
  {kDb, kOpOpcode, 0}, {kEint, 0, 0}, {kDint, 0, 0}, {kSetc, 0, 0},
  {kPop, kOpSt, 0}, {kStsp, 0, 0}, {kRets, 0, 0}, {kReti, 0, 0},
  {kDb, kOpOpcode, 0}, {kLdsp, 0, 0}, {kPush, kOpSt, 0}, {kDb, kOpOpcode, 0},
  {kDb, kOpOpcode, 0}, {kDb, kOpOpcode, 0}, {kMov, kOpRn, kOpA}, {kAnd, kOpRn, kOpA},
  {kOr, kOpRn, kOpA}, {kXor, kOpRn, kOpA}, {kBtjo, kOpRn, kOpA}, {kBtjz, kOpRn, kOpA},
  {kAdd, kOpRn, kOpA}, {kAdc, kOpRn, kOpA}, {kSub, kOpRn, kOpA}, {kSbb, kOpRn, kOpA},
  {kMpy, kOpRn, kOpA}, {kCmp, kOpRn, kOpA}, {kDac, kOpRn, kOpA}, {kDsb, kOpRn, kOpA},
  {kDb, kOpOpcode, 0}, {kDb, kOpOpcode, 0}, {kMov, kOpByte, kOpA}, {kAnd, kOpByte, kOpA},
  {kOr, kOpByte, kOpA}, {kXor, kOpByte, kOpA}, {kBtjo, kOpByte, kOpA}, {kBtjz, kOpByte, kOpA},
  {kAdd, kOpByte, kOpA}, {kAdc, kOpByte, kOpA}, {kSub, kOpByte, kOpA}, {kSbb, kOpByte, kOpA},
  {kMpy, kOpByte, kOpA}, {kCmp, kOpByte, kOpA}, {kDac, kOpByte, kOpA}, {kDsb, kOpByte, kOpA},
  {kDb, kOpOpcode, 0}, {kDb, kOpOpcode, 0}, {kMov, kOpRn, kOpB}, {kAnd, kOpRn, kOpB},
  {kOr, kOpRn, kOpB}, {kXor, kOpRn, kOpB}, {kBtjo, kOpRn, kOpB}, {kBtjz, kOpRn, kOpB},
  {kAdd, kOpRn, kOpB}, {kAdc, kOpRn, kOpB}, {kSub, kOpRn, kOpB}, {kSbb, kOpRn, kOpB},
  {kMpy, kOpRn, kOpB}, {kCmp, kOpRn, kOpB}, {kDac, kOpRn, kOpB}, {kDsb, kOpRn, kOpB},
  {kDb, kOpOpcode, 0}, {kDb, kOpOpcode, 0}, {kMov, kOpRn, kOpRn}, {kAnd, kOpRn, kOpRn},
  {kOr, kOpRn, kOpRn}, {kXor, kOpRn, kOpRn}, {kBtjo, kOpRn, kOpRn}, {kBtjz, kOpRn, kOpRn},
  {kAdd, kOpRn, kOpRn}, {kAdc, kOpRn, kOpRn}, {kSub, kOpRn, kOpRn}, {kSbb, kOpRn, kOpRn},
  {kMpy, kOpRn, kOpRn}, {kCmp, kOpRn, kOpRn}, {kDac, kOpRn, kOpRn}, {kDsb, kOpRn, kOpRn},
  {kDb, kOpOpcode, 0}, {kDb, kOpOpcode, 0}, {kMov, kOpByte, kOpB}, {kAnd, kOpByte, kOpB},
  {kOr, kOpByte, kOpB}, {kXor, kOpByte, kOpB}, {kBtjo, kOpByte, kOpB}, {kBtjz, kOpByte, kOpB},
  {kAdd, kOpByte, kOpB}, {kAdc, kOpByte, kOpB}, {kSub, kOpByte, kOpB}, {kSbb, kOpByte, kOpB},
  {kMpy, kOpByte, kOpB}, {kCmp, kOpByte, kOpB}, {kDac, kOpByte, kOpB}, {kDsb, kOpByte, kOpB},
  {kDb, kOpOpcode, 0}, {kDb, kOpOpcode, 0}, {kMov, kOpB, kOpA}, {kAnd, kOpB, kOpA},
  {kOr, kOpB, kOpA}, {kXor, kOpB, kOpA}, {kBtjo, kOpB, kOpA}, {kBtjz, kOpB, kOpA},
  {kAdd, kOpB, kOpA}, {kAdc, kOpB, kOpA}, {kSub, kOpB, kOpA}, {kSbb, kOpB, kOpA},
  {kMpy, kOpB, kOpA}, {kCmp, kOpB, kOpA}, {kDac, kOpB, kOpA}, {kDsb, kOpB, kOpA},
  {kDb, kOpOpcode, 0}, {kDb, kOpOpcode, 0}, {kMov, kOpByte, kOpRn}, {kAnd, kOpByte, kOpRn},
  {kOr, kOpByte, kOpRn}, {kXor, kOpByte, kOpRn}, {kBtjo, kOpByte, kOpRn}, {kBtjz, kOpByte, kOpRn},
  {kAdd, kOpByte, kOpRn}, {kAdc, kOpByte, kOpRn}, {kSub, kOpByte, kOpRn}, {kSbb, kOpByte, kOpRn},
  {kMpy, kOpByte, kOpRn}, {kCmp, kOpByte, kOpRn}, {kDac, kOpByte, kOpRn}, {kDsb, kOpByte, kOpRn},
  {kMovp, kOpPn, kOpA}, {kDb, kOpOpcode, 0}, {kMovp, kOpA, kOpPn}, {kAndp, kOpA, kOpPn},
  {kOrp, kOpA, kOpPn}, {kXorp, kOpA, kOpPn}, {kBtjop, kOpA, kOpPn}, {kBtjzp, kOpA, kOpPn},
  {kMovd, kOpWord, kOpRn}, {kDb, kOpOpcode, 0}, {kLda, kOpAddr, 0}, {kSta, kOpAddr, 0},
  {kBr, kOpAddr, 0}, {kCmpa, kOpAddr, 0}, {kCall, kOpAddr, 0}, {kDb, kOpOpcode, 0},
  {kDb, kOpOpcode, 0}, {kMovp, kOpPn, kOpB}, {kMovp, kOpB, kOpPn}, {kAndp, kOpB, kOpPn},
  {kOrp, kOpB, kOpPn}, {kXorp, kOpB, kOpPn}, {kBtjop, kOpB, kOpPn}, {kBtjzp, kOpB, kOpPn},
  {kMovd, kOpRn, kOpRn}, {kDb, kOpOpcode, 0}, {kLda, kOpAtrn, 0}, {kSta, kOpAtrn, 0},
  {kBr, kOpAtrn, 0}, {kCmpa, kOpAtrn, 0}, {kCall, kOpAtrn, 0}, {kDb, kOpOpcode, 0},
  {kDb, kOpOpcode, 0}, {kDb, kOpOpcode, 0}, {kMovp, kOpByte, kOpPn}, {kAndp, kOpByte, kOpPn},
  {kOrp, kOpByte, kOpPn}, {kXorp, kOpByte, kOpPn}, {kBtjop, kOpByte, kOpPn}, {kBtjzp, kOpByte, kOpPn},
  {kMovd, kOpWordB, kOpRn}, {kDb, kOpOpcode, 0}, {kLda, kOpAddrB, 0}, {kSta, kOpAddrB, 0},
  {kBr, kOpAddrB, 0}, {kCmpa, kOpAddrB, 0}, {kCall, kOpAddrB, 0}, {kDb, kOpOpcode, 0},
  {kTsta, 0, 0}, {kDb, kOpOpcode, 0}, {kDec, kOpA, 0}, {kInc, kOpA, 0},
  {kInv, kOpA, 0}, {kClr, kOpA, 0}, {kXchb, kOpA, 0}, {kSwap, kOpA, 0},
  {kPush, kOpA, 0}, {kPop, kOpA, 0}, {kDjnz, kOpA, 0}, {kDecd, kOpA, 0},
  {kRr, kOpA, 0}, {kRrc, kOpA, 0}, {kRl, kOpA, 0}, {kRlc, kOpA, 0},
  {kMov, kOpA, kOpB}, {kTstb, 0, 0}, {kDec, kOpB, 0}, {kInc, kOpB, 0},
  {kInv, kOpB, 0}, {kClr, kOpB, 0}, {kXchb, kOpB, 0}, {kSwap, kOpB, 0},
  {kPush, kOpB, 0}, {kPop, kOpB, 0}, {kDjnz, kOpB, 0}, {kDecd, kOpB, 0},
  {kRr, kOpB, 0}, {kRrc, kOpB, 0}, {kRl, kOpB, 0}, {kRlc, kOpB, 0},
  {kMov, kOpA, kOpRn}, {kMov, kOpB, kOpRn}, {kDec, kOpRn, 0}, {kInc, kOpRn, 0},
  {kInv, kOpRn, 0}, {kClr, kOpRn, 0}, {kXchb, kOpRn, 0}, {kSwap, kOpRn, 0},
  {kPush, kOpRn, 0}, {kPop, kOpRn, 0}, {kDjnz, kOpRn, 0}, {kDecd, kOpRn, 0},
  {kRr, kOpRn, 0}, {kRrc, kOpRn, 0}, {kRl, kOpRn, 0}, {kRlc, kOpRn, 0},
  {kJmp, kOpOfst, 0}, {kJn, kOpOfst, 0}, {kJz, kOpOfst, 0}, {kJc, kOpOfst, 0},
  {kJp, kOpOfst, 0}, {kJpz, kOpOfst, 0}, {kJnz, kOpOfst, 0}, {kJnc, kOpOfst, 0},
  {kTrap, kOpNtrap, 0}, {kTrap, kOpNtrap, 0}, {kTrap, kOpNtrap, 0}, {kTrap, kOpNtrap, 0},
  {kTrap, kOpNtrap, 0}, {kTrap, kOpNtrap, 0}, {kTrap, kOpNtrap, 0}, {kTrap, kOpNtrap, 0},
  {kTrap, kOpNtrap, 0}, {kTrap, kOpNtrap, 0}, {kTrap, kOpNtrap, 0}, {kTrap, kOpNtrap, 0},
  {kTrap, kOpNtrap, 0}, {kTrap, kOpNtrap, 0}, {kTrap, kOpNtrap, 0}, {kTrap, kOpNtrap, 0},
  {kTrap, kOpNtrap, 0}, {kTrap, kOpNtrap, 0}, {kTrap, kOpNtrap, 0}, {kTrap, kOpNtrap, 0},
  {kTrap, kOpNtrap, 0}, {kTrap, kOpNtrap, 0}, {kTrap, kOpNtrap, 0}, {kTrap, kOpNtrap, 0},
  // 结尾哨兵
  {kNop, 0, 0},
};

} // namespace

Tms7000Cpu::Tms7000Cpu(Tms7000Io* io) : io_(io)
{
  // DECD/MOVD 对 data[0] 反向一字节的访问落在 pad0_ 上 (参考实现为该位置的 irq 字段)
  pad0_ = 0;
  std::memset(data_, 0, sizeof data_);
  std::memset(&st_, 0, sizeof st_);
  a_ = data_;
  b_ = data_ + 1;
  sp_ = 1;
  iocnt0_ = 0;
  iocnt1_ = 0;
  pc_ = 0;
  irqPending_ = 0;
  stopped_ = false;
}

void Tms7000Cpu::Reset()
{
  // 1) 状态寄存器清 0 (清除全局中断使能 I)
  st_.raw = 0;
  // 2) IOCNT0 清 0 (禁用 INT1*/INT2/INT3*)
  iocnt0_ = 0;
  // 3) IOCNT1 清 0
  iocnt1_ = 0;
  // 4) 复位前 PC 高低字节存入 R0/R1 (寄存器 A/B)
  *a_ = (uint8_t) (pc_ >> 8);
  *b_ = (uint8_t) (pc_ & 0xFF);
  // 5) 栈指针初始化为 >01
  sp_ = 1;
  // 6) 复位向量取自 0xFFFE/0xFFFF
  pc_ = (uint16_t) ((Read(0xFFFE) << 8) | Read(0xFFFF));
  irqPending_ = 0;
  stopped_ = false;
}

uint8_t Tms7000Cpu::Read(uint16_t addr)
{
  if (addr < 0x100)
    return data_[addr];
  if (addr >= 0x200 && io_)
    return io_->ReadData(addr);
  return 0xFF;
}

void Tms7000Cpu::Write(uint16_t addr, uint8_t data)
{
  if (addr < 0x100)
    data_[addr] = data;
  else if (addr >= 0x200 && io_)
    io_->WriteData(addr, data);
}

uint8_t Tms7000Cpu::In(uint8_t addr)
{
  switch (addr)
  {
    case 0:
      return iocnt0_;
    case 1:
      return iocnt1_;
    default:
      if (io_)
        return io_->InPort(addr);
  }
  return 0xFF;
}

uint8_t Tms7000Cpu::Out(uint8_t addr, uint8_t data)
{
  switch (addr)
  {
    case 0: // IOCNT0
      iocnt0_ = (uint8_t) ((data & ~0x2A) | (iocnt0_ & 0x2A & ~data));
      break;
    case 16: // IOCNT1
      iocnt1_ = (uint8_t) ((data & ~0xFA) | (iocnt1_ & 0x0A & ~data));
      break;
    default:
      if (io_)
        io_->OutPort(addr, data);
  }
  return data;
}

uint8_t Tms7000Cpu::MemRead(uint16_t addr)
{
  return Read(addr);
}

void Tms7000Cpu::SimIntDetect()
{
  if (irqPending_ & 0x02) // IRQ1 (输出)
  {
    iocnt0_ |= 0x02;
    irqPending_ &= ~0x02;
  }
  if (irqPending_ & 0x08) // IRQ3 (输入)
  {
    iocnt0_ |= 0x20;
    irqPending_ &= ~0x08;
  }
}

void Tms7000Cpu::SimIntProcess()
{
  Psw psw;
  psw.raw = st_.raw;
  if (psw.f.i)
  {
    uint16_t itrap;
    if ((iocnt0_ & 0x03) == 0x03)
      itrap = 1;
    else if ((iocnt0_ & 0x30) == 0x30)
      itrap = 3;
    else
      return;

    itrap = (uint16_t) (0xFFFE - (itrap << 1));
    data_[++sp_] = st_.raw;
    data_[++sp_] = (uint8_t) (pc_ >> 8);
    data_[++sp_] = (uint8_t) (pc_ & 0xFF);
    psw.f.i = 0;
    st_.raw = psw.raw;
    pc_ = (uint16_t) ((Read(itrap) << 8) | Read(itrap + 1));
  }
}

void Tms7000Cpu::Step()
{
  // 取指执行单条指令 + 中断检测/服务 (参考 sim())
  const uint8_t opcode = ReadCode();
  SimOp(opcode);
  SimIntDetect();
  SimIntProcess();
}

void Tms7000Cpu::SimOp(const uint8_t opcode)
{
  const TosInstr& instr = kInstrTable[opcode];
  uint8_t* pOpn1 = nullptr;
  uint8_t* pOpn2 = nullptr;
  uint8_t opn1 = 0, opn2 = 0, byte = 0;
  uint16_t res;
  uint16_t word = 0;

  switch (instr.opn1)
  {
    case kOpNone:
      break;
    case kOpA:
      pOpn1 = a_;
      break;
    case kOpB:
      pOpn1 = b_;
      break;
    case kOpRn: // Rn
      pOpn1 = data_ + ReadCode();
      break;
    case kOpPn: // Pn
      opn1 = ReadCode();
      break;
    case kOpByte: // %>byte
      opn1 = ReadCode();
      break;
    case kOpWord: // %>word
      word = Ladr();
      break;
    case kOpWordB: // %>word(B)
      word = (uint16_t) (Ladr() + *b_);
      break;
    case kOpOfst: // PC+offs
      word = Sadr();
      break;
    case kOpAddr: // @>addr
      word = Ladr();
      break;
    case kOpAddrB: // &>addr(B)
      word = (uint16_t) (Ladr() + *b_);
      break;
    case kOpAtrn: // *Rn
      byte = ReadCode();
      word = (uint16_t) ((data_[(uint8_t) (byte - 1)] << 8) + data_[byte]);
      break;
    case kOpSt: // ST
      opn1 = st_.raw;
      break;
    case kOpN:
      break;
    case kOpNtrap: // TRAP n
      word = (uint16_t) (0xFFFE - ((0xFF - opcode) << 1));
      word = (uint16_t) ((Read(word) << 8) | Read(word + 1));
      break;
    case kOpOpcode: // DB opcode
      stop();
      break;
    default:
      stop();
  }

  if (pOpn1)
    opn1 = *pOpn1;

  switch (instr.opn2)
  {
    case kOpNone:
      break;
    case kOpA:
      pOpn2 = a_;
      break;
    case kOpB:
      pOpn2 = b_;
      break;
    case kOpRn:
      pOpn2 = data_ + ReadCode();
      break;
    case kOpPn:
      opn2 = ReadCode();
      break;
    case kOpByte:
      opn2 = ReadCode();
      break;
    case kOpWord:
      word = Ladr();
      break;
    case kOpWordB:
      word = (uint16_t) (Ladr() + *b_);
      break;
    case kOpOfst:
      word = Sadr();
      break;
    case kOpAddr:
      word = Ladr();
      break;
    case kOpAddrB:
      word = (uint16_t) (Ladr() + *b_);
      break;
    case kOpAtrn:
      byte = ReadCode();
      word = (uint16_t) ((data_[(uint8_t) (byte - 1)] << 8) + data_[byte]);
      break;
    case kOpSt:
      opn2 = st_.raw;
      break;
    case kOpN:
      break;
    case kOpNtrap:
      word = (uint16_t) (0xFFFE - ((0xFF - opcode) << 1));
      word = (uint16_t) ((Read(word) << 8) | Read(word + 1));
      break;
    case kOpOpcode:
      stop();
      break;
    default:
      stop();
  }

  if (pOpn2)
    opn2 = *pOpn2;

  Psw psw;
  psw.raw = st_.raw;
  // 读取当前进位 (指令内部分更新后立即用, 与参考实现一致)
  const auto c0 = [&]() -> unsigned { return psw.f.c; };

  switch (instr.mnemon)
  {
    case kAdc: // 带进位加
      res = (uint16_t) (opn2 + opn1 + c0());
      opn2 = (uint8_t) res;
      psw.f.c = (res >> 8) & 1;
      psw.f.n = (res >> 7) & 1;
      psw.f.z = opn2 == 0;
      pOpn1 = nullptr;
      break;
    case kAdd: // 加
      res = (uint16_t) (opn2 + opn1);
      opn2 = (uint8_t) res;
      psw.f.c = (res >> 8) & 1;
      psw.f.n = (res >> 7) & 1;
      psw.f.z = opn2 == 0;
      pOpn1 = nullptr;
      break;
    case kAnd: // 逻辑与
      res = (uint16_t) (opn2 & opn1);
      opn2 = (uint8_t) res;
      psw.f.c = 0;
      psw.f.n = (res >> 7) & 1;
      psw.f.z = opn2 == 0;
      pOpn1 = nullptr;
      break;
    case kAndp: // 外设口与
      res = (uint16_t) (In(opn2) & opn1);
      Out(opn2, (uint8_t) res);
      psw.f.c = 0;
      psw.f.n = (res >> 7) & 1;
      psw.f.z = opn2 == 0;
      pOpn1 = nullptr;
      break;
    case kBtjo: // 位测试为 1 则跳
      word = Sadr();
      if (opn1 & opn2)
        pc_ = word;
      pOpn1 = pOpn2 = nullptr;
      break;
    case kBtjop:
      stop();
      break;
    case kBtjz: // 位测试为 0 则跳
      word = Sadr();
      if (opn1 & ~opn2)
        pc_ = word;
      pOpn1 = pOpn2 = nullptr;
      break;
    case kBtjzp:
      stop();
      break;
    case kBr: // 分支
      pc_ = word;
      break;
    case kCall:
      data_[++sp_] = (uint8_t) (pc_ >> 8);
      data_[++sp_] = (uint8_t) (pc_ & 0xFF);
      pc_ = word;
      break;
    case kClr: // 清
      opn1 = 0;
      psw.f.c = 0;
      psw.f.n = 0;
      psw.f.z = 1;
      break;
    case kClrc:
      stop();
      break;
    case kCmp: // 比较
      res = (uint16_t) (opn2 - opn1);
      psw.f.c = ((res >> 8) & 1) ^ 1; // 借位时 c == 0
      psw.f.n = (res >> 7) & 1;
      psw.f.z = res == 0;
      pOpn1 = pOpn2 = nullptr;
      break;
    case kCmpa: // 与内存比较
      res = (uint16_t) (Read(word));
      res = (uint16_t) (*a_ - res);
      psw.f.c = ((res >> 8) & 1) ^ 1;
      psw.f.n = (res >> 7) & 1;
      psw.f.z = res == 0;
      pOpn1 = pOpn2 = nullptr;
      break;
    case kDac:
      stop();
      break;
    case kDec: // 减 1
      --opn1;
      psw.f.c = opn1 != 0xFF;
      psw.f.n = (opn1 >> 7) & 1;
      psw.f.z = (opn1 == 0);
      break;
    case kDecd: // 双字减 1 (参考实现的指针步进语义等价, 反向字节落在 pad0_)
      --opn1;
      if (opn1 == 0xFF)
      {
        --ByteBefore(pOpn1);
        psw.f.c = ByteBefore(pOpn1) != 0xFF;
      }
      psw.f.n = (ByteBefore(pOpn1) >> 7) & 1;
      psw.f.z = (ByteBefore(pOpn1) == 0);
      break;
    case kDint:
      stop();
      break;
    case kDjnz:
      stop();
      break;
    case kDsb:
      stop();
      break;
    case kEint: // 开中断
      psw.raw |= 0xF0; // 参考实现: 整个高四位 (I 及 z/n/c) 一并置位
      break;
    case kIdle:
      stop();
      break;
    case kInc: // 加 1
      ++opn1;
      psw.f.c = psw.f.z = (opn1 == 0);
      psw.f.n = (opn1 >> 7) & 1;
      break;
    case kInv:
      stop();
      break;
    case kJmp: // 无条件跳
      pc_ = word;
      break;
    case kJn: // 负则跳
      if (psw.f.n)
        pc_ = word;
      break;
    case kJz: // 零则跳
      if (psw.f.z)
        pc_ = word;
      break;
    case kJc:
      stop();
      break;
    case kJp: // 正则跳
      if (!psw.f.n && !psw.f.z)
        pc_ = word;
      break;
    case kJpz: // 正或零则跳
      if (!psw.f.n)
        pc_ = word;
      break;
    case kJnz: // 非零则跳
      if (!psw.f.z)
        pc_ = word;
      break;
    case kJnc: // 无进位则跳
      if (!psw.f.c)
        pc_ = word;
      break;
    case kLda: // 载入寄存器 A
      *a_ = res = (uint8_t) Read(word);
      psw.f.c = 0;
      psw.f.n = (res >> 7) & 1;
      psw.f.z = res == 0;
      break;
    case kLdsp: // 载入栈指针
      sp_ = *b_;
      break;
    case kMov: // 传送
      opn2 = opn1;
      psw.f.c = 0;
      psw.f.n = (opn2 >> 7) & 1;
      psw.f.z = opn2 == 0;
      pOpn1 = nullptr;
      break;
    case kMovd: // 双字传送
      if (pOpn1)
      {
        word = (uint16_t) ((ByteBefore(pOpn1) << 8) | *pOpn1);
        pOpn1 = nullptr;
      }
      if (pOpn2)
      {
        ByteBefore(pOpn2) = res = (uint8_t) (word >> 8);
        *pOpn2 = (uint8_t) (word & 0xFF);
        pOpn2 = nullptr;
        psw.f.c = 0;
        psw.f.n = (res >> 7) & 1;
        psw.f.z = res == 0;
      }
      else
      {
        stop(); // 不应发生
      }
      break;
    case kMovp: // 外设口传送
      if (instr.opn1 == kOpPn)
        opn1 = In(opn1);
      psw.f.c = 0;
      psw.f.n = (opn1 >> 7) & 1;
      psw.f.z = opn1 == 0;
      if (instr.opn2 == kOpPn)
      {
        Out(opn2, opn1);
        pOpn2 = nullptr;
      }
      else
      {
        opn2 = opn1;
      }
      pOpn1 = nullptr;
      break;
    case kMpy: // 乘
      res = (uint16_t) (opn1 * opn2);
      *a_ = (uint8_t) (res >> 8);
      *b_ = (uint8_t) (res & 0xFF);
      psw.f.c = 0;
      psw.f.n = (*a_ >> 7) & 1;
      psw.f.z = !*a_;
      pOpn1 = pOpn2 = nullptr;
      break;
    case kNop:
      stop();
      break;
    case kOr: // 逻辑或
      res = (uint16_t) (opn2 | opn1);
      opn2 = (uint8_t) res;
      psw.f.c = 0;
      psw.f.n = (res >> 7) & 1;
      psw.f.z = opn2 == 0;
      pOpn1 = nullptr;
      break;
    case kOrp: // 外设口或
      res = (uint16_t) (In(opn2) | opn1);
      Out(opn2, (uint8_t) res);
      psw.f.c = 0;
      psw.f.n = (res >> 7) & 1;
      psw.f.z = opn2 == 0;
      pOpn1 = nullptr;
      break;
    case kPop: // 弹栈
      opn1 = data_[sp_--];
      break;
    case kPush: // 压栈
      data_[++sp_] = opn1;
      pOpn1 = nullptr;
      break;
    case kReti: // 中断返回
      pc_ = data_[sp_--];
      pc_ |= (uint16_t) (data_[sp_--] << 8);
      psw.raw = data_[sp_--];
      break;
    case kRets: // 子程序返回
      pc_ = data_[sp_--];
      pc_ |= (uint16_t) (data_[sp_--] << 8);
      break;
    case kRl:
      stop();
      break;
    case kRlc:
      stop();
      break;
    case kRr:
      stop();
      break;
    case kRrc: // 带进位右移
      res = (uint16_t) ((opn1 >> 1) | (psw.f.c << 7));
      psw.f.c = opn1 & 1;
      opn1 = (uint8_t) res;
      psw.f.n = (res >> 7) & 1;
      psw.f.z = res == 0;
      break;
    case kSbb: // 带借位减
      res = (uint16_t) (opn2 - opn1 - 1 + c0());
      opn2 = (uint8_t) res;
      psw.f.c = ((res >> 8) & 1) ^ 1;
      psw.f.n = (res >> 7) & 1;
      psw.f.z = res == 0;
      pOpn1 = nullptr;
      break;
    case kSetc:
      stop();
      break;
    case kSta: // 存寄存器 A
      Write(word, res = *a_);
      psw.f.c = 0;
      psw.f.n = (res >> 7) & 1;
      psw.f.z = res == 0;
      break;
    case kStsp:
      stop();
      break;
    case kSub: // 减
      res = (uint16_t) (opn2 - opn1);
      opn2 = (uint8_t) res;
      psw.f.c = ((res >> 8) & 1) ^ 1;
      psw.f.n = (res >> 7) & 1;
      psw.f.z = res == 0;
      pOpn1 = nullptr;
      break;
    case kSwap: // 高低半字节交换
      opn1 = (uint8_t) ((opn1 >> 4) | (opn1 << 4));
      psw.f.c = opn1 & 1;
      psw.f.n = (opn1 >> 7) & 1;
      psw.f.z = !opn1;
      break;
    case kTrap:
      stop();
      break;
    case kTsta: // 测试 A
      psw.f.c = 0;
      psw.f.n = *a_ >> 7;
      psw.f.z = !*a_;
      break;
    case kTstb:
      stop();
      break;
    case kXchb:
      stop();
      break;
    case kXor:
      stop();
      break;
    case kXorp:
      stop();
      break;
    case kDb:
      stop();
      break;
    default:
      stop();
  }

  st_.raw = psw.raw;

  if (pOpn1)
    *pOpn1 = opn1;
  if (pOpn2)
    *pOpn2 = opn2;
}

} // namespace cts256a
} // namespace orm