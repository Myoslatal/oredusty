#include <t2d/core/rng.h>

namespace t2d {
namespace {

[[nodiscard]] constexpr u64 rotl(u64 value, int shift) { return (value << shift) | (value >> (64 - shift)); }

[[nodiscard]] constexpr u64 splitmix64(u64& state) {
    u64 z = (state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

} // namespace

void Rng::seed(u64 seed_value) {
    u64 state = seed_value;
    for (u64& word : state_) word = splitmix64(state);
}

u64 Rng::next_u64() {
    const u64 result = rotl(state_[1] * 5ull, 7) * 9ull;
    const u64 t = state_[1] << 17;
    state_[2] ^= state_[0];
    state_[3] ^= state_[1];
    state_[1] ^= state_[2];
    state_[0] ^= state_[3];
    state_[2] ^= t;
    state_[3] = rotl(state_[3], 45);
    return result;
}

u32 Rng::next_u32() { return static_cast<u32>(next_u64() >> 32); }

u32 Rng::next_bounded(u32 bound) {
    if (bound == 0) return 0;
    // Lemire's multiply-shift with rejection: unbiased and identical on every platform.
    const u64 product = static_cast<u64>(next_u32()) * static_cast<u64>(bound);
    u32 low = static_cast<u32>(product);
    if (low < bound) {
        const u32 threshold = static_cast<u32>(-static_cast<i32>(bound)) % bound;
        while (low < threshold) {
            const u64 retry = static_cast<u64>(next_u32()) * static_cast<u64>(bound);
            low = static_cast<u32>(retry);
        }
    }
    return static_cast<u32>(product >> 32);
}

i32 Rng::next_range(i32 low, i32 high) {
    if (high <= low) return low;
    const u32 span = static_cast<u32>(high - low + 1);
    return low + static_cast<i32>(next_bounded(span));
}

f32 Rng::next_float() { return static_cast<f32>(next_u64() >> 40) * (1.0f / 16777216.0f); }

bool Rng::chance(f32 probability) { return next_float() < probability; }

u64 Rng::state_hash() const {
    u64 hash = 0xCBF29CE484222325ull;
    for (u64 word : state_) {
        hash ^= word;
        hash *= 0x100000001B3ull;
    }
    return hash;
}

} // namespace t2d
