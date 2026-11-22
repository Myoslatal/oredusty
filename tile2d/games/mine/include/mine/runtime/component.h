// Mine - the runtime's component vocabulary: ids, the C descriptor a table hands in, and the type
// object built from it.
//
// An object in this runtime holds **data only**. What it can do lives in its component type, in the
// function tables the descriptor names. The runtime keeps those tables as `const void*`: it never
// calls through one, because the signature belongs to the layer that defined the component, and it
// answers only "which table is active". A registration declares the schema; a mod replaces an active
// table (ComponentType::override_interface) instead of re-registering the type.
#pragma once

#include <t2d/core/types.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace mine::runtime {

using t2d::u32;
using t2d::u64;
using t2d::usize;

/// Stable identity of a component type, shared by the game and whatever a code table adds. It is
/// written down by the layer that defines the component, not handed out in registration order: two
/// processes that load the same tables must name the same component by the same number. 0 is "none".
using ComponentTypeId = u64;
/// Identity of one function table a component type carries (e.g. "update", "draw"). The names are the
/// upper layer's; the runtime only compares numbers. 0 is "none".
using InterfaceId = u32;

inline constexpr ComponentTypeId kNoComponentType = 0;
inline constexpr InterfaceId kNoInterface = 0;

/// The ABI hash a component registered through the mod surface must carry. It is a stable 64-bit FNV-1a
/// of the C surface name, so a module built against another layout is refused before its data is used.
[[nodiscard]] constexpr u64 mod_component_abi_hash() {
    u64 hash = 14695981039346656037ull;
    constexpr const char* text = "mine.runtime.component.desc.v1";
    for (const char* at = text; *at != '\0'; ++at) {
        hash ^= static_cast<u64>(static_cast<unsigned char>(*at));
        hash *= 1099511628211ull;
    }
    return hash;
}

inline constexpr u64 kModComponentAbiHash = mod_component_abi_hash();

/// One entry of a descriptor's interface table: which interface, and the opaque table implementing it.
struct InterfaceBinding {
    InterfaceId interface_id;
    const void* table;
};

/// The first member of every interface table. The runtime does not read it - a table is opaque - but
/// its layout is part of the C surface, so a table built by another compiler can still check itself
/// before handing its address to this runtime.
struct InterfaceTableHeader {
    u32 struct_size;
    u32 version;
};

/// The C-compatible description a registration hands in: what a component is, how big one instance
/// is, how to construct and destroy it, and which function tables it starts with.
///
/// Function pointers take `void* data`, the instance's own bytes, as `self`: that is what lets one
/// table serve objects of unrelated C++ types. There is no base class to cast to, and the runtime
/// does not know a type at all.
struct ComponentDesc {
    /// `sizeof(ComponentDesc)` as the producer compiled it. A descriptor at least as large as the
    /// pre-save/load prefix is accepted; fields beyond the producer's size are treated as null, so
    /// appending to this struct stays backward compatible. A smaller descriptor is refused.
    u32 struct_size;
    ComponentTypeId type_id;
    const char* name;
    /// Schema version of the component. Bumped when the data layout or its meaning changes.
    u32 version;
    u32 instance_size;
    u32 instance_align;
    void (*construct)(void* data);
    void (*destruct)(void* data);
    const InterfaceBinding* interfaces;
    u32 interface_count;
    /// What the producer was compiled as, for the loader to compare. The runtime stores it and does
    /// not interpret it.
    u64 abi_hash;

    // --- appended in the custom-state surface; both may be null -------------------------------------
    /// Serialises the instance state. Called first with out == null and capacity == 0 to query the
    /// number of bytes needed; a second call writes when capacity is large enough. Returns the byte
    /// count, or 0 for "this instance has no saved state".
    u32 (*save)(const void* shared, const void* data, void* out, u32 capacity);
    /// Restores bytes written by save(). Returns the number of bytes consumed; 0 is a refusal. A
    /// successful load must consume exactly the size the chunk recorded.
    u32 (*load)(const void* shared, void* data, const void* in, u32 size);
};

/// The size of the descriptor before save/load were appended. A descriptor at least this large is
/// accepted; its appended callbacks are treated as null. This keeps modules built against the previous
/// header loadable, while a descriptor smaller than the old prefix is still refused.
inline constexpr usize kComponentDescPrefixSize = offsetof(ComponentDesc, save);

static_assert(std::is_standard_layout_v<ComponentDesc>, "ComponentDesc must stay C compatible");
static_assert(std::is_trivially_copyable_v<ComponentDesc>, "ComponentDesc must stay C compatible");
static_assert(std::is_standard_layout_v<InterfaceBinding>, "InterfaceBinding must stay C compatible");
static_assert(std::is_trivially_copyable_v<InterfaceBinding>, "InterfaceBinding must stay C compatible");
static_assert(std::is_standard_layout_v<InterfaceTableHeader>, "InterfaceTableHeader must stay C compatible");
static_assert(std::is_trivially_copyable_v<InterfaceTableHeader>,
              "InterfaceTableHeader must stay C compatible");

