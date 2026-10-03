// Tile2D - deterministic random numbers (xoshiro256** seeded through splitmix64).
//
// std::mt19937 and the <random> distributions are *not* portable: the standard does not pin their
// output, so a server and a client built with different standard libraries would diverge. Every
// random number in the simulation goes through this generator instead.
#pragma once

#include <t2d/core/types.h>

namespace t2d {

class Rng {
public:
    Rng() { seed(0x2545F4914F6CDD1Dull); }
    explicit Rng(u64 seed_value) { seed(seed_value); }

    void seed(u64 seed_value);
    [[nodiscard]] u64 next_u64();
    [[nodiscard]] u32 next_u32();
    /// Uniform in [0, bound); bound must be > 0 (Lemire's method, unbiased and portable).
    [[nodiscard]] u32 next_bounded(u32 bound);
    /// Uniform in [low, high] inclusive.
    [[nodiscard]] i32 next_range(i32 low, i32 high);
    /// Uniform in [0, 1).
    [[nodiscard]] f32 next_float();
    [[nodiscard]] bool chance(f32 probability);
    /// Current state, so tests can snapshot and restore the stream.
    [[nodiscard]] u64 state_hash() const;

private:
    u64 state_[4]{};
};

} // namespace t2d
