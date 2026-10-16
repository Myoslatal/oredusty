// Mine - what holds things.
//
// A **container** is the storage half of everything that keeps something: a warehouse, a machine's input,
// a demand's submission box (docs/GAME_DESIGN.md section 1.27). It is a component and not a plot: it
// knows a capacity, what is inside and in which order it arrived, and it answers "put this in" and "take
// that out" by **telling the caller how much it actually took** - a full container takes nothing, and a
// half-full one takes what fits, because a caller that is not told would have to guess.
//
// It is deliberately content-blind: what it holds is a kind and an id (mine/registry.h), the same pair a
// save stores, so anything that can name content can talk to a container - a machine, a demand, a mod.
#pragma once

#include <mine/registry.h>

#include <t2d/core/types.h>

#include <vector>

namespace mine::types {

using t2d::usize;

/// One thing a container holds, and how many of it: what a panel shows and what "take" works on.
struct ContainerSlot {
    ContentKind kind = ContentKind::Item;
    ContentId id = kNoContent;
    usize count = 0;
};

class Container {
public:
    Container() = default;
    /// A container that can hold \p capacity things in total (whatever they are).
    explicit Container(usize capacity) : capacity_(capacity) {}

    // --- how much fits ----------------------------------------------------------------------------
    [[nodiscard]] usize capacity() const { return capacity_; }
    /// Sets the capacity. A capacity below what is already inside **keeps what is there**: what is in a
    /// container is not thrown away by a number in a file - the container is then simply full until
    /// something is taken out.
    void set_capacity(usize capacity) { capacity_ = capacity; }

    // --- what is inside ---------------------------------------------------------------------------
    /// How many things it holds, all kinds together: what the capacity is measured against.
    [[nodiscard]] usize total() const { return total_; }
    /// How many more things fit.
    [[nodiscard]] usize room() const { return capacity_ > total_ ? capacity_ - total_ : 0; }
    [[nodiscard]] bool empty() const { return total_ == 0; }
    [[nodiscard]] bool full() const { return room() == 0; }
    /// How many of one piece of content it holds.
    [[nodiscard]] usize count_of(ContentKind kind, ContentId id) const;
    /// What is inside, in the order each kind first arrived: what a panel lists.
    [[nodiscard]] const std::vector<ContainerSlot>& slots() const { return slots_; }

    // --- putting in and taking out ----------------------------------------------------------------
    /// Puts \p count of one piece of content in and returns **how many it actually took**: none when it
    /// is full, what fits when it is nearly full. Nothing is ever half recorded - the count that comes
    /// back is the count that went in.
    usize add(ContentKind kind, ContentId id, usize count = 1);
    /// Takes \p count out and returns how many it actually gave: none when it does not hold that much.
    usize take(ContentKind kind, ContentId id, usize count = 1);
    /// Empties it. What was inside is gone - the caller takes it out when it wants it.
    void clear();

private:
    /// Finds the slot of one piece of content, or nullptr.
    [[nodiscard]] ContainerSlot* find(ContentKind kind, ContentId id);

    usize capacity_ = 0;
    std::vector<ContainerSlot> slots_;
    usize total_ = 0;
};

} // namespace mine::types