/// One registered component type: its schema, its construct/destruct callbacks, and the function
/// tables it carries. A ComponentRegistry owns one and hands out pointers to it; those pointers stay
/// valid until the registry is destroyed (a move of the registry keeps them valid - the types are
/// heap allocated).
///
/// The active table of an interface is the one an object sees. An override at registration time
/// replaces it and keeps the table it replaced, so a mod can wrap the game's implementation and call
/// the previous one. Objects never cache a table pointer: every query goes through this type.
class ComponentType {
public:
    ComponentType(const ComponentType&) = delete;
    ComponentType& operator=(const ComponentType&) = delete;

    [[nodiscard]] ComponentTypeId type_id() const { return type_id_; }
    [[nodiscard]] std::string_view name() const { return name_; }
    [[nodiscard]] u32 version() const { return version_; }
    [[nodiscard]] u32 instance_size() const { return instance_size_; }
    [[nodiscard]] u32 instance_align() const { return instance_align_; }
    [[nodiscard]] u64 abi_hash() const { return abi_hash_; }

    /// Constructs one instance at \p data, if the descriptor supplied a constructor.
    void construct_instance(void* data) const {
        if (construct_ != nullptr) construct_(data);
    }
    /// Destroys one instance at \p data, if the descriptor supplied a destructor.
    void destroy_instance(void* data) const {
        if (destruct_ != nullptr) destruct_(data);
    }
    [[nodiscard]] bool has_constructor() const { return construct_ != nullptr; }
    [[nodiscard]] bool has_destructor() const { return destruct_ != nullptr; }
    [[nodiscard]] bool has_save() const { return save_ != nullptr; }
    [[nodiscard]] bool has_load() const { return load_ != nullptr; }
    /// Calls the optional save() callback. See ComponentDesc::save for the two-phase contract.
    [[nodiscard]] u32 save_instance(const void* shared, const void* data, void* out, u32 capacity) const {
        return save_ != nullptr ? save_(shared, data, out, capacity) : 0;
    }
    /// Calls the optional load() callback; 0 means the bytes were refused.
    [[nodiscard]] u32 load_instance(const void* shared, void* data, const void* in, u32 size) const {
        return load_ != nullptr ? load_(shared, data, in, size) : 0;
    }

    [[nodiscard]] u32 interface_count() const { return static_cast<u32>(interfaces_.size()); }
    [[nodiscard]] bool has_interface(InterfaceId interface_id) const;
    /// The table objects currently see, or nullptr when the interface does not exist.
    [[nodiscard]] const void* active_interface(InterfaceId interface_id) const;
    /// The table the last successful override replaced, or nullptr when nothing has been overridden.
    [[nodiscard]] const void* previous_interface(InterfaceId interface_id) const;
    /// The table declared at registration, kept whatever later overrides do.
    [[nodiscard]] const void* base_interface(InterfaceId interface_id) const;

    /// Replaces the active table of \p interface_id with \p replacement. The table that was active
    /// is written to \p previous when it is not null. Returns false for an unknown interface or a
    /// null replacement. Called while content is registered, before objects run; not thread safe.
    [[nodiscard]] bool override_interface(InterfaceId interface_id, const void* replacement,
                                          const void** previous = nullptr);

    /// Puts the active table back to the one declared at registration. A reload does this before the
    /// module that installed an override is unloaded, so no object is left pointing into code that is
    /// about to disappear.
    void restore_interface(InterfaceId interface_id);

    /// Replaces the schema and function tables of an already registered type **in place**. Used when a
    /// mod is reloaded: the ComponentType pointer (and every archetype built from it) stays valid, while
    /// the construct/destruct callbacks and the base interface tables point at the new library. The
    /// identity and data layout must match; the active tables are reset to the new base.
    [[nodiscard]] bool refresh(const ComponentDesc& desc, std::string* error = nullptr);

private:
    friend class ComponentRegistry;

    struct Slot {
        InterfaceId interface_id = kNoInterface;
        const void* base = nullptr;
        const void* active = nullptr;
        const void* previous = nullptr;
    };

    ComponentType(const ComponentDesc& desc, std::string name);

    [[nodiscard]] const Slot* find_slot(InterfaceId interface_id) const;

    ComponentTypeId type_id_ = kNoComponentType;
    std::string name_;
    u32 version_ = 0;
    u32 instance_size_ = 0;
    u32 instance_align_ = 0;
    u64 abi_hash_ = 0;
    void (*construct_)(void* data) = nullptr;
    void (*destruct_)(void* data) = nullptr;
    u32 (*save_)(const void* shared, const void* data, void* out, u32 capacity) = nullptr;
    u32 (*load_)(const void* shared, void* data, const void* in, u32 size) = nullptr;
    std::vector<Slot> interfaces_;
};

static_assert(!std::is_polymorphic_v<ComponentType>, "the runtime must not need RTTI or a vtable");

} // namespace mine::runtime
