#pragma once

// 系列共享的 UI 工具函数 (Analyzer 版, 从 BandPass 裁剪):
// 只保留设置面板旋钮绘制用的 DrawKnob; 频率格式化/解析、Ghost 覆盖、随机颜色菜单
// 随 BandPass 的随机/滤波功能一并移除。

#include "IControls.h"
#include "../Theme.h"

#include <algorithm>
#include <cmath>

BEGIN_IPLUG_NAMESPACE
BEGIN_IGRAPHICS_NAMESPACE

// 频谱右侧布局 (Analyzer.cpp 图例与 SpectrumPad 共用):
// - kGainBarW: 电平表竖条单条横向宽度 (px)；L/R 两条紧挨无间隙, 总宽 = 2 × kGainBarW
//   (与 VU 条同宽 kVuBarW, 模式按钮以幽灵样式浮在条上, 不再需要更宽底座)
// - 表头区总宽 = 2 × kGainBarW (dB 刻度文字已移入频谱区域内部右侧)
// - kVuScaleW: 独立 VU 表左侧刻度文字区宽度 (L/R 条与 VU 表之间的间距, 放 VU 刻度文字)
// - kVuBarW:   独立 VU 表单条宽度 (L/R 两条并排, 总宽 2 × kVuBarW)
// - kLufsScaleW: 响度条左侧刻度文字区宽度 (VU 条与响度条之间的间距)
// - kLoudBarW:  响度条宽度 (M/S/I 三条并排, 总宽 3 × kLoudBarW)
// 电平区总让宽 (kMeterStripW) = 2×kGainBarW + kVuScaleW + 2×kVuBarW
//                           + kLufsScaleW + 3×kLoudBarW
constexpr float kGainBarW = 14.f;
constexpr float kVuScaleW = 28.f;
constexpr float kVuBarW = 14.f;
constexpr float kLufsScaleW = 28.f;
constexpr float kLoudBarW = 14.f;
constexpr float kMeterStripW = 2.f * kGainBarW + kVuScaleW + 2.f * kVuBarW + kLufsScaleW + 3.f * kLoudBarW;

// 电平表 UI 数据 (插件 OnIdle 每帧下发; 全 4 字节字段, 打包/解析安全)
struct LevelMeterUiData {
  float peakL, peakR;     // dBFS 样本峰值 (已平滑)
  float trueL, trueR;     // dBTP 真峰值 (已平滑)
  float rmsL, rmsR;       // RMS dBFS (300ms 积分)
  float vuL, vuR;         // VU 对应 dBFS (0 VU = -18 dBFS), 常驻
  float vuHoldL, vuHoldR; // 独立 VU 表峰值保持 (dB; -1000 = 无效)
  float holdL, holdR;     // L/R 条峰值保持 (显示域 dB; -1000 = 无效)
  float persistL, persistR; // dBTP 持久锁存 (真峰值 > 0, 只增不减; -120 = 未触发)
  float holdSec;          // 保持时长 (s, 0 = 关)
  int mode;               // L/R 条模式: 0: dBTP, 1: dBFS+RMS
  int overL, overR;       // 过载锁存
};

// 响度计 UI 数据 (插件 OnIdle 每帧下发; 全 4 字节字段, 打包/解析安全)
struct LoudnessUiData {
  float momentary, shortTerm; // M / S, LUFS
  float integrated;           // I, LUFS
  float range;                // LRA, LU
  float tpMax;                // dBTP 锁存
  float target;               // 预设目标 LUFS
  int preset;                 // 预设档位索引
  int scaleOff;               // M/S/I 条刻度窗偏移 (LU): 顶=目标+off, 1/3=目标, 底=目标-2·off
  int iValid, lraValid;       // I / LRA 有效性
};

// 标准旋钮手柄: 白环 + 深色核心
inline void DrawKnob(IGraphics &g, float cx, float cy) {
  g.FillCircle(COL_100(), cx, cy, HANDLE_R + HANDLE_RING);
  g.FillCircle(COL_900(), cx, cy, HANDLE_R);
}

END_IGRAPHICS_NAMESPACE
END_IPLUG_NAMESPACE
