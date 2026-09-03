#pragma once
#include <nlohmann/json.hpp>

#include <fstream>
#include <cstring>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

// 插件状态文件读写（.orm 格式，JSON 序列化，文件头魔数 "analyzer"）
constexpr const char *kStateMagic = "analyzer";

struct StateFileData {
  std::vector<double> values;
};

inline bool WriteStateFile(const std::string &path, const StateFileData &data, std::string &err) {
  try {
    nlohmann::json j;
    j["version"] = 1;
    j["values"] = data.values;

    std::ofstream f(path, std::ios::binary);
    if (!f) {
      err = "Cannot open file for writing: " + path;
      return false;
    }
    f << kStateMagic << "\n" << j.dump(2);
    return true;
  } catch (const std::exception &e) {
    err = std::string("JSON write error: ") + e.what();
    return false;
  }
}

inline bool ReadStateFile(const std::string &path, StateFileData &out, std::string &err) {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    err = "Cannot open file: " + path;
    return false;
  }
  const std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

  // 校验文件头，拒绝非 ORM Analyzer 文件
  if (content.rfind(kStateMagic, 0) != 0) {
    err = std::string("Not an ORM Analyzer file (missing '") + kStateMagic + "' header)";
    return false;
  }
  std::size_t pos = std::strlen(kStateMagic);
  while (pos < content.size() && (content[pos] == '\n' || content[pos] == '\r'))
    ++pos;

  nlohmann::json j;
  try {
    j = nlohmann::json::parse(content.substr(pos));
  } catch (const std::exception &e) {
    err = std::string("JSON parse error: ") + e.what();
    return false;
  }

  try {
    if (!j.is_object() || !j.contains("version")) {
      err = "Unsupported state file (missing version)";
      return false;
    }
    const int version = j["version"].get<int>();
    if (version != 1) {
      err = "Unsupported state file (expected version 1)";
      return false;
    }
    if (!j.contains("values") || !j["values"].is_array()) {
      err = "Missing 'values' array";
      return false;
    }

    out.values.clear();
    for (const auto &v : j["values"])
      out.values.push_back(v.get<double>());
    return true;
  } catch (const std::exception &e) {
    err = std::string("JSON field error: ") + e.what();
    return false;
  }
}
