#include "modules/backlight.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <limits>
#include <memory>

#include <net/if.h>
#include <netlink/attr.h>
#include <netlink/msg.h>
#include <netlink/netlink.h>
#include <netlink/socket.h>

#include <libudev.h>

#include <sys/epoll.h>
#include <unistd.h>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

namespace {
class FileDescriptor {
public:
  explicit FileDescriptor(int fd) : fd_(fd) {}
  FileDescriptor(const FileDescriptor &other) = delete;
  FileDescriptor(FileDescriptor &&other) noexcept = delete;
  FileDescriptor &operator=(const FileDescriptor &other) = delete;
  FileDescriptor &operator=(FileDescriptor &&other) noexcept = delete;
  ~FileDescriptor() {
    if (fd_ != -1) {
      if (close(fd_) != 0) {
        spdlog::error("Failed to close fd: {}", errno);
      }
    }
  }
  int get() const { return fd_; }

private:
  int fd_;
};

struct UdevDeleter {
  void operator()(udev *ptr) { udev_unref(ptr); }
};

struct UdevDeviceDeleter {
  void operator()(udev_device *ptr) { udev_device_unref(ptr); }
};

struct UdevEnumerateDeleter {
  void operator()(udev_enumerate *ptr) { udev_enumerate_unref(ptr); }
};

struct UdevMonitorDeleter {
  void operator()(udev_monitor *ptr) { udev_monitor_unref(ptr); }
};

void check_eq(int rc, int expected, const char *message = "eq, rc was: ") {
  if (rc != expected) {
    throw std::runtime_error(fmt::format(message, rc));
  }
}

void check_neq(int rc, int bad_rc, const char *message = "neq, rc was: ") {
  if (rc == bad_rc) {
    throw std::runtime_error(fmt::format(message, rc));
  }
}

void check0(int rc, const char *message = "rc wasn't 0") {
  check_eq(rc, 0, message);
}

void check_gte(int rc, int gte, const char *message = "rc was: ") {
  if (rc < gte) {
    throw std::runtime_error(fmt::format(message, rc));
  }
}

void check_nn(const void *ptr, const char *message = "ptr was null") {
  if (ptr == nullptr) {
    throw std::runtime_error(message);
  }
}
} // namespace

waybar::modules::Backlight::BacklightDev::BacklightDev(std::string name,
                                                       int actual, int max)
    : name_(std::move(name)), actual_(actual), max_(max) {}

std::string_view waybar::modules::Backlight::BacklightDev::name() const {
  return name_;
}

int waybar::modules::Backlight::BacklightDev::get_actual() const {
  return actual_;
}

int waybar::modules::Backlight::BacklightDev::get_max() const {
  assert(max_ != 0);
  return max_;
}

void waybar::modules::Backlight::BacklightDev::set_actual(int actual) {
  actual_ = actual;
}

waybar::modules::Backlight::Backlight(const std::string &name,
                                      const Json::Value &config)
    : ALabel(config, "{}", 2), name_(name),
      preferred_device_(
          config["device"].isString() ? config["device"].asString() : "") {
  label_.set_name("backlight");

  udev_thread_ = [this] {
    std::unique_ptr<udev, UdevDeleter> udev{udev_new()};
    check_nn(udev.get(), "Udev new failed");
    SPDLOG_DEBUG("udev init");

    std::unique_ptr<udev_monitor, UdevMonitorDeleter> mon{
        udev_monitor_new_from_netlink(udev.get(), "udev")};
    check_nn(mon.get(), "udev monitor new failed");
    check_gte(udev_monitor_filter_add_match_subsystem_devtype(
                  mon.get(), "backlight", nullptr),
              0, "udev failed to add monitor filter: ");
    udev_monitor_enable_receiving(mon.get());

    auto udev_fd = udev_monitor_get_fd(mon.get());
    SPDLOG_DEBUG("udev_fd: {}", udev_fd);

    auto epoll_fd = FileDescriptor{epoll_create1(0)};
    check_neq(epoll_fd.get(), -1, "epoll init failed: ");
    epoll_event ctl_event;
    ctl_event.events = EPOLLIN;
    ctl_event.data.fd = udev_fd;

    check0(
        epoll_ctl(epoll_fd.get(), EPOLL_CTL_ADD, ctl_event.data.fd, &ctl_event),
        "epoll_ctl failed: {}");
    epoll_event events[EPOLL_MAX_EVENTS];

    // Get initial state
    {
      decltype(devices_) devices;
      {
        std::scoped_lock<std::mutex> lock(udev_thread_mutex_);
        devices = devices_;
      }
      enumerate_devices(devices.begin(), devices.end(),
                        std::back_inserter(devices), udev.get());
      {
        std::scoped_lock<std::mutex> lock(udev_thread_mutex_);
        devices_ = devices;
      }
      dp.emit();
    }

    while (udev_thread_.isRunning()) {
      const int event_count =
          epoll_wait(epoll_fd.get(), events, EPOLL_MAX_EVENTS,
                     std::chrono::milliseconds{interval_}.count());
      decltype(devices_) devices;
      {
        std::scoped_lock<std::mutex> lock(udev_thread_mutex_);
        devices = devices_;
      }
      SPDLOG_TRACE("udev epoll got {} events", event_count);
      for (int i = 0; i < event_count; ++i) {
        const auto &event = events[i];
        check_eq(event.data.fd, udev_fd, "unexpected udev fd");
        std::unique_ptr<udev_device, UdevDeviceDeleter> dev{
            udev_monitor_receive_device(mon.get())};
        check_nn(dev.get(), "epoll dev was null");
        upsert_device(devices.begin(), devices.end(),
                      std::back_inserter(devices), dev.get());
      }

      // Refresh state if timed out
      if (event_count == 0) {
        enumerate_devices(devices.begin(), devices.end(),
                          std::back_inserter(devices), udev.get());
      }
      {
        std::scoped_lock<std::mutex> lock(udev_thread_mutex_);
        devices_ = devices;
      }
      dp.emit();
    }
  };
}

