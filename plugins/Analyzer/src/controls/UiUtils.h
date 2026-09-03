#pragma once

// 系列共享的 UI 工具函数 (Analyzer 版)

#include "IControls.h"
#include "../Theme.h"

#include <algorithm>
#include <cmath>

BEGIN_IPLUG_NAMESPACE
BEGIN_IGRAPHICS_NAMESPACE

constexpr float kGainBarW = 14.f;
constexpr float kVuScaleW = 28.f;
constexpr float kVuBarW = 14.f;
constexpr float kLufsScaleW = 28.f;
constexpr float kLoudBarW = 14.f;
constexpr float kLraZoneW = 32.f;
constexpr float kMeterStripW = 2.f * kGainBarW + kVuScaleW + 2.f * kVuBarW + kLufsScaleW + 4.f * kLoudBarW + kLraZoneW;

// 电平表 UI 数据 (插件 OnIdle 每帧下发)
struct LevelMeterUiData {
  float peakL, peakR;     // dBFS 样本峰值
  float trueL, trueR;     // dBTP 真峰值
  float rmsL, rmsR;       // RMS dBFS (300ms 积分)
  float vuL, vuR;         // VU 对应 dBFS (0 VU = -18 dBFS)
  float vuHoldL, vuHoldR; // VU 表峰值保持 (-1000 = 无效)
  float holdL, holdR;     // L/R 条峰值保持 (-1000 = 无效)
  float persistL, persistR; // dBTP 持久锁存 (-120 = 未触发)
  float holdSec;          // 保持时长 (s, 0 = 关)
  int mode;               // 0: dBTP, 1: dBFS+RMS
  int overL, overR;       // 过载锁存
};

// 响度计 UI 数据 (插件 OnIdle 每帧下发)
struct LoudnessUiData {
  float momentary, shortTerm; // M / S, LUFS
  float integrated;           // I, LUFS
  float range;                // LRA, LU
  float lraMin, lraMax;       // LRA 直方图 10%/95% 端点
  float tpMax;                // dBTP 锁存
  float target;               // 预设目标 LUFS
  int preset;                 // 预设档位索引
  int scaleOff;               // 刻度窗偏移 (LU): 顶=目标+off, 1/3=目标, 底=目标-2·off
  int iValid, lraValid;       // I / LRA 有效性
};

// 标准旋钮手柄: 白环 + 深色核心
inline void DrawKnob(IGraphics &g, float cx, float cy) {
  g.FillCircle(COL_100(), cx, cy, HANDLE_R + HANDLE_RING);
  g.FillCircle(COL_900(), cx, cy, HANDLE_R);
}

END_IGRAPHICS_NAMESPACE
END_IPLUG_NAMESPACE
