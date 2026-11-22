// Mine - one live object: an archetype plus the bytes of one instance of it.
//
// ObjectData owns the block and nothing else. It has no vtable, no component pointer and no cached
// function table: asking for an interface goes through the archetype to the ComponentType every time,
// which is what makes a registration-time override visible to objects that already exist.
//
// The archetype must outlive the object. A block of data whose address could change would invalidate
// every pointer an interface table just handed out, so ObjectData is neither copyable nor movable;
// hold it in a container by pointer (std::unique_ptr) when it has to move. The construct/destruct
// callbacks are C functions and must not throw: this core is meant to be compiled into a code table,
// which the toolchain builds with exceptions off.
#pragma once

#include <mine/runtime/archetype.h>

#include <t2d/core/types.h>

#include <cstddef>
#include <type_traits>

namespace mine::runtime {

using t2d::usize;

class ObjectData {
public:
    /// Allocates one instance of \p archetype, zeroes it, and constructs every component in archetype
    /// order. The block is aligned for the archetype's alignment, so every component's data pointer is
    /// aligned for its type.
    explicit ObjectData(const Archetype& archetype);
    /// Destroys every component in reverse archetype order and frees the block.
    ~ObjectData();

    T2D_NON_MOVABLE(ObjectData);

    [[nodiscard]] const Archetype& archetype() const { return *archetype_; }

    /// This object's bytes for \p type_id, or nullptr when the archetype does not have it.
    [[nodiscard]] void* instance(ComponentTypeId type_id);
    [[nodiscard]] const void* instance(ComponentTypeId type_id) const;
    [[nodiscard]] const void* shared(ComponentTypeId type_id) const;
    [[nodiscard]] bool contains(ComponentTypeId type_id) const {
        return archetype_->find(type_id) != nullptr;
    }

    /// The active table of \p interface_id for \p type_id, or nullptr. Read through the type, never
    /// cached: an override performed after this object was built is what this object sees next time.
    [[nodiscard]] const void* interface(ComponentTypeId type_id, InterfaceId interface_id) const;

private:
    const Archetype* archetype_ = nullptr;
    std::byte* data_ = nullptr;
};

static_assert(!std::is_polymorphic_v<ObjectData>, "the runtime must not need RTTI or a vtable");

} // namespace mine::runtime
