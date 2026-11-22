#include <mine/runtime/archetype.h>

#include <format>
#include <limits>
#include <string>
#include <utility>

namespace mine::runtime {
namespace {

void set_error(std::string* error, std::string message) {
    if (error != nullptr) *error = std::move(message);
}

constexpr usize kMaxUsize = std::numeric_limits<usize>::max();

} // namespace

std::optional<Archetype> Archetype::build(const ComponentRegistry& registry,
                                          ConstSpan<const ArchetypeComponent> components,
                                          std::string* error) {
    if (error != nullptr) error->clear();
    Archetype archetype;
    archetype.layout_.reserve(components.size());
    usize total = 0;
    usize max_align = 1;
    for (const ArchetypeComponent& component : components) {
        const ComponentType* type = registry.find(component.type_id);
        if (type == nullptr) {
            set_error(error, std::format("archetype names component type {}, which is not registered",
                                         component.type_id));
            return std::nullopt;
        }
        if (archetype.find(component.type_id) != nullptr) {
            set_error(error, std::format("archetype names component type {} ('{}') twice", component.type_id,
                                         type->name()));
            return std::nullopt;
        }
        const usize alignment = type->instance_align();
        const usize mask = alignment - 1;
        if (total > kMaxUsize - mask) {
            set_error(error, std::format("archetype size overflows while aligning component type {} ('{}')",
                                         component.type_id, type->name()));
            return std::nullopt;
        }
        const usize offset = (total + mask) & ~mask;
        if (type->instance_size() > kMaxUsize - offset) {
            set_error(error, std::format("archetype size overflows at component type {} ('{}')",
                                         component.type_id, type->name()));
            return std::nullopt;
        }
        total = offset + type->instance_size();
        if (alignment > max_align) max_align = alignment;
        archetype.layout_.push_back(ComponentLayout{type, offset, component.shared});
    }
    const usize mask = max_align - 1;
    if (total > kMaxUsize - mask) {
        set_error(error, "archetype size overflows while aligning its total size");
        return std::nullopt;
    }
    archetype.total_size_ = (total + mask) & ~mask;
    archetype.alignment_ = max_align;
    return archetype;
}

std::optional<Archetype> Archetype::build(const ComponentRegistry& registry,
                                          std::initializer_list<ArchetypeComponent> components,
                                          std::string* error) {
    return build(registry, ConstSpan<const ArchetypeComponent>(components.begin(), components.size()), error);
}

void Archetype::refresh_shared(ComponentTypeId type_id, const void* shared) {
    for (ComponentLayout& layout : layout_) {
        if (layout.type != nullptr && layout.type->type_id() == type_id) {
            layout.shared = shared;
            return;
        }
    }
}

const Archetype::ComponentLayout* Archetype::find(ComponentTypeId type_id) const {
    for (const ComponentLayout& component : layout_) {
        if (component.type->type_id() == type_id) return &component;
    }
    return nullptr;
}

usize Archetype::offset(ComponentTypeId type_id) const {
    const ComponentLayout* component = find(type_id);
    return component != nullptr ? component->offset : kNoOffset;
}

const ComponentType* Archetype::type(ComponentTypeId type_id) const {
    const ComponentLayout* component = find(type_id);
    return component != nullptr ? component->type : nullptr;
}

const void* Archetype::shared(ComponentTypeId type_id) const {
    const ComponentLayout* component = find(type_id);
    return component != nullptr ? component->shared : nullptr;
}

} // namespace mine::runtime
