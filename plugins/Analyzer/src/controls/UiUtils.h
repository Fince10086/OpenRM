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
// - 表头区总宽 = 2 × kGainBarW (dB 刻度文字已移入频谱区域内部右侧)
constexpr float kGainBarW = 16.f;

// 电平表 UI 数据 (插件 OnIdle 每帧下发; 全 4 字节字段, 打包/解析安全)
struct LevelMeterUiData {
  float peakL, peakR; // dBFS 样本峰值 (已平滑)
  float trueL, trueR; // dBTP 真峰值 (已平滑)
  float rmsL, rmsR;   // RMS dBFS (300ms 积分)
  float vuL, vuR;     // VU 对应 dBFS (0 VU = -18 dBFS)
  float holdL, holdR; // 峰值保持 (显示域 dB; -1000 = 无效)
  float holdSec;      // 保持时长 (s, 0 = 关)
  int mode;           // 0: dBTP, 1: dBFS+RMS, 2: VU
  int overL, overR;   // 过载锁存
};

// 响度计 UI 数据 (插件 OnIdle 每帧下发; 全 4 字节字段, 打包/解析安全)
struct LoudnessUiData {
  float momentary, shortTerm; // M / S, LUFS
  float integrated;           // I, LUFS
  float range;                // LRA, LU
  float tpMax;                // dBTP 锁存
  float target;               // 预设目标 LUFS
  int preset;                 // 预设档位索引
  int iValid, lraValid;       // I / LRA 有效性
};

// 标准旋钮手柄: 白环 + 深色核心
inline void DrawKnob(IGraphics &g, float cx, float cy) {
  g.FillCircle(COL_100(), cx, cy, HANDLE_R + HANDLE_RING);
  g.FillCircle(COL_900(), cx, cy, HANDLE_R);
}

END_IGRAPHICS_NAMESPACE
END_IPLUG_NAMESPACE
