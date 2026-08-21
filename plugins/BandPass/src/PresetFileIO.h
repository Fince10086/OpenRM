#pragma once
// ============================================================================
// PresetFileIO.h — 预设库 JSON 读写 (header-only, 不依赖插件类型)
//
// 通用容器: PresetFileData 存"名称 + 参数值数组", 由插件层负责类型转换与
// 数量校验 (kNumPresets / kNumParams)。文件格式见 WritePresetFile 注释。
// ============================================================================
#include <nlohmann/json.hpp>

#include <fstream>
#include <string>
#include <utility>
#include <vector>

struct PresetFileEntry
{
  std::string name;            // 预设名
  std::vector<double> values;  // 参数值 (期望 kNumParams 个)
};

struct PresetFileData
{
  std::vector<PresetFileEntry> presets;  // 期望 kNumPresets 个
  std::vector<double> currentValues;     // 当前参数快照 (期望 kNumParams 个)
  int currentPreset = 0;                 // 当前选中槽位
  double morphPos = 0.0;                 // morph 条位置
};

// JSON 格式 (schema v1):
// {
//   "version": 1,
//   "presetCount": 16,
//   "presets": [ { "name": "Preset 1", "values": [ ...11 个数... ] }, ... ],
//   "currentPreset": 3,
//   "currentValues": [ ...11 个数... ],
//   "morphPos": 0.25
// }

// 写文件。成功返回 true; 失败时 err 填原因。
inline bool WritePresetFile(const std::string& path, const PresetFileData& data, std::string& err)
{
  try
  {
    nlohmann::json j;
    j["version"] = 1;
    j["presetCount"] = static_cast<int>(data.presets.size());

    nlohmann::json arr = nlohmann::json::array();
    for (const auto& p : data.presets)
      arr.push_back({{"name", p.name}, {"values", p.values}});
    j["presets"] = arr;

    j["currentPreset"] = data.currentPreset;
    j["currentValues"] = data.currentValues;
    j["morphPos"] = data.morphPos;

    std::ofstream f(path, std::ios::binary);
    if (!f)
    {
      err = "Cannot open file for writing: " + path;
      return false;
    }
    f << j.dump(2);
    return true;
  }
  catch (const std::exception& e)
  {
    err = std::string("JSON write error: ") + e.what();
    return false;
  }
}

// 读文件。成功返回 true; 失败时 err 填原因。只做结构解析,
// 字段数量/范围校验由插件层完成 (见 GRMBandPass::ReadPresetFileFrom)。
inline bool ReadPresetFile(const std::string& path, PresetFileData& out, std::string& err)
{
  std::ifstream f(path, std::ios::binary);
  if (!f)
  {
    err = "Cannot open file: " + path;
    return false;
  }

  nlohmann::json j;
  try
  {
    f >> j;
  }
  catch (const std::exception& e)
  {
    err = std::string("JSON parse error: ") + e.what();
    return false;
  }

  try
  {
    if (!j.is_object() || !j.contains("version") || j["version"].get<int>() != 1)
    {
      err = "Unsupported preset file (expected version 1)";
      return false;
    }
    if (!j.contains("presets") || !j["presets"].is_array())
    {
      err = "Missing 'presets' array";
      return false;
    }

    out.presets.clear();
    for (const auto& item : j["presets"])
    {
      PresetFileEntry e;
      e.name = item.value("name", "");
      if (item.contains("values") && item["values"].is_array())
        for (const auto& v : item["values"])
          e.values.push_back(v.get<double>());
      out.presets.push_back(std::move(e));
    }

    out.currentPreset = j.value("currentPreset", 0);
    out.currentValues.clear();
    if (j.contains("currentValues") && j["currentValues"].is_array())
      for (const auto& v : j["currentValues"])
        out.currentValues.push_back(v.get<double>());
    out.morphPos = j.value("morphPos", 0.0);
    return true;
  }
  catch (const std::exception& e)
  {
    err = std::string("JSON field error: ") + e.what();
    return false;
  }
}
