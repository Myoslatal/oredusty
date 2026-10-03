#include <ore/renderer/scene.h>

#include <ore/core/log.h>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <unordered_set>

namespace ore {

usize EntityHash::operator()(const Entity& e) const noexcept {
    constexpr u64 kMix = 0x9E37'79B9'7F4A'7C15ull;
    const u64 index = static_cast<u64>(e.index);
    const u64 generation = static_cast<u64>(e.generation);
    u64 hash = index * kMix;
    hash ^= generation + kMix + (hash << 6) + (hash >> 2);
    return static_cast<usize>(hash);
}

glm::mat4 Transform::local_matrix() const {
    const glm::mat4 translation = glm::translate(glm::mat4(1.0f), position);
    const glm::mat4 rotation_matrix = glm::mat4_cast(rotation);
    const glm::mat4 scaling = glm::scale(glm::mat4(1.0f), scale);
    return translation * rotation_matrix * scaling;
}

// --- entities ------------------------------------------------------------------------------

Entity World::create() {
    u32 index = kInvalidIndex;
    if (!free_indices_.empty()) {
        index = free_indices_.back();
        free_indices_.pop_back();
    } else {
        index = static_cast<u32>(slots_.size());
        slots_.push_back(Slot{});
    }
    Slot& slot = slots_[index];
    slot.alive = true;
    ++entity_count_;
    return Entity{index, slot.generation};
}

bool World::valid(Entity entity) const {
    return entity.index != kInvalidIndex && static_cast<usize>(entity.index) < slots_.size() &&
           slots_[entity.index].alive && slots_[entity.index].generation == entity.generation;
}

void World::clear() {
    for (auto& entry : pools_) {
        entry.second->clear();
    }
    slots_.clear();
    free_indices_.clear();
    hierarchy_.clear();
    entity_count_ = 0;
}

void World::destroy(Entity entity, DestroyPolicy policy) {
    if (!valid(entity)) {
        ORE_WARN("World::destroy: entity {} (generation {}) is not alive", entity.index, entity.generation);
        return;
    }
    if (policy == DestroyPolicy::ReparentChildren) {
        destroy_one(entity);
        return;
    }

    // Gather the subtree, then destroy it deepest-first so every parent still alive keeps a
    // children list that only contains living entities.
    std::vector<Entity> stack;
    std::vector<Entity> order;
    stack.push_back(entity);
    while (!stack.empty()) {
        const Entity current = stack.back();
        stack.pop_back();
        order.push_back(current);
        if (const auto it = hierarchy_.find(current.index); it != hierarchy_.end()) {
            stack.insert(stack.end(), it->second.children.begin(), it->second.children.end());
        }
    }
    for (usize i = order.size(); i-- > 0;) {
        destroy_one(order[i]);
    }
}

void World::destroy_recursive(Entity entity) { destroy(entity, DestroyPolicy::DestroyChildren); }

void World::destroy_one(Entity entity) {
    const Entity grandparent = parent(entity);

    // Children survive their parent: hand them over to the grandparent (an invalid grandparent
    // turns them into roots) before the entity itself disappears.
    std::vector<Entity> adopted;
    if (const auto it = hierarchy_.find(entity.index); it != hierarchy_.end()) {
        adopted = it->second.children;
    }
    for (const Entity child : adopted) {
        attach(child, grandparent);
    }

    if (grandparent.valid()) {
        if (const auto it = hierarchy_.find(grandparent.index); it != hierarchy_.end()) {
            auto& siblings = it->second.children;
            siblings.erase(std::remove(siblings.begin(), siblings.end(), entity), siblings.end());
        }
    }
    hierarchy_.erase(entity.index);

    for (auto& entry : pools_) {
        entry.second->remove(entity);
    }

    Slot& slot = slots_[entity.index];
    slot.alive = false;
    ++slot.generation;
    free_indices_.push_back(entity.index);
    --entity_count_;
}

// --- hierarchy -----------------------------------------------------------------------------

void World::set_parent(Entity child, Entity new_parent) {
    if (!valid(child)) {
        ORE_WARN("World::set_parent: entity {} (generation {}) is not alive", child.index, child.generation);
        return;
    }
    if (new_parent.valid() && !valid(new_parent)) {
        ORE_WARN("World::set_parent: parent {} (generation {}) is not alive", new_parent.index,
                 new_parent.generation);
        return;
    }
    if (child == new_parent) {
        ORE_WARN("World::set_parent: entity {} cannot be its own parent", child.index);
        return;
    }
    // Reject cycles: the walk from the new parent up to the root must never reach the child.
    for (Entity cursor = new_parent; cursor.valid(); cursor = parent(cursor)) {
        if (cursor == child) {
            ORE_WARN("World::set_parent: parenting {} under {} would create a cycle", child.index,
                     new_parent.index);
            return;
        }
    }
    attach(child, new_parent);
}

Entity World::parent(Entity entity) const {
    const auto it = hierarchy_.find(entity.index);
    return it == hierarchy_.end() ? Entity{} : it->second.parent;
}

ConstSpan<Entity> World::children(Entity entity) const {
    const auto it = hierarchy_.find(entity.index);
    if (it == hierarchy_.end()) return {};
    return it->second.children;
}

void World::attach(Entity child, Entity new_parent) {
    const Entity previous = parent(child);
    if (previous.valid()) {
        if (auto it = hierarchy_.find(previous.index); it != hierarchy_.end()) {
            auto& siblings = it->second.children;
            siblings.erase(std::remove(siblings.begin(), siblings.end(), child), siblings.end());
        }
    }

    if (new_parent.valid()) {
        hierarchy_[new_parent.index].children.push_back(child);
        hierarchy_[child.index].parent = new_parent;
    } else {
        // An unparented entity only needs a node while it still has children.
        const auto it = hierarchy_.find(child.index);
        if (it != hierarchy_.end()) {
            if (it->second.children.empty()) {
                hierarchy_.erase(it);
            } else {
                it->second.parent = Entity{};
            }
        }
    }

    // Mirror the link in the Hierarchy component: update it when present, add it when the
    // entity gains a real parent.
    if (Hierarchy* component = try_get<Hierarchy>(child)) {
        component->parent = new_parent;
    } else if (new_parent.valid()) {
        emplace<Hierarchy>(child, Hierarchy{new_parent});
    }
}

// --- transforms ----------------------------------------------------------------------------

void update_transforms(World& world) {
    std::vector<Entity> pending;
    world.each<Transform>([&pending](Entity entity, Transform&) { pending.push_back(entity); });
    if (pending.empty()) return;

    std::unordered_set<u32> resolved;
    std::unordered_set<u32> walking;
    std::vector<Entity> chain;
    resolved.reserve(pending.size());

    for (const Entity start : pending) {
        if (resolved.contains(start.index)) continue;

        // Walk up to the topmost ancestor whose world matrix is not final yet, remembering the
        // path so it can be resolved downwards.
        chain.clear();
        walking.clear();
        bool cycle = false;
        Entity cursor = start;
        while (cursor.valid() && !resolved.contains(cursor.index)) {
            if (!walking.insert(cursor.index).second) {
                cycle = true;
                break;
            }
            chain.push_back(cursor);
            cursor = world.parent(cursor);
        }
        if (cycle) {
            ORE_WARN("update_transforms: hierarchy cycle reached from entity {} - breaking it", start.index);
        }

        // chain is [start, ..., topmost]; parents come first when walking it backwards.
        for (usize i = chain.size(); i-- > 0;) {
            const Entity entity = chain[i];
            const bool topmost = i + 1 == chain.size();
            glm::mat4 parent_world{1.0f};
            if (!(cycle && topmost)) {
                const Entity parent = world.parent(entity);
                const Transform* parent_transform = nullptr;
                if (parent.valid()) parent_transform = world.try_get<Transform>(parent);
                if (parent_transform != nullptr) parent_world = parent_transform->world;
            }
            if (Transform* transform = world.try_get<Transform>(entity)) {
                transform->world = parent_world * transform->local_matrix();
            }
            resolved.insert(entity.index);
        }
    }
}

} // namespace ore
