#include "modules/sway/ipc/client.hpp"

#include "util/bit_cast.hpp"

waybar::modules::sway::Ipc::Ipc()
{
  const std::string& socketPath = getSocketPath();
  fd_ = open(socketPath);
  fd_event_ = open(socketPath);
}

waybar::modules::sway::Ipc::~Ipc()
{
  // To fail the IPC header
  write(fd_, "close-sway-ipc", 14);
  write(fd_event_, "close-sway-ipc", 14);

  close(fd_);
  close(fd_event_);
}

const std::string waybar::modules::sway::Ipc::getSocketPath() const
{
  const char *env = getenv("SWAYSOCK");
  if (env != nullptr) {
    return std::string(env);
  }
  std::string str;
  {
    std::string str_buf;
    FILE*  in;
    char  buf[512] = { 0 };
    if ((in = popen("sway --get-socketpath 2>/dev/null", "r")) == nullptr) {
      throw std::runtime_error("Failed to get socket path");
    }
    while (fgets(buf, sizeof(buf), in) != nullptr) {
      str_buf.append(buf, sizeof(buf));
    }
    pclose(in);
    str = str_buf;
  }
  if (str.back() == '\n') {
    str.pop_back();
  }
  return str;
}

int waybar::modules::sway::Ipc::open(const std::string& socketPath) const
{
  struct sockaddr_un addr = {0};
  int fd = -1;
  if ((fd = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
    throw std::runtime_error("Unable to open Unix socket");
  }
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, socketPath.c_str(), sizeof(addr.sun_path) - 1);
  addr.sun_path[sizeof(addr.sun_path) - 1] = 0;
  int l = sizeof(struct sockaddr_un);
  if (::connect(fd, reinterpret_cast<struct sockaddr *>(&addr), l) == -1) {
    throw std::runtime_error("Unable to connect to Sway");
  }
  return fd;
}

struct waybar::modules::sway::Ipc::ipc_response
  waybar::modules::sway::Ipc::recv(int fd) const
{
  std::string header;
  header.reserve(ipc_header_size_);
  size_t total = 0;

  while (total < ipc_header_size_) {
    auto res = ::recv(fd, header.data() + total, ipc_header_size_ - total, 0);
    if (res <= 0) {
      throw std::runtime_error("Unable to receive IPC header");
    }
    total += res;
  }

  auto magic = std::string(header.data(), header.data() + ipc_magic_.size());
  if (magic != ipc_magic_) {
    throw std::runtime_error("Invalid IPC magic");
  }

  total = 0;
  std::string payload;
  const char *const data32_begin = header.data() + ipc_magic_.size();
  const auto size = util::bit_cast_from_ptr<uint32_t>(data32_begin);
  const auto type = util::bit_cast_from_ptr<uint32_t>(data32_begin + sizeof(uint32_t));
  payload.reserve(size + 1);
  while (total < size) {
    const auto res = ::recv(fd, payload.data() + total, size - total, 0);
    if (res < 0) {
      throw std::runtime_error("Unable to receive IPC payload");
    }
    total += res;
  }
  payload[size] = 0;
  return { size, type, &payload.front() };
}

struct waybar::modules::sway::Ipc::ipc_response
  waybar::modules::sway::Ipc::send(int fd, uint32_t type,
  const std::string& payload) const
{
  std::string header;
  header.reserve(ipc_header_size_);
  std::memcpy(header.data(), ipc_magic_.c_str(), ipc_magic_.size());
  char *const data32_begin = header.data() + ipc_magic_.size();
  const uint32_t payload_size = payload.size();
  std::memcpy(data32_begin, &payload_size, sizeof(uint32_t));
  std::memcpy(data32_begin + sizeof(uint32_t), &type, sizeof(uint32_t));

  if (::send(fd, header.data(), ipc_header_size_, 0) == -1) {
    throw std::runtime_error("Unable to send IPC header");
  }
  if (::send(fd, payload.c_str(), payload.size(), 0) == -1) {
    throw std::runtime_error("Unable to send IPC payload");
  }
  return recv(fd);
}

struct waybar::modules::sway::Ipc::ipc_response
  waybar::modules::sway::Ipc::sendCmd(uint32_t type,
  const std::string& payload) const
{
  return send(fd_, type, payload);
}

void waybar::modules::sway::Ipc::subscribe(const std::string& payload) const
{
  auto res = send(fd_event_, IPC_SUBSCRIBE, payload);
  if (res.payload != "{\"success\": true}") {
    throw std::runtime_error("Unable to subscribe ipc event");
  }
}

struct waybar::modules::sway::Ipc::ipc_response
  waybar::modules::sway::Ipc::handleEvent() const
{
  return recv(fd_event_);
}
