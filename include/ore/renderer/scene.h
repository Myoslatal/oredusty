// Ore framework - entities, components and the scene world.
//
// A compact ECS: entities are generation-checked indices into a slot table, components live in
// per-type sparse-set pools, and the world itself owns the parent/child links so that
// destroying an entity can hand its children over to the grandparent.
//
//   ore::World world;
//   const ore::Entity e = world.create();
//   world.emplace<ore::Transform>(e, ore::Transform{glm::vec3(1.0f, 0.0f, 0.0f)});
//   world.each<ore::Transform>([](ore::Entity entity, ore::Transform& t) { ... });
#pragma once

#include <ore/core/assert.h>
#include <ore/core/types.h>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
#include <functional>
#include <string>
#include <type_traits>
#include <typeindex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ore {

/// Handle to a world slot. Destroying an entity bumps the slot generation, so old handles stay
/// recognisable as stale instead of silently pointing at whatever reuses the slot.
struct Entity {
    u32 index = kInvalidIndex;
    u32 generation = 0;
    [[nodiscard]] bool valid() const { return index != kInvalidIndex; }
    friend bool operator==(const Entity&, const Entity&) = default;
};

/// Hashes both halves of the handle, so it can be used as a key in unordered containers.
struct EntityHash {
    usize operator()(const Entity& e) const noexcept;
};

/// Local transform of an entity. Transform::world is owned by update_transforms(); the local
/// matrix is composed as translation * rotation * scale.
struct Transform {
    glm::vec3 position{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f}; // identity
    glm::vec3 scale{1.0f};
    glm::mat4 world{1.0f}; // filled by update_transforms()
    [[nodiscard]] glm::mat4 local_matrix() const;
};

/// Mirror of the world hierarchy. The world's own bookkeeping is authoritative; this component
/// is kept in sync by World::set_parent()/destroy() for every entity that owns one, and is
/// emplaced automatically the first time an entity gets a valid parent. An invalid parent
/// means "root".
struct Hierarchy {
    Entity parent{};
};

struct MeshRenderer {
    u32 mesh = kInvalidIndex;
    u32 material = kInvalidIndex;
    bool visible = true;
};

struct CameraComponent {
    f32 fov_y_degrees = 60.0f;
    f32 near_plane = 0.1f;
    f32 far_plane = 100.0f;
    bool primary = true;
};

struct Name {
    std::string value;
};

/// Internal: type-erased view of one component pool. World::each() uses it to pick the smallest
/// pool to walk without knowing the component type at compile time.
class IComponentPool {
public:
    virtual ~IComponentPool() = default;
    [[nodiscard]] virtual usize size() const noexcept = 0;
    [[nodiscard]] virtual bool contains(Entity entity) const noexcept = 0;
    /// Entity stored at dense index \p index; valid for index < size().
    [[nodiscard]] virtual Entity entity_at(usize index) const = 0;
    /// O(1) swap-and-pop removal; no-op when the entity does not own the component.
    virtual void remove(Entity entity) = 0;
    virtual void clear() = 0;
};

/// Internal: sparse-set storage for one component type. The dense arrays keep the components
/// packed with their entities, the sparse array maps an entity index to its dense slot (or
/// kInvalidIndex). Removal swaps the last element into the hole, so it never moves other
/// entities' components around in a way that invalidates the sparse indices.
template <class T>
class ComponentPool final : public IComponentPool {
    static_assert(std::is_object_v<T> && !std::is_const_v<T>, "component types must be non-const objects");
    static_assert(std::is_move_constructible_v<T> && std::is_move_assignable_v<T>,
                  "component types must be movable");

public:
    /// Adds the component, or overwrites the existing one, and returns the stored value.
    template <class... Args>
    T& emplace(Entity entity, Args&&... args);

    [[nodiscard]] usize size() const noexcept override { return components_.size(); }
    [[nodiscard]] bool contains(Entity entity) const noexcept override;
    [[nodiscard]] Entity entity_at(usize index) const override { return entities_[index]; }
    void remove(Entity entity) override;
    void clear() override;

    [[nodiscard]] T* try_get(Entity entity);
    [[nodiscard]] const T* try_get(Entity entity) const;
    /// Internal fast path for World::each(): contains(entity) must already be true.
    [[nodiscard]] T& at_unchecked(Entity entity) { return components_[sparse_[entity.index]]; }

private:
    std::vector<T> components_;
    std::vector<Entity> entities_;
    std::vector<u32> sparse_;
};

/// Policy for World::destroy().
enum class DestroyPolicy : u8 {
    /// Default: the entity goes away and its children are re-parented to its own parent.
    ReparentChildren = 0,
    /// The entity and every one of its descendants are destroyed.
    DestroyChildren,
};

/// Owns entity slots, component pools and the hierarchy.
class World {
public:
    World() = default;

