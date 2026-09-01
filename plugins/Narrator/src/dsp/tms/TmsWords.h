#pragma once

// TMS 词条共享类型: 每套音色词表 (military/ti99/acorn/clock) 都以
// `orm::tms::<bank>::kWords` 暴露同一种 Word 结构, 供 Tms5220Engine 查询。
namespace orm::tms {

struct Word {
  const char* name;           // 大写词名 (查表用, 含下划线片段如 THIR_/A_M_)
  const unsigned char* data;  // LPC 位流字节 (Talkie 帧格式, LSB-first)
  int len;
};

} // namespace orm::tms
