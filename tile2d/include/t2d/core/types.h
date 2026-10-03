// Tile2D - basic vocabulary types shared by every module.
#pragma once

#include <t2d/config.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace t2d {

using i8 = std::int8_t;
using i16 = std::int16_t;
using i32 = std::int32_t;
using i64 = std::int64_t;
using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using f32 = float;
using f64 = double;
using usize = std::size_t;
using isize = std::ptrdiff_t;

inline constexpr u32 kInvalidId = 0xFFFF'FFFFu;
inline constexpr u16 kInvalidIndex16 = 0xFFFFu;

template <class T>
using Scope = std::unique_ptr<T>;
template <class T>
using Ref = std::shared_ptr<T>;
template <class T>
using Span = std::span<T>;
template <class T>
using ConstSpan = std::span<const T>;

template <class T, class... Args>
[[nodiscard]] Scope<T> make_scope(Args&&... args) {
    return std::make_unique<T>(std::forward<Args>(args)...);
}
template <class T, class... Args>
[[nodiscard]] Ref<T> make_ref(Args&&... args) {
    return std::make_shared<T>(std::forward<Args>(args)...);
}

[[nodiscard]] constexpr u64 align_up(u64 value, u64 alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}
[[nodiscard]] constexpr bool is_power_of_two(u64 value) { return value != 0 && (value & (value - 1)) == 0; }

} // namespace t2d

#define T2D_NON_COPYABLE(Type)                                                                       \
    Type(const Type&) = delete;                                                                      \
    Type& operator=(const Type&) = delete

#define T2D_NON_MOVABLE(Type)                                                                        \
    T2D_NON_COPYABLE(Type);                                                                          \
    Type(Type&&) = delete;                                                                           \
    Type& operator=(Type&&) = delete
