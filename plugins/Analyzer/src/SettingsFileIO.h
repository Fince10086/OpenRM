#pragma once
#include <nlohmann/json.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

// 插件全局 UI 偏好持久化
// 文件位置:
//   macOS: ~/Library/Application Support/OpenRM/<Plugin>.settings
//   Windows: %APPDATA%/OpenRM/<Plugin>.settings

constexpr const char *kSettingsFileName = "ORMAnalyzer.settings";

struct SettingsData {
  int lang = -1;       // -1 = 未保存过, 用 DetectSystemLanguage()
  int hue = 45;
  int satMax = 15;
  int themeMode = 0;
  // 内置测试信号发生器 (开发者工具, ORM_ENABLE_TEST_GEN 关闭时不生效)
  int genType = 0;       // orm::ETestSignal, 0 = OFF
  double genFreq = 1000.0;  // Hz (对数滑杆 1 Hz .. 20 kHz)
  double genLevel = -12.0;  // dBFS 峰值
  int genHold = 0;       // 冻结时锁相位
  int genToOutput = 0;   // 路由到输出 (默认关)
};

inline std::string SettingsDir() {
  const char *base = nullptr;
#if defined(OS_WIN)
  base = std::getenv("APPDATA");
#else
  base = std::getenv("HOME");
#endif
  if (!base || !*base)
    return "";
  std::string dir = base;
#if defined(OS_WIN)
  dir += "/OpenRM";
#else
  dir += "/Library/Application Support/OpenRM";
#endif
  return dir;
}

inline std::string SettingsFilePath() {
  const std::string dir = SettingsDir();
  return dir.empty() ? std::string() : dir + "/" + kSettingsFileName;
}

inline bool LoadSettings(SettingsData &out) {
  std::ifstream f(SettingsFilePath());
  if (!f)
    return false;
  try {
    nlohmann::json j = nlohmann::json::parse(f);
    if (!j.is_object())
      return false;
    if (j.contains("lang"))
      out.lang = j["lang"].get<int>();
    if (j.contains("hue"))
      out.hue = j["hue"].get<int>();
    if (j.contains("satMax"))
      out.satMax = j["satMax"].get<int>();
    if (j.contains("themeMode"))
      out.themeMode = j["themeMode"].get<int>();
    if (j.contains("genType"))
      out.genType = j["genType"].get<int>();
    if (j.contains("genFreq"))
      out.genFreq = j["genFreq"].get<double>();
    if (j.contains("genLevel"))
      out.genLevel = j["genLevel"].get<double>();
    if (j.contains("genHold"))
      out.genHold = j["genHold"].get<int>();
    if (j.contains("genToOutput"))
      out.genToOutput = j["genToOutput"].get<int>();
    return true;
  } catch (...) {
    return false;
  }
}

inline bool SaveSettings(const SettingsData &d) {
  try {
    const std::string path = SettingsFilePath();
    if (path.empty())
      return false;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    std::ofstream f(path);
    if (!f)
      return false;
    nlohmann::json j;
    j["lang"] = d.lang;
    j["hue"] = d.hue;
    j["satMax"] = d.satMax;
    j["themeMode"] = d.themeMode;
    j["genType"] = d.genType;
    j["genFreq"] = d.genFreq;
    j["genLevel"] = d.genLevel;
    j["genHold"] = d.genHold;
    j["genToOutput"] = d.genToOutput;
    f << j.dump(2);
    return true;
  } catch (...) {
    return false;
  }
}
