#pragma once

#include <fmt/format.h>
#include <iostream>
#include <optional>

#include <string>
#include <vector>

#include "ALabel.hpp"
#include "filesystem.hpp"
#include "util/command.hpp"
#include "util/json.hpp"
#include "util/sleeper_thread.hpp"

namespace waybar::modules {

class Disk : public ALabel {
  struct DiskInfo {
    std::string path;
    std::optional<fs::space_info> space;
  };

public:
  Disk(const std::string &name, const Json::Value &config);
  ~Disk();
  auto update() -> void;

private:
  static std::string friendly_bytes(int64_t bytes, int precision = 1);
  static std::vector<DiskInfo> parse_infos(const Json::Value &config);

  static const std::vector<std::string> byte_mapping_;
  const std::string name_;

  std::mutex fs_thread_mutex_;
  std::vector<DiskInfo> infos_;

  waybar::util::SleeperThread fs_thread_;
};
} // namespace waybar::modules
