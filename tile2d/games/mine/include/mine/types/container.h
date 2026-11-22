// Mine - a view of one object's Container component.
//
// The data belongs to the runtime object: a ContainerData in its block, a ContainerShared in the content
// definition, and the ContainerFunctions table the component type carries. This class holds the three
// pointers and nothing else. It is a view, not a component: a Tile never stores one, and it can be
// copied or dropped without touching the object.
#pragma once

#include <mine/registry.h>

#include <mine/runtime/content/components.h>

#include <t2d/core/types.h>

#include <vector>

namespace mine::types {

using t2d::usize;

/// One stack, named the way the game names content: a kind and the id a save stores.
struct ContainerSlot {
    ContentKind kind = ContentKind::Item;
    ContentId id = kNoContent;
    usize count = 0;
};

class Container {
public:
    Container() = default;
    Container(void* data, const void* shared, const runtime::content::ContainerFunctions* functions);

    [[nodiscard]] bool valid() const { return data_ != nullptr && functions_ != nullptr; }
    [[nodiscard]] usize capacity() const;
    /// How many things it holds, all stacks together: what capacity is measured against.
    [[nodiscard]] usize total() const;
    /// How many more things fit.
    [[nodiscard]] usize room() const;
    [[nodiscard]] bool empty() const { return total() == 0; }
    [[nodiscard]] bool full() const { return room() == 0; }
    [[nodiscard]] usize count_of(ContentKind kind, ContentId id) const;
    /// What is inside, in arrival order: what a panel lists and what a save writes.
    [[nodiscard]] std::vector<ContainerSlot> slots() const;

    /// Puts up to \p count in and returns how many it actually took.
    usize add(ContentKind kind, ContentId id, usize count = 1);
    /// Takes up to \p count out and returns how many it actually gave.
    usize take(ContentKind kind, ContentId id, usize count = 1);

private:
    void* data_ = nullptr;
    const void* shared_ = nullptr;
    const runtime::content::ContainerFunctions* functions_ = nullptr;
};

} // namespace mine::types