    [[nodiscard]] Entity create();
    /// Destroys \p entity and removes every one of its components. Children are re-parented to
    /// the entity's own parent (the grandparent, or the scene root) by default; pass
    /// DestroyPolicy::DestroyChildren to take the whole subtree down instead. Both policies
    /// unlink the entity from its parent's children list and bump the slot generation, so
    /// handles to it become stale. Destroying a stale handle is a warning, not a crash.
    void destroy(Entity entity, DestroyPolicy policy = DestroyPolicy::ReparentChildren);
    /// Destroys \p entity and all of its descendants - shorthand for DestroyChildren.
    void destroy_recursive(Entity entity);
    /// Drops every entity, component and hierarchy link; generations restart at zero.
    void clear();

    /// True when the handle still refers to the live slot it was handed out for.
    [[nodiscard]] bool valid(Entity entity) const;
    [[nodiscard]] u32 entity_count() const { return entity_count_; }
    /// Number of slots ever handed out; the free list keeps it from growing again after churn.
    [[nodiscard]] u32 capacity() const { return static_cast<u32>(slots_.size()); }

    template <class T, class... Args>
    T& emplace(Entity entity, Args&&... args);
    template <class T>
    [[nodiscard]] bool has(Entity entity) const;
    template <class T>
    T* try_get(Entity entity);
    template <class T>
    [[nodiscard]] const T* try_get(Entity entity) const;
    /// Checked access; asserts when the entity does not own the component.
    template <class T>
    T& get(Entity entity);
    template <class T>
    void remove(Entity entity);
    template <class T>
    [[nodiscard]] usize component_count() const;

    /// Iterates every entity that owns all of the listed components. The callback receives
    /// (Entity, T1&, T2&, ...). The walk starts from the smallest of the pools and skips the
    /// entities that miss any of the others, so a call allocates nothing. 1 to 4 component types
    /// are the intended use; more work as well.
    ///
    /// The callback may create/destroy entities and add components freely, as long as it does
    /// not remove one of the iterated components while the walk is running: removal swap-and-
    /// pops the dense array, so the walk may skip or repeat entities. Destroying an entity that
    /// owns an iterated component counts as such a removal.
    template <class... Ts, class Fn>
    void each(Fn&& fn);

    // --- hierarchy -------------------------------------------------------------------------
    /// Parents \p child to \p parent; an invalid parent makes the child a root. Children keep
    /// their insertion order and re-parenting appends to the end of the new list. Cycles are
    /// rejected with a warning. Also keeps a Hierarchy component on the child in sync.
    void set_parent(Entity child, Entity parent);
    /// Parent of \p entity, or an invalid handle when it is a root (or does not exist).
    [[nodiscard]] Entity parent(Entity entity) const;
    /// Children of \p entity in insertion order. The span is invalidated by set_parent(),
    /// destroy() and any reparenting of the same entity.
    [[nodiscard]] ConstSpan<Entity> children(Entity entity) const;

private:
    struct Slot {
        u32 generation = 0;
        bool alive = false;
    };

    struct HierarchyNode {
        Entity parent{};
        std::vector<Entity> children;
    };

    template <class T>
    [[nodiscard]] ComponentPool<T>* pool() {
        const auto it = pools_.find(std::type_index(typeid(T)));
        return it == pools_.end() ? nullptr : static_cast<ComponentPool<T>*>(it->second.get());
    }

    template <class T>
    [[nodiscard]] const ComponentPool<T>* pool() const {
        const auto it = pools_.find(std::type_index(typeid(T)));
        return it == pools_.end() ? nullptr : static_cast<const ComponentPool<T>*>(it->second.get());
    }

    template <class T>
    ComponentPool<T>& pool_ensure() {
        if (ComponentPool<T>* existing = pool<T>()) return *existing;
        Scope<ComponentPool<T>> created = make_scope<ComponentPool<T>>();
        ComponentPool<T>* raw = created.get();
        pools_.emplace(std::type_index(typeid(T)), std::move(created));
        return *raw;
    }

    /// Destroys one entity, always handing its children over to its own parent.
    void destroy_one(Entity entity);
    /// Unlinks \p child from its current parent and links it under \p parent.
    void attach(Entity child, Entity parent);

    std::vector<Slot> slots_;
    std::vector<u32> free_indices_;
    u32 entity_count_ = 0;
    std::unordered_map<std::type_index, Scope<IComponentPool>> pools_;
    std::unordered_map<u32, HierarchyNode> hierarchy_;
};

// --- ComponentPool -------------------------------------------------------------------------

