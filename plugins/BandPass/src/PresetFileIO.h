#pragma once
#include <nlohmann/json.hpp>

#include <fstream>
#include <string>
#include <utility>
#include <vector>

struct PresetFileData
{
  std::vector<std::vector<double>> presets;
  std::vector<double> currentValues;
  int currentPreset = 0;
  double fadePos = 0.0;
};

inline bool WritePresetFile(const std::string& path, const PresetFileData& data, std::string& err)
{
  try
  {
    nlohmann::json j;
    j["version"] = 3;
    j["presets"] = data.presets;
    j["currentPreset"] = data.currentPreset;
    j["currentValues"] = data.currentValues;
    j["fadePos"] = data.fadePos;

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
    if (!j.is_object() || !j.contains("version"))
    {
      err = "Unsupported preset file (missing version)";
      return false;
    }
    const int version = j["version"].get<int>();
    if (version != 2 && version != 3)
    {
      err = "Unsupported preset file (expected version 2 or 3)";
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
      std::vector<double> vals;
      if (item.is_array())
        for (const auto& v : item)
          vals.push_back(v.get<double>());
      out.presets.push_back(std::move(vals));
    }

    out.currentPreset = j.value("currentPreset", 0);
    out.currentValues.clear();
    if (j.contains("currentValues") && j["currentValues"].is_array())
      for (const auto& v : j["currentValues"])
        out.currentValues.push_back(v.get<double>());
    out.fadePos = j.value("fadePos", 0.0);
    return true;
  }
  catch (const std::exception& e)
  {
    err = std::string("JSON field error: ") + e.what();
    return false;
  }
}
