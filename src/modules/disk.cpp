#include "modules/disk.hpp"

#include <algorithm>

std::vector<waybar::modules::Disk::DiskInfo>
waybar::modules::Disk::parse_infos(const Json::Value &config) {
  if (!config.isArray()) {
    throw std::runtime_error("Config error, expected array for disk format");
  }
  std::vector<waybar::modules::Disk::DiskInfo> infos;
  infos.reserve(std::distance(config.begin(), config.end()));
  std::transform(
      config.begin(), config.end(), std::back_inserter(infos),
      [](const auto &val) {
        if (!val.isString()) {
          throw std::runtime_error(
              "Config error, expected string for disk format");
        }
        return waybar::modules::Disk::DiskInfo{val.asString(), std::nullopt};
      });
  return infos;
}

waybar::modules::Disk::Disk(const std::string &name, const Json::Value &config)
    : ALabel(config, "{}", 10), name_(name),
      infos_(parse_infos(config["paths"])) {
  label_.set_name("disk");

  fs_thread_ = [this] {
    decltype(infos_) infos;
    {
      std::scoped_lock<std::mutex> lock(fs_thread_mutex_);
      infos = infos_;
    }

    for (auto &info : infos) {
      std::error_code ec;
      const auto space = fs::space(info.path, ec);
      info.space = ec ? std::nullopt : std::optional{space};
    }

    {
      std::scoped_lock<std::mutex> lock(fs_thread_mutex_);
      infos_ = infos;
    }
    dp.emit();
    fs_thread_.sleep_for(interval_);
  };
}

waybar::modules::Disk::~Disk() {}

auto waybar::modules::Disk::update() -> void {
  std::string markup_fmt;
  if (config_["format"].isString()) {
    markup_fmt = config_["format"].asString();
  }

  if (markup_fmt.empty()) {
    markup_fmt = "[{path}]: {used} / {total}";
  }

  std::scoped_lock<std::mutex> lock(fs_thread_mutex_);
  if (infos_.empty()) {
    label_.set_markup("");
    label_.set_tooltip_text("");
  } else {
    std::vector<std::string> info_strings;
    info_strings.reserve(infos_.size());
    for (const auto &info : infos_) {
      const auto &path = info.path;
      const auto &space = info.space;
      const bool has_info = space.has_value();
      info_strings.push_back(fmt::format(
          markup_fmt, fmt::arg("path", path),
          fmt::arg("used", has_info ? friendly_bytes(space.value().capacity -
                                                     space.value().free)
                                    : "Unknown"),
          fmt::arg("free",
                   has_info ? friendly_bytes(space.value().free) : "Unknown"),
          fmt::arg("total", has_info ? friendly_bytes(space.value().capacity)
                                     : "Unknown")));
    }
    label_.set_markup(info_strings[0]);
    const auto info_strings_joined =
        fmt::join(info_strings.cbegin(), info_strings.cend(), "\n");
    label_.set_tooltip_text(fmt::format(
        "{info_string}", fmt::arg("info_string", info_strings_joined)));
  }
}

const std::vector<std::string> waybar::modules::Disk::byte_mapping_{
    "B", "KiB", "MiB", "GiB", "TiB", "PiB"};
std::string waybar::modules::Disk::friendly_bytes(int64_t bytes,
                                                  int precision) {
  double bytesd = bytes;
  int i = 0;
  for (; bytesd > 2048; ++i) {
    bytesd /= 1024;
  }
  return fmt::format("{:3.{}f}{}", bytesd, precision, byte_mapping_.at(i));
}
