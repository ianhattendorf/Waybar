#pragma once

#ifdef FILESYSTEM_EXPERIMENTAL
#include <experimental/filesystem>
#else
#include <filesystem>
#endif

namespace waybar {
#ifdef FILESYSTEM_EXPERIMENTAL
  namespace fs = std::experimental::filesystem;
#else
  namespace fs = std::filesystem;
#endif
}
