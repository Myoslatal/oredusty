// Mine - the component type registry: registration and lookup, plus the one mutation the runtime
// allows after registration (replacing an active interface table).
//
// Registration is declarative: a descriptor names the schema and hands in its function tables. The
// registry does not resolve symbols, build a vtable, or inspect a table - that is the loader's and the
// upper layer's business. A name or id is registered once; a mod that wants to change behaviour
// overrides an interface table instead of registering the type again.
#pragma once

#include <mine/runtime/component.h>

#include <t2d/core/types.h>

#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace mine::runtime {

using t2d::usize;

class ComponentRegistry {
public:
    ComponentRegistry() = default;
    ComponentRegistry(const ComponentRegistry&) = delete;
    ComponentRegistry& operator=(const ComponentRegistry&) = delete;
    /// Moving the registry moves the owned pointers; the ComponentType objects themselves stay put,
    /// so pointers returned by register_component() and stored in archetypes stay valid.
    ComponentRegistry(ComponentRegistry&&) noexcept = default;
    ComponentRegistry& operator=(ComponentRegistry&&) noexcept = default;

    /// Validates \p desc and registers it. Returns the registered type, or nullptr and a reason in
    /// \p error. Refused: a descriptor too small for this runtime, id 0, an empty or duplicate name,
    /// version 0, size 0, a zero or non-power-of-two alignment, a null table, interface 0, and a
    /// duplicate interface id.
    [[nodiscard]] const ComponentType* register_component(const ComponentDesc& desc,
                                                          std::string* error = nullptr);

    [[nodiscard]] const ComponentType* find(ComponentTypeId type_id) const;
    [[nodiscard]] const ComponentType* find_by_name(std::string_view name) const;
    [[nodiscard]] usize count() const { return types_.size(); }

    /// Replaces the active \p interface_id table of \p type_id. \p previous receives the table that
    /// was active (nullptr on failure or when the interface has no table yet). Returns false and sets
    /// \p error for an unknown type, an unknown interface, or a null replacement.
    [[nodiscard]] bool override_interface(ComponentTypeId type_id, InterfaceId interface_id,
                                          const void* replacement, const void** previous = nullptr,
                                          std::string* error = nullptr);

    /// Puts p interface_id back to the table declared at registration. Safe to call for an unknown
    /// type or interface (it does nothing); a reload calls it for every override it installed.
    void restore_interface(ComponentTypeId type_id, InterfaceId interface_id);

    /// Updates an already registered type in place. The type id, name, version, instance size and
    /// alignment must be unchanged, and the interface ids must match; only the function tables,
    /// construct/destruct callbacks and base tables are replaced. Returns false and sets p error when
    /// there is no such type, or when the schema would change.
    [[nodiscard]] bool refresh_component(const ComponentDesc& desc, std::string* error = nullptr);

private:
    [[nodiscard]] ComponentType* find_mutable(ComponentTypeId type_id);

    std::vector<std::unique_ptr<ComponentType>> types_;
};

static_assert(!std::is_polymorphic_v<ComponentRegistry>, "the runtime must not need RTTI or a vtable");

} // namespace mine::runtime