template <class T>
template <class... Args>
T& ComponentPool<T>::emplace(Entity entity, Args&&... args) {
    ORE_ASSERT_MSG(entity.index != kInvalidIndex, "ComponentPool::emplace: invalid entity handle");
    if (T* existing = try_get(entity)) {
        *existing = T(std::forward<Args>(args)...);
        return *existing;
    }
    if (static_cast<usize>(entity.index) >= sparse_.size()) {
        sparse_.resize(std::max(static_cast<usize>(entity.index) + 1, sparse_.size() * 2), kInvalidIndex);
    }
    components_.emplace_back(std::forward<Args>(args)...);
    entities_.push_back(entity);
    sparse_[entity.index] = static_cast<u32>(components_.size() - 1);
    return components_.back();
}

template <class T>
bool ComponentPool<T>::contains(Entity entity) const noexcept {
    if (static_cast<usize>(entity.index) >= sparse_.size()) return false;
    const u32 dense = sparse_[entity.index];
    return dense != kInvalidIndex && entities_[dense] == entity;
}

template <class T>
void ComponentPool<T>::remove(Entity entity) {
    if (!contains(entity)) return;
    const u32 dense = sparse_[entity.index];
    const u32 last = static_cast<u32>(components_.size() - 1);
    if (dense != last) {
        components_[dense] = std::move(components_[last]);
        entities_[dense] = entities_[last];
        sparse_[entities_[dense].index] = dense;
    }
    components_.pop_back();
    entities_.pop_back();
    sparse_[entity.index] = kInvalidIndex;
}

template <class T>
void ComponentPool<T>::clear() {
    components_.clear();
    entities_.clear();
    sparse_.clear();
}

template <class T>
T* ComponentPool<T>::try_get(Entity entity) {
    if (!contains(entity)) return nullptr;
    return &components_[sparse_[entity.index]];
}

template <class T>
const T* ComponentPool<T>::try_get(Entity entity) const {
    if (!contains(entity)) return nullptr;
    return &components_[sparse_[entity.index]];
}

// --- World ---------------------------------------------------------------------------------

template <class T, class... Args>
T& World::emplace(Entity entity, Args&&... args) {
    ORE_ASSERT_MSG(valid(entity), "World::emplace: entity {} (generation {}) is not alive", entity.index,
                   entity.generation);
    return pool_ensure<T>().emplace(entity, std::forward<Args>(args)...);
}

template <class T>
bool World::has(Entity entity) const {
    return try_get<T>(entity) != nullptr;
}

template <class T>
T* World::try_get(Entity entity) {
    if (!valid(entity)) return nullptr;
    ComponentPool<T>* components = pool<T>();
    return components != nullptr ? components->try_get(entity) : nullptr;
}

template <class T>
const T* World::try_get(Entity entity) const {
    if (!valid(entity)) return nullptr;
    const ComponentPool<T>* components = pool<T>();
    return components != nullptr ? components->try_get(entity) : nullptr;
}

template <class T>
T& World::get(Entity entity) {
    T* component = try_get<T>(entity);
    ORE_ASSERT_MSG(component != nullptr, "World::get: entity {} does not own component {}", entity.index,
                   typeid(T).name());
    return *component;
}

template <class T>
void World::remove(Entity entity) {
    ComponentPool<T>* components = pool<T>();
    if (components != nullptr) components->remove(entity);
}

template <class T>
usize World::component_count() const {
    const ComponentPool<T>* components = pool<T>();
    return components != nullptr ? components->size() : 0;
}

template <class... Ts, class Fn>
void World::each(Fn&& fn) {
    static_assert(sizeof...(Ts) > 0, "World::each() needs at least one component type");
    constexpr usize kCount = sizeof...(Ts);
    const std::array<IComponentPool*, kCount> pools{pool<Ts>()...};

    // A missing pool means nobody owns that component, so there is nothing to visit.
    usize base = kCount;
    usize base_size = 0;
    for (usize i = 0; i < kCount; ++i) {
        if (pools[i] == nullptr) return;
        if (base == kCount || pools[i]->size() < base_size) {
            base = i;
            base_size = pools[i]->size();
        }
    }

    IComponentPool* const base_pool = pools[base];
    // Snapshot the count: entities added by the callback after this point are not visited.
    const usize count = base_pool->size();
    auto visit = [&]<usize... Is>(std::index_sequence<Is...>) {
        for (usize i = 0; i < count; ++i) {
            Entity entity = base_pool->entity_at(i);
            if (!(pools[Is]->contains(entity) && ...)) continue;
            std::invoke(fn, entity, static_cast<ComponentPool<Ts>*>(pools[Is])->at_unchecked(entity)...);
        }
    };
    visit(std::make_index_sequence<kCount>{});
}

/// Recomputes Transform::world for every entity that owns a Transform, parents before children,
/// as parent_world * local_matrix(). An entity whose parent has no Transform is treated as a
/// root (identity parent matrix). Iterative, so deep hierarchies are fine; a hierarchy that
/// contains a cycle is reported with ORE_WARN and broken at the topmost entity that was reached,
/// which keeps the call from hanging.
void update_transforms(World& world);

} // namespace ore
