#pragma once

#include <array>

enum EEngine
{
  kEngineSAM = 0,
  kEngineDEC = 1,
  kEngineSP = 2,
  kEngineTMS = 3,
  kEngineTSI = 4,
  kNumEngines = 5
};

enum EParams
{
  kEngine = 0,
  kMapMode,
  kBaseKey,
  // SAM
  kSamPitch,
  kSamSpeed,
  kSamMouth,
  kSamThroat,
  // TMS
  kTmsSpeed,
  kTmsPitch,
  kTmsBank,
  // TSI
  kTsiSpeed,
  kTsiBank,
  // SP0256
  kSp0256Speed,
  kSp0256Voice,
  // DECTALK
  kDectalkVoice,
  kDectalkRate,
  kDectalkPitch,
  // 通用
  kAttack,
  kRelease,
  kMono,
  kGain,
  kLoop,
  kNumParams
};

using ParamSnapshot = std::array<double, kNumParams>;

enum
{
  kBankMilitary = 0,
  kBankTi99 = 1,
  kBankAcorn = 2,
  kBankSspell = 3,
  kBankClock = 4,
  kNumBanks = 5
};

enum
{
  kS14001Bzk = 0,
  kS14001F2k = 1,
  kS14001Csc0 = 2,
  kS14001Csc1 = 3,
  kS14001Csc2 = 4,
  kS14001Csc3 = 5,
  kS14001Csc4 = 6,
  kS14001Csc5 = 7,
  kS14001Csc6 = 8,
  kNumS14001Sets = 9
};

enum
{
  kSp0256Text = 0,
  kSp0256Phoneme = 1,
  kNumSp0256 = 2
};

enum EControlTags
{
  kCtrlTagKeyboard = 100,
  kCtrlTagTimeline,
};

constexpr int kPhraseEnvPoints = 256;
constexpr int kMaxBlock = 16384;
