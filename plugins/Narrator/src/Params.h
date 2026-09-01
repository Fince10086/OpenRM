#pragma once

// 参数枚举独立成头: 插件本体 (Narrator.h/.cpp) 与 UI 控件 (ORMSlider 等)
// 都要引用参数索引, 避免控件反向依赖插件头文件。

enum EParams
{
  kBaseKey = 0, // 触发基准键 (音高偏移的 0 点)
  kPitch,       // SAM 原生音高
  kSpeed,       // SAM 原生语速 (值越大越慢)
  kMouth,       // SAM 口腔形变
  kThroat,      // SAM 喉腔形变
  kAttack,      // 触发起音 ms
  kRelease,     // 松键释放 ms
  kRetrig,      // 重复 note-on 是否从头重放
  kMono,        // 单音 (true) / 复音 (false)
  kGain,        // 输出电平 dB
  kNumParams
};

// 跨线程数据绑定用的控件 tag (AttachControl 第二参)
enum EControlTags
{
  kCtrlTagKeyboard = 100, // 屏幕键盘: 宿主 MIDI 回显
  kCtrlTagTimeline,       // 语句时间线: 波形 + 播放指针
};

// 时间线包络点数 (音频线程推送的波形分辨率)
constexpr int kPhraseEnvPoints = 256;

// 宿主块尺寸上限 (超长块在 ProcessBlock 内钳制)
constexpr int kMaxBlock = 16384;
