#pragma once

#include <cstring>
#include <type_traits>

namespace waybar::util {

// Read sizeof(To) bytes from From as To
// e.g. char[4] to uint32_t
template <typename To, typename From>
To bit_cast_from_ptr(const From *from) {
  static_assert(std::is_trivially_copyable<From>::value);
  static_assert(std::is_trivially_copyable<To>::value);

  To to;
  std::memcpy(&to, from, sizeof(To));
  return to;
}

} // namespace waybar::util
