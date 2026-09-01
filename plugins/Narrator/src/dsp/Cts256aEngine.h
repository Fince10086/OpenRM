// Cts256aEngine.h — GI/Microchip CTS256A-AL2 text-to-speech 控制器
//
// CTS256A-AL2 是 SP0256-AL2 的配套文本转语音芯片 (1987): 一颗 TMS7000 单片机 +
// 4KB 掩膜 ROM, ROM 内含 letter-to-sound 规则引擎。主机把英文文本写进芯片,
// 芯片输出 SP0256-AL2 allophone 码流 (0..63)。
//
// 本引擎 = TMS7000 CPU (见 dsp/cts256a/Tms7000Cpu) + 芯片内存映射仿真:
//   - 文本 经 0x0200-0x0FFF 读入 (大写化; 耗尽后恒返 CR);
//   - allophone 码 写 0x2000-0x2FFF 输出 (收集到 vector);
//   - 0xF33E 高水位补丁 (强制每个 allophone 即时送出, 保持参考仿真行为);
//   - 开机 "O.K." (前 6 个输出) 恒抑制;
//   - 文本耗尽后 eofctr 计数 (199999 次读) 停止仿真。
// 参考: GmEsoft SP0256_CTS256A-AL2 (GPL-3.0-or-later); ROM 数据见
// dsp/cts256a/Cts256aRoms.h。
#pragma once

#include <string>
#include <vector>

namespace orm
{

class Cts256aEngine
{
public:
  // 英文文本 -> SP0256-AL2 allophone 码序列 (0..63, 后接 SP0256 渲染)。
  // 文本自动大写 (\n/\r 归一为空格, 与参考仿真的逐字符 toupper 一致),
  // 无对应规则的内容自然跳过; 无可合成输出返回 false。
  static bool Convert(const std::string& text, std::vector<int>& codes);
};

} // namespace orm