waybar::modules::Backlight::~Backlight() = default;

auto waybar::modules::Backlight::update() -> void {
  std::vector<BacklightDev> devices;
  {
    std::scoped_lock<std::mutex> lock(udev_thread_mutex_);
    devices = devices_;
  }

  std::string markup_fmt;
  if (config_["format"].isString()) {
    markup_fmt = config_["format"].asString();
  }

  if (markup_fmt.empty()) {
    markup_fmt = "{percent}%";
  }

  const auto best =
      best_device(devices.cbegin(), devices.cend(), preferred_device_);
  if (best != nullptr) {
    SPDLOG_TRACE("backlight: {}", best->name());
    if (previous_best_.has_value() && previous_best_.value() == *best) {
      return;
    }
    const auto percent = best->get_actual() * 100 / best->get_max();
    label_.set_markup(fmt::format(markup_fmt,
                                  fmt::arg("percent", std::to_string(percent)),
                                  fmt::arg("icon", getIcon(percent))));
  } else {
    if (!previous_best_.has_value()) {
      return;
    }
    label_.set_markup("");
  }
  previous_best_ = *best;
}

template <class ForwardIt>
const waybar::modules::Backlight::BacklightDev *
waybar::modules::Backlight::best_device(ForwardIt first, ForwardIt last,
                                        std::string_view preferred_device) {
  const auto found =
      std::find_if(first, last, [preferred_device](const auto &dev) {
        return dev.name() == preferred_device;
      });
  if (found != last) {
    return &(*found);
  }

  const auto max =
      std::max_element(first, last, [](const auto &l, const auto &r) {
        return l.get_max() < r.get_max();
      });

  return max == last ? nullptr : &(*max);
}

template <class ForwardIt, class Inserter>
void waybar::modules::Backlight::upsert_device(ForwardIt first, ForwardIt last,
                                               Inserter inserter,
                                               udev_device *dev) {
  const char *name = udev_device_get_sysname(dev);
  check_nn(name);
  const char *actual = udev_device_get_sysattr_value(dev, "actual_brightness");
  check_nn(actual);
  const int actual_int = std::stoi(actual);
  auto found = std::find_if(first, last, [name](const auto &device) {
    return device.name() == name;
  });
  if (found != last) {
    found->set_actual(actual_int);
  } else {
    const char *max = udev_device_get_sysattr_value(dev, "max_brightness");
    check_nn(max);
    const int max_int = std::stoi(max);
    *inserter = BacklightDev{name, actual_int, max_int};
    ++inserter;
  }
}

template <class ForwardIt, class Inserter>
void waybar::modules::Backlight::enumerate_devices(ForwardIt first,
                                                   ForwardIt last,
                                                   Inserter inserter,
                                                   udev *udev) {
  std::unique_ptr<udev_enumerate, UdevEnumerateDeleter> enumerate{
      udev_enumerate_new(udev)};
  udev_enumerate_add_match_subsystem(enumerate.get(), "backlight");
  udev_enumerate_scan_devices(enumerate.get());
  udev_list_entry *enum_devices =
      udev_enumerate_get_list_entry(enumerate.get());
  udev_list_entry *dev_list_entry;
  udev_list_entry_foreach(dev_list_entry, enum_devices) {
    const char *path = udev_list_entry_get_name(dev_list_entry);
    std::unique_ptr<udev_device, UdevDeviceDeleter> dev{
        udev_device_new_from_syspath(udev, path)};
    check_nn(dev.get(), "dev new failed");
    upsert_device(first, last, inserter, dev.get());
  }
}