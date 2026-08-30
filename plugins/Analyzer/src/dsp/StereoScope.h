#pragma once

// StereoScope — 立体声声像显示引擎 (音频线程只攒 hop 原始样本包)
//
// 与 SpectrumSTFT 同一数据管线形态: 音频线程逐样本拷入 hop 缓冲, 攒满即经 ISender
// 队列推给 UI; 方位角/能量分桶/相关性等几何计算全部在 UI 线程完成 (重活不过音频线程)。
// hop 恒 1024, 与四个频谱引擎帧进给同格 —— 冻结回放帧格天然对齐, 同一冻结环可以
// 同时喂频谱与声像两条显示链路。

#include "ISender.h"

#include <algorithm>
#include <array>
#include <cstring>

BEGIN_IPLUG_NAMESPACE

template <int HOP = 1024, int QUEUE_SIZE = 64>
class StereoScope : public ISender<2, QUEUE_SIZE, std::array<float, HOP>> {
public:
  using TDataPacket = std::array<float, HOP>;
  using Data = ISenderData<2, TDataPacket>;
  using Base = ISender<2, QUEUE_SIZE, TDataPacket>;

  // inputs[0] = L, inputs[1] = R (单声道输入时两路相同, 声像天然居中)
  void ProcessBlock(sample **inputs, int nFrames, int ctrlTag = kNoTag) {
    for (int s = 0; s < nFrames; ++s) {
      mPending[0][mBufCount] = static_cast<float>(inputs[0][s]);
      mPending[1][mBufCount] = static_cast<float>(inputs[1][s]);
      if (++mBufCount == HOP) {
        Data d{ctrlTag, 2, 0};
        std::copy(mPending[0].begin(), mPending[0].end(), d.vals[0].begin());
        std::copy(mPending[1].begin(), mPending[1].end(), d.vals[1].begin());
        Base::PushData(d);
        mBufCount = 0;
      }
    }
  }

private:
  int mBufCount = 0;
  std::array<std::array<float, HOP>, 2> mPending{};
};

END_IPLUG_NAMESPACE
