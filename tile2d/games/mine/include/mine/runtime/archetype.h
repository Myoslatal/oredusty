// Mine - an archetype: the composition of one object, and the layout its data is stored in.
//
// An archetype is a list of (component type, shared definition). It computes, once, where every
// component's bytes sit inside one instance and how large one instance is. Objects of the same
// archetype are then just contiguous data blocks of that size - the runtime stores no per-component
// pointer in an object and no function pointer either.
//
// The shared definition is a read-only pointer owned by the caller (a content entry, say). It is not
// the instance data and the runtime never copies or frees it; two archetypes with the same components
// but different shared definitions are different archetypes, which is what lets one "ore" component
// carry stone's numbers for one object and copper's for another.
//
// A ComponentRegistry must outlive every archetype built from it: the layout holds ComponentType*.
#pragma once

#include <mine/runtime/component_registry.h>

#include <t2d/core/types.h>

#include <initializer_list>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

namespace mine::runtime {

using t2d::ConstSpan;
using t2d::usize;

/// "No component at this type in this archetype" - the value Archetype::offset() returns.
inline constexpr usize kNoOffset = static_cast<usize>(-1);

/// One entry of an archetype's input list. The shared pointer may be null for a component that needs
/// no per-content definition.
struct ArchetypeComponent {
    ComponentTypeId type_id = kNoComponentType;
    const void* shared = nullptr;
};

class Archetype {
public:
    /// Where one component's bytes start inside one instance of the archetype.
    struct ComponentLayout {
        const ComponentType* type = nullptr;
        usize offset = 0;
        const void* shared = nullptr;
    };

    Archetype() = default;

    /// Builds the layout in input order. Refuses a component type that is not registered, the same
    /// type named twice, and a size arithmetic overflow; \p error receives the reason. The empty list
    /// is a valid archetype: an object with no data, of size 0 and alignment 1.
    [[nodiscard]] static std::optional<Archetype> build(const ComponentRegistry& registry,
                                                        ConstSpan<const ArchetypeComponent> components,
                                                        std::string* error = nullptr);
    [[nodiscard]] static std::optional<Archetype> build(const ComponentRegistry& registry,
                                                        std::initializer_list<ArchetypeComponent> components,
                                                        std::string* error = nullptr);

    [[nodiscard]] ConstSpan<ComponentLayout> components() const {
        return ConstSpan<ComponentLayout>(layout_.data(), layout_.size());
    }
    [[nodiscard]] usize component_count() const { return layout_.size(); }
    [[nodiscard]] bool empty() const { return layout_.empty(); }
    /// Bytes one instance occupies, including the padding between components and at the end.
    [[nodiscard]] usize total_size() const { return total_size_; }
    /// Alignment every instance must have so that each component's offset is aligned.
    [[nodiscard]] usize alignment() const { return alignment_; }

    [[nodiscard]] const ComponentLayout* find(ComponentTypeId type_id) const;
    /// Replaces the shared definition of one component after a tile type refresh. The layout stays
    /// where it was; only the read-only record objects of this type see on their next call changes.
    void refresh_shared(ComponentTypeId type_id, const void* shared);
    /// Offset of \p type_id inside one instance, or kNoOffset when the archetype does not have it.
    [[nodiscard]] usize offset(ComponentTypeId type_id) const;
    [[nodiscard]] const ComponentType* type(ComponentTypeId type_id) const;
    /// The read-only definition stored for \p type_id, or nullptr when absent or stored as null.
    [[nodiscard]] const void* shared(ComponentTypeId type_id) const;

private:
    std::vector<ComponentLayout> layout_;
    usize total_size_ = 0;
    usize alignment_ = 1;
};

static_assert(!std::is_polymorphic_v<Archetype>, "the runtime must not need RTTI or a vtable");

} // namespace mine::runtime
