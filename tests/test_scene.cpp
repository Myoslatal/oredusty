// Unit tests for the scene module: entities, component pools, each<> and the hierarchy.
#include <ore/renderer/scene.h>

#include <support/test_support.h>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <format>
#include <new>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

using ore::f32;
using ore::u32;
using ore::usize;

using ore::CameraComponent;
using ore::DestroyPolicy;
using ore::Entity;
using ore::EntityHash;
using ore::Hierarchy;
using ore::MeshRenderer;
using ore::Name;
using ore::Transform;
using ore::World;

// --- global allocation counter, used to prove that each<> does not allocate -----------------

namespace {
usize g_allocations = 0;
}

namespace {
[[nodiscard]] void* counted_allocate(std::size_t size) noexcept {
    void* block = std::malloc(size == 0 ? 1 : size);
    if (block != nullptr) ++g_allocations;
    return block;
}
} // namespace

// Replacing the global operators with malloc/free is intentional (it keeps the counter accurate
// even for allocations made inside the standard library). Optimising GCC versions then warn about
// the deliberate allocator mismatch, which is a false positive here.
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif

void* operator new(std::size_t size) {
    void* block = counted_allocate(size);
    if (block == nullptr) throw std::bad_alloc();
    return block;
}

void* operator new[](std::size_t size) { return ::operator new(size); }
void* operator new(std::size_t size, const std::nothrow_t&) noexcept { return counted_allocate(size); }
void* operator new[](std::size_t size, const std::nothrow_t& tag) noexcept {
    return ::operator new(size, tag);
}

// Every delete must pair with the replacements above, otherwise allocating through the nothrow
// forms and freeing through these would mix allocators.
void operator delete(void* block) noexcept { std::free(block); }
void operator delete[](void* block) noexcept { std::free(block); }
void operator delete(void* block, std::size_t) noexcept { std::free(block); }
void operator delete[](void* block, std::size_t) noexcept { std::free(block); }
void operator delete(void* block, const std::nothrow_t&) noexcept { std::free(block); }
void operator delete[](void* block, const std::nothrow_t&) noexcept { std::free(block); }

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace {

[[nodiscard]] glm::quat identity_rotation() { return glm::quat(1.0f, 0.0f, 0.0f, 0.0f); }

[[nodiscard]] Transform make_transform(glm::vec3 position) {
    Transform transform;
    transform.position = position;
    return transform;
}

[[nodiscard]] Transform make_transform(glm::vec3 position, glm::quat rotation, glm::vec3 scale) {
    Transform transform;
    transform.position = position;
    transform.rotation = rotation;
    transform.scale = scale;
    return transform;
}

/// The same composition Transform::local_matrix() promises, written out independently.
[[nodiscard]] glm::mat4 compose(const Transform& transform) {
    const glm::mat4 translation = glm::translate(glm::mat4(1.0f), transform.position);
    const glm::mat4 rotation = glm::mat4_cast(transform.rotation);
    const glm::mat4 scaling = glm::scale(glm::mat4(1.0f), transform.scale);
    return translation * rotation * scaling;
}

void check_mat4_near(const glm::mat4& actual, const glm::mat4& expected, f32 epsilon, std::string_view what) {
    f32 worst = 0.0f;
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            worst = std::max(worst, std::abs(actual[column][row] - expected[column][row]));
        }
    }
    ORE_CHECK_MSG(worst <= epsilon, "{}: max element delta {}", what, worst);
}

[[nodiscard]] bool same_indices(std::vector<u32> actual, std::vector<u32> expected) {
    std::sort(actual.begin(), actual.end());
    std::sort(expected.begin(), expected.end());
    return actual == expected;
}

} // namespace

// --- entities ------------------------------------------------------------------------------

ORE_TEST(entity_create_and_count) {
    World world;
    ORE_CHECK_EQ(world.entity_count(), 0u);
    ORE_CHECK_EQ(world.capacity(), 0u);

    const Entity first = world.create();
    const Entity second = world.create();
    ORE_CHECK(first.valid());
    ORE_CHECK(second.valid());
    ORE_CHECK_NE(first, second);
    ORE_CHECK_EQ(first.index, 0u);
    ORE_CHECK_EQ(second.index, 1u);
    ORE_CHECK_EQ(first.generation, 0u);
    ORE_CHECK(world.valid(first));
    ORE_CHECK(world.valid(second));
    ORE_CHECK_EQ(world.entity_count(), 2u);
    ORE_CHECK_EQ(world.capacity(), 2u);

    ORE_CHECK_FALSE(world.valid(Entity{}));
    ORE_CHECK_FALSE(world.valid(Entity{0u, 7u}));
    ORE_CHECK_FALSE(world.valid(Entity{99u, 0u}));
}

ORE_TEST(entity_destroy_bumps_generation_and_recycles) {
    World world;
    const Entity a = world.create();
    const Entity b = world.create();
    const Entity c = world.create();
    world.emplace<Name>(b, Name{"b"});
    world.emplace<Name>(c, Name{"c"});

    world.destroy(b);
    ORE_CHECK_FALSE(world.valid(b));
    ORE_CHECK_EQ(world.entity_count(), 2u);
    ORE_CHECK_EQ(world.capacity(), 3u);
    ORE_CHECK(world.valid(a));
    ORE_CHECK(world.valid(c));

    world.destroy(c);
    ORE_CHECK_EQ(world.entity_count(), 1u);

    // The free list is LIFO: the slot released last comes back first, with a bumped generation.
    const Entity recycled_c = world.create();
    ORE_CHECK_EQ(recycled_c.index, c.index);
    ORE_CHECK_EQ(recycled_c.generation, c.generation + 1u);
    const Entity recycled_b = world.create();
    ORE_CHECK_EQ(recycled_b.index, b.index);
    ORE_CHECK_EQ(recycled_b.generation, b.generation + 1u);
    ORE_CHECK_EQ(world.capacity(), 3u);
    ORE_CHECK_EQ(world.entity_count(), 3u);

    // Old handles stay stale even though their slots are alive again.
    ORE_CHECK_FALSE(world.valid(b));
    ORE_CHECK_FALSE(world.valid(c));
    ORE_CHECK(world.try_get<Name>(b) == nullptr);
    ORE_CHECK(world.try_get<Name>(c) == nullptr);
    ORE_CHECK_FALSE(world.has<Name>(b));
    ORE_CHECK_EQ(world.component_count<Name>(), 0u);
}

ORE_TEST(destroy_of_a_stale_handle_is_ignored) {
    World world;
    const Entity entity = world.create();
    world.emplace<Name>(entity, Name{"ghost"});
    world.destroy(entity);
    world.destroy(entity); // warns, must not corrupt the slot table
    ORE_CHECK_EQ(world.entity_count(), 0u);
    ORE_CHECK_EQ(world.capacity(), 1u);

    const Entity recycled = world.create();
    ORE_CHECK_EQ(recycled.index, entity.index);
    ORE_CHECK_EQ(recycled.generation, entity.generation + 1u);
    ORE_CHECK_FALSE(world.has<Name>(recycled));
    ORE_CHECK_EQ(world.entity_count(), 1u);
}

ORE_TEST(entity_hash_distinguishes_recycled_slots) {
    World world;
    const Entity first = world.create();
    world.destroy(first);
    const Entity second = world.create();
    ORE_CHECK_EQ(first.index, second.index);
    ORE_CHECK_NE(first, second);

    std::unordered_set<Entity, EntityHash> set;
    set.insert(first);
    set.insert(second);
    set.insert(second);
    ORE_CHECK_EQ(set.size(), 2u);
    ORE_CHECK(set.contains(first));
    ORE_CHECK(set.contains(second));
    ORE_CHECK_FALSE(set.contains(Entity{first.index, first.generation + 7u}));
    ORE_CHECK_EQ(EntityHash{}(first), EntityHash{}(first));
}

// --- components ----------------------------------------------------------------------------

ORE_TEST(component_emplace_get_has_remove) {
    World world;
    const Entity entity = world.create();
    ORE_CHECK_FALSE(world.has<Transform>(entity));
    ORE_CHECK(world.try_get<Transform>(entity) == nullptr);
    ORE_CHECK_EQ(world.component_count<Transform>(), 0u);

    Transform& added = world.emplace<Transform>(entity, make_transform(glm::vec3(1.0f, 2.0f, 3.0f)));
    ORE_CHECK(world.has<Transform>(entity));
    ORE_CHECK_EQ(world.component_count<Transform>(), 1u);
    ORE_CHECK_NEAR(world.get<Transform>(entity).position.y, 2.0f, 1e-6f);
    ORE_CHECK(&world.get<Transform>(entity) == &added);
    ORE_CHECK(world.try_get<Transform>(entity) == &added);

    // A second emplace overwrites in place instead of adding a duplicate.
    world.emplace<Transform>(entity, make_transform(glm::vec3(4.0f, 5.0f, 6.0f)));
    ORE_CHECK_EQ(world.component_count<Transform>(), 1u);
    ORE_CHECK_NEAR(world.get<Transform>(entity).position.x, 4.0f, 1e-6f);

    world.emplace<Name>(entity, Name{"hero"});
    ORE_CHECK_EQ(world.get<Name>(entity).value, std::string("hero"));
    ORE_CHECK_EQ(world.component_count<Name>(), 1u);

    // Removing a component the entity does not own is a no-op.
    world.remove<CameraComponent>(entity);
    ORE_CHECK_EQ(world.component_count<CameraComponent>(), 0u);

    world.remove<Transform>(entity);
    ORE_CHECK_FALSE(world.has<Transform>(entity));
    ORE_CHECK_EQ(world.component_count<Transform>(), 0u);
    ORE_CHECK(world.has<Name>(entity));

    const World& const_world = world;
    ORE_CHECK(const_world.try_get<Name>(entity) != nullptr);
    ORE_CHECK_EQ(const_world.component_count<Name>(), 1u);
    ORE_CHECK(const_world.try_get<Transform>(entity) == nullptr);
}

ORE_TEST(component_removal_keeps_the_sparse_set_consistent) {
    World world;
    std::vector<Entity> entities;
    for (u32 i = 0; i < 5; ++i) {
        const Entity entity = world.create();
        world.emplace<Name>(entity, Name{std::format("entity_{}", i)});
        entities.push_back(entity);
    }

    // Removing from the middle swaps the last component into the hole.
    world.remove<Name>(entities[1]);
    ORE_CHECK_EQ(world.component_count<Name>(), 4u);
    ORE_CHECK_FALSE(world.has<Name>(entities[1]));
    ORE_CHECK_EQ(world.get<Name>(entities[0]).value, std::string("entity_0"));
    ORE_CHECK_EQ(world.get<Name>(entities[2]).value, std::string("entity_2"));
    ORE_CHECK_EQ(world.get<Name>(entities[3]).value, std::string("entity_3"));
    ORE_CHECK_EQ(world.get<Name>(entities[4]).value, std::string("entity_4"));

    // The swapped entity can still be removed and re-added.
    world.remove<Name>(entities[4]);
    world.emplace<Name>(entities[4], Name{"entity_4_again"});
    ORE_CHECK_EQ(world.component_count<Name>(), 4u);
    ORE_CHECK_EQ(world.get<Name>(entities[4]).value, std::string("entity_4_again"));

    usize visited = 0;
    world.each<Name>([&visited](Entity, Name&) { ++visited; });
    ORE_CHECK_EQ(visited, 4u);
}

ORE_TEST(clear_resets_the_world) {
    World world;
    const Entity a = world.create();
    const Entity b = world.create();
    world.set_parent(b, a);
    world.emplace<Transform>(a);
    world.emplace<Name>(b, Name{"b"});

    world.clear();
    ORE_CHECK_EQ(world.entity_count(), 0u);
    ORE_CHECK_EQ(world.capacity(), 0u);
    ORE_CHECK_EQ(world.component_count<Transform>(), 0u);
    ORE_CHECK_EQ(world.component_count<Name>(), 0u);
    ORE_CHECK_FALSE(world.valid(a));
    ORE_CHECK_FALSE(world.valid(b));
    ORE_CHECK_EQ(world.children(a).size(), 0u);
    ORE_CHECK_EQ(world.children(b).size(), 0u);
    ORE_CHECK_FALSE(world.parent(b).valid());

    const Entity fresh = world.create();
    ORE_CHECK_EQ(fresh.index, 0u);
    ORE_CHECK_EQ(fresh.generation, 0u);
    ORE_CHECK(world.valid(fresh));
    ORE_CHECK_EQ(world.entity_count(), 1u);
}

// --- each<> --------------------------------------------------------------------------------

ORE_TEST(each_visits_exactly_the_matching_entities) {
    World world;
    const Entity e0 = world.create(); // Transform + Name + MeshRenderer + Camera
    const Entity e1 = world.create(); // Transform + Name
    const Entity e2 = world.create(); // Transform + MeshRenderer + Camera
    const Entity e3 = world.create(); // Transform
    const Entity e4 = world.create(); // Name + MeshRenderer
    const Entity e5 = world.create(); // MeshRenderer

    world.emplace<Transform>(e0);
    world.emplace<Name>(e0, Name{"e0"});
    world.emplace<MeshRenderer>(e0);
    world.emplace<CameraComponent>(e0);
    world.emplace<Transform>(e1);
    world.emplace<Name>(e1, Name{"e1"});
    world.emplace<Transform>(e2);
    world.emplace<MeshRenderer>(e2);
    world.emplace<CameraComponent>(e2);
    world.emplace<Transform>(e3);
    world.emplace<Name>(e4, Name{"e4"});
    world.emplace<MeshRenderer>(e4);
    world.emplace<MeshRenderer>(e5);

    usize visited = 0;
    std::vector<u32> indices;

    visited = 0;
    world.each<Transform>([&visited](Entity, Transform&) { ++visited; });
    ORE_CHECK_EQ(visited, 4u);

    visited = 0;
    indices.clear();
    world.each<Transform, Name>([&](Entity entity, Transform&, Name&) {
        ++visited;
        indices.push_back(entity.index);
    });
    ORE_CHECK_EQ(visited, 2u);
    ORE_CHECK_MSG(same_indices(indices, {e0.index, e1.index}),
                  "each<Transform, Name> visited the wrong entities");

    visited = 0;
    indices.clear();
    world.each<MeshRenderer, Transform>([&](Entity entity, MeshRenderer&, Transform&) {
        ++visited;
        indices.push_back(entity.index);
    });
    ORE_CHECK_EQ(visited, 2u);
    ORE_CHECK_MSG(same_indices(indices, {e0.index, e2.index}), "each<MeshRenderer, Transform> mismatch");

    visited = 0;
    indices.clear();
    world.each<Transform, Name, MeshRenderer>([&](Entity entity, Transform&, Name&, MeshRenderer&) {
        ++visited;
        indices.push_back(entity.index);
    });
    ORE_CHECK_EQ(visited, 1u);
    ORE_CHECK_MSG(same_indices(indices, {e0.index}), "each<Transform, Name, MeshRenderer> mismatch");

    visited = 0;
    indices.clear();
    world.each<Transform, Name, MeshRenderer, CameraComponent>(
        [&](Entity entity, Transform&, Name&, MeshRenderer&, CameraComponent&) {
            ++visited;
            indices.push_back(entity.index);
        });
    ORE_CHECK_EQ(visited, 1u);
    ORE_CHECK_MSG(same_indices(indices, {e0.index}), "each<...4 types> mismatch");

    visited = 0;
    indices.clear();
    world.each<Transform, CameraComponent>([&](Entity entity, Transform&, CameraComponent&) {
        ++visited;
        indices.push_back(entity.index);
    });
    ORE_CHECK_EQ(visited, 2u);
    ORE_CHECK_MSG(same_indices(indices, {e0.index, e2.index}), "each<Transform, CameraComponent> mismatch");

    // The callback gets mutable references into the pool.
    world.each<Transform>([&](Entity, Transform& transform) { transform.position.x += 10.0f; });
    ORE_CHECK_NEAR(world.get<Transform>(e3).position.x, 10.0f, 1e-6f);
    ORE_CHECK_NEAR(world.get<Transform>(e1).position.x, 10.0f, 1e-6f);
}

ORE_TEST(each_with_missing_pools_visits_nothing) {
    World world;
    for (u32 i = 0; i < 4; ++i) {
        world.emplace<Transform>(world.create());
    }

    usize visited = 0;
    world.each<CameraComponent>([&visited](Entity, CameraComponent&) { ++visited; });
    world.each<CameraComponent, Transform>([&visited](Entity, CameraComponent&, Transform&) { ++visited; });
    world.each<Transform, CameraComponent>([&visited](Entity, Transform&, CameraComponent&) { ++visited; });
    ORE_CHECK_EQ(visited, 0u);

    // ... while the pool that does exist is still walkable.
    world.each<Transform>([&visited](Entity, Transform&) { ++visited; });
    ORE_CHECK_EQ(visited, 4u);
}

ORE_TEST(each_tolerates_structural_changes_to_other_entities) {
    World world;
    std::vector<Entity> transformed;
    for (u32 i = 0; i < 64; ++i) {
        const Entity entity = world.create();
        world.emplace<Transform>(entity);
        world.emplace<Name>(entity, Name{std::format("entity_{}", i)});
        transformed.push_back(entity);
    }
    // Owns neither of the iterated components, so the walk must leave it alone.
    const Entity bystander = world.create();
    world.emplace<MeshRenderer>(bystander);

    bool changed = false;
    usize visited = 0;
    world.each<Transform, Name>([&](Entity, Transform&, Name&) {
        ++visited;
        if (changed) return;
        changed = true;
        // Adding a component to another entity plus creating/destroying entities that are not
        // part of this walk is allowed.
        world.emplace<MeshRenderer>(transformed.front());
        world.destroy(bystander);
        const Entity fresh = world.create();
        world.emplace<Transform>(fresh);
        world.emplace<Name>(fresh, Name{"created_mid_walk"});
    });

    ORE_CHECK(changed);
    ORE_CHECK_EQ(visited, 64u);              // the entity added mid-walk is not visited
    ORE_CHECK_EQ(world.entity_count(), 65u); // 64 walked + 1 fresh - 1 destroyed bystander
    ORE_CHECK_FALSE(world.valid(bystander));
    ORE_CHECK_EQ(world.component_count<Transform>(), 65u);
    ORE_CHECK(world.has<MeshRenderer>(transformed.front()));
}

ORE_TEST(each_does_not_allocate) {
    World world;
    for (u32 i = 0; i < 32; ++i) {
        const Entity entity = world.create();
        world.emplace<Transform>(entity);
        world.emplace<Name>(entity, Name{"x"});
    }

    usize visited = 0;
    g_allocations = 0;
    world.each<Transform, Name>([&visited](Entity, Transform&, Name&) { ++visited; });
    world.each<Transform>([&visited](Entity, Transform&) { ++visited; });
    world.each<Transform, Name, MeshRenderer>(
        [&visited](Entity, Transform&, Name&, MeshRenderer&) { ++visited; });
    const usize allocations = g_allocations;

    ORE_CHECK_EQ(visited, 64u);
    ORE_CHECK_MSG(allocations == 0, "each<> performed {} heap allocation(s)", allocations);
}

// --- transforms and hierarchy ---------------------------------------------------------------

ORE_TEST(transform_local_matrix_composes_translation_rotation_scale) {
    Transform transform;
    transform.position = glm::vec3(1.0f, 2.0f, 3.0f);
    transform.rotation = glm::angleAxis(glm::radians(90.0f), glm::vec3(0.0f, 0.0f, 1.0f));
    transform.scale = glm::vec3(2.0f);

    check_mat4_near(transform.local_matrix(), compose(transform), 1e-5f, "local_matrix");

    // Scale first, then rotate (+90 degrees around Z turns +X into +Y), then translate.
    const glm::vec4 point = transform.local_matrix() * glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);
    ORE_CHECK_NEAR(point.x, 1.0f, 1e-5f);
    ORE_CHECK_NEAR(point.y, 4.0f, 1e-5f);
    ORE_CHECK_NEAR(point.z, 3.0f, 1e-5f);

    // The identity transform is the identity matrix.
    check_mat4_near(Transform{}.local_matrix(), glm::mat4(1.0f), 1e-6f, "identity local_matrix");
}

ORE_TEST(update_transforms_three_level_chain) {
    World world;
    const Entity root = world.create();
    const Entity child = world.create();
    const Entity grandchild = world.create();
    world.set_parent(child, root);
    world.set_parent(grandchild, child);

    const Transform root_transform =
        make_transform(glm::vec3(1.0f, 2.0f, 0.0f),
                       glm::angleAxis(glm::radians(90.0f), glm::vec3(0.0f, 1.0f, 0.0f)), glm::vec3(2.0f));
    const Transform child_transform =
        make_transform(glm::vec3(1.0f, 0.0f, 0.0f), identity_rotation(), glm::vec3(1.0f));
    const Transform grandchild_transform =
        make_transform(glm::vec3(0.0f, 0.0f, 1.0f),
                       glm::angleAxis(glm::radians(90.0f), glm::vec3(1.0f, 0.0f, 0.0f)), glm::vec3(0.5f));
    world.emplace<Transform>(root, root_transform);
    world.emplace<Transform>(child, child_transform);
    world.emplace<Transform>(grandchild, grandchild_transform);

    update_transforms(world);

    const glm::mat4 root_world = compose(root_transform);
    const glm::mat4 child_world = root_world * compose(child_transform);
    const glm::mat4 grandchild_world = child_world * compose(grandchild_transform);
    check_mat4_near(world.get<Transform>(root).world, root_world, 1e-5f, "root world");
    check_mat4_near(world.get<Transform>(child).world, child_world, 1e-5f, "child world");
    check_mat4_near(world.get<Transform>(grandchild).world, grandchild_world, 1e-5f, "grandchild world");

    // The parent rotation and scale really are part of the child matrix: the root's 2x scale and
    // +90 degree yaw move the child to (1, 2, -2) instead of the naively summed (2, 2, 0).
    const glm::vec3 child_origin = glm::vec3(world.get<Transform>(child).world[3]);
    ORE_CHECK_NEAR(child_origin.x, 1.0f, 1e-5f);
    ORE_CHECK_NEAR(child_origin.y, 2.0f, 1e-5f);
    ORE_CHECK_NEAR(child_origin.z, -2.0f, 1e-5f);
    ORE_CHECK(glm::length(child_origin - (root_transform.position + child_transform.position)) > 1.0f);

    // Running it again must not change anything.
    update_transforms(world);
    check_mat4_near(world.get<Transform>(grandchild).world, grandchild_world, 1e-5f,
                    "grandchild world (rerun)");
}

ORE_TEST(update_transforms_treats_children_of_untransformed_entities_as_roots) {
    World world;
    const Entity root = world.create();
    const Entity pivot = world.create(); // deliberately without a Transform
    const Entity leaf = world.create();
    const Entity orphan = world.create();
    world.set_parent(pivot, root);
    world.set_parent(leaf, pivot);

    const Transform root_transform = make_transform(glm::vec3(5.0f, 0.0f, 0.0f));
    const Transform leaf_transform = make_transform(glm::vec3(0.0f, 1.0f, 0.0f));
    const Transform orphan_transform = make_transform(glm::vec3(0.0f, 0.0f, 7.0f));
    world.emplace<Transform>(root, root_transform);
    world.emplace<Transform>(leaf, leaf_transform);
    world.emplace<Transform>(orphan, orphan_transform);

    update_transforms(world);
    check_mat4_near(world.get<Transform>(root).world, compose(root_transform), 1e-5f, "root world");
    check_mat4_near(world.get<Transform>(leaf).world, compose(leaf_transform), 1e-5f, "leaf world");
    check_mat4_near(world.get<Transform>(orphan).world, compose(orphan_transform), 1e-5f, "orphan world");
}

ORE_TEST(update_transforms_deep_chain_of_2000_entities) {
    World world;
    std::vector<Entity> chain;
    chain.reserve(2000);
    Entity parent{};
    for (u32 i = 0; i < 2000; ++i) {
        const Entity entity = world.create();
        world.emplace<Transform>(entity, make_transform(glm::vec3(1.0f, 0.0f, 0.0f)));
        world.set_parent(entity, parent);
        parent = entity;
        chain.push_back(entity);
    }

    update_transforms(world);

    bool finite = true;
    for (const Entity entity : chain) {
        const f32 x = world.get<Transform>(entity).world[3][0];
        finite = finite && std::isfinite(x) && x >= 1.0f;
    }
    ORE_CHECK(finite);
    ORE_CHECK_NEAR(world.get<Transform>(chain.front()).world[3][0], 1.0f, 1e-4f);
    ORE_CHECK_NEAR(world.get<Transform>(chain[999]).world[3][0], 1000.0f, 1e-2f);
    ORE_CHECK_NEAR(world.get<Transform>(chain.back()).world[3][0], 2000.0f, 1e-1f);
    ORE_CHECK_EQ(world.children(chain.front()).size(), 1u);
    ORE_CHECK_EQ(world.parent(chain.back()), chain[1998]);
}

ORE_TEST(hierarchy_set_parent_and_children) {
    World world;
    const Entity root = world.create();
    const Entity a = world.create();
    const Entity b = world.create();
    const Entity c = world.create();

    ORE_CHECK_FALSE(world.parent(root).valid());
    ORE_CHECK_EQ(world.children(root).size(), 0u);

    world.set_parent(a, root);
    world.set_parent(b, root);
    world.set_parent(c, a);
    ORE_CHECK_EQ(world.parent(a), root);
    ORE_CHECK_EQ(world.parent(c), a);
    ORE_CHECK_EQ(world.children(root).size(), 2u);
    ORE_CHECK_EQ(world.children(root)[0], a); // insertion order
    ORE_CHECK_EQ(world.children(root)[1], b);
    ORE_CHECK_EQ(world.children(a).size(), 1u);
    ORE_CHECK_EQ(world.children(a)[0], c);
    ORE_CHECK_EQ(world.children(c).size(), 0u);

    // The Hierarchy component mirrors the tree.
    ORE_CHECK(world.has<Hierarchy>(a));
    ORE_CHECK_EQ(world.get<Hierarchy>(a).parent, root);
    ORE_CHECK(world.try_get<Hierarchy>(root) == nullptr); // roots stay untouched

    // Re-parenting appends to the end of the new sibling list and keeps the subtree.
    world.set_parent(a, b);
    ORE_CHECK_EQ(world.parent(a), b);
    ORE_CHECK_EQ(world.children(root).size(), 1u);
    ORE_CHECK_EQ(world.children(root)[0], b);
    ORE_CHECK_EQ(world.children(b).size(), 1u);
    ORE_CHECK_EQ(world.children(b)[0], a);
    ORE_CHECK_EQ(world.get<Hierarchy>(a).parent, b);
    ORE_CHECK_EQ(world.parent(c), a);

    // Detaching turns the entity back into a root without touching its own children.
    world.set_parent(a, Entity{});
    ORE_CHECK_FALSE(world.parent(a).valid());
    ORE_CHECK_EQ(world.children(b).size(), 0u);
    ORE_CHECK_FALSE(world.get<Hierarchy>(a).parent.valid());
    ORE_CHECK_EQ(world.parent(c), a);
    ORE_CHECK_EQ(world.children(a).size(), 1u);

    // A stale handle is refused instead of corrupting the tree.
    const Entity ghost = world.create();
    world.destroy(ghost);
    world.set_parent(ghost, root);
    ORE_CHECK_EQ(world.children(root).size(), 1u);
}

ORE_TEST(hierarchy_rejects_cycles_and_self_parenting) {
    World world;
    const Entity a = world.create();
    const Entity b = world.create();
    const Entity c = world.create();
    world.set_parent(b, a);
    world.set_parent(c, b);

    world.set_parent(a, c); // would close a -> b -> c -> a
    ORE_CHECK_FALSE(world.parent(a).valid());
    ORE_CHECK_EQ(world.parent(b), a);
    ORE_CHECK_EQ(world.parent(c), b);
    ORE_CHECK_EQ(world.children(a).size(), 1u);

    world.set_parent(a, a);
    ORE_CHECK_FALSE(world.parent(a).valid());
    ORE_CHECK_EQ(world.children(a).size(), 1u);
    ORE_CHECK_EQ(world.parent(b), a);
}

ORE_TEST(destroy_reparents_children_to_the_grandparent) {
    World world;
    const Entity grand = world.create();
    const Entity parent = world.create();
    const Entity first = world.create();
    const Entity second = world.create();
    const Entity grandchild = world.create();
    const Entity sibling = world.create();

    world.set_parent(parent, grand);
    world.set_parent(sibling, grand); // grand's children: [parent, sibling]
    world.set_parent(first, parent);
    world.set_parent(second, parent); // parent's children: [first, second]
    world.set_parent(grandchild, first);
    world.emplace<Name>(parent, Name{"parent"});

    world.destroy(parent);
    ORE_CHECK_FALSE(world.valid(parent));
    ORE_CHECK_EQ(world.component_count<Name>(), 0u);
    ORE_CHECK_EQ(world.entity_count(), 5u);

    // The two children were handed over to the grandparent, behind the existing sibling.
    ORE_CHECK_EQ(world.children(grand).size(), 3u);
    ORE_CHECK_EQ(world.children(grand)[0], sibling);
    ORE_CHECK_EQ(world.children(grand)[1], first);
    ORE_CHECK_EQ(world.children(grand)[2], second);
    ORE_CHECK_EQ(world.parent(first), grand);
    ORE_CHECK_EQ(world.parent(second), grand);
    ORE_CHECK_EQ(world.get<Hierarchy>(first).parent, grand);

    // The deeper subtree is untouched.
    ORE_CHECK_EQ(world.parent(grandchild), first);
    ORE_CHECK_EQ(world.children(first).size(), 1u);
    ORE_CHECK_EQ(world.children(first)[0], grandchild);

    // Destroying a root promotes its children to roots.
    world.destroy(grand);
    ORE_CHECK_FALSE(world.parent(sibling).valid());
    ORE_CHECK_FALSE(world.parent(first).valid());
    ORE_CHECK_EQ(world.parent(grandchild), first);
    ORE_CHECK_EQ(world.children(first).size(), 1u);
    ORE_CHECK_EQ(world.entity_count(), 4u);
}

ORE_TEST(destroy_recursive_takes_the_whole_subtree) {
    World world;
    const Entity root = world.create();
    const Entity child = world.create();
    const Entity grandchild = world.create();
    const Entity survivor = world.create();
    world.set_parent(child, root);
    world.set_parent(grandchild, child);
    world.set_parent(root, survivor);

    for (const Entity entity : {root, child, grandchild, survivor}) {
        world.emplace<Transform>(entity);
    }

    world.destroy_recursive(root);
    ORE_CHECK_FALSE(world.valid(root));
    ORE_CHECK_FALSE(world.valid(child));
    ORE_CHECK_FALSE(world.valid(grandchild));
    ORE_CHECK(world.valid(survivor));
    ORE_CHECK_EQ(world.entity_count(), 1u);
    ORE_CHECK_EQ(world.component_count<Transform>(), 1u);
    ORE_CHECK_EQ(world.children(survivor).size(), 0u);
    ORE_CHECK_EQ(world.capacity(), 4u);
}

// --- stress --------------------------------------------------------------------------------

ORE_TEST(stress_5000_entities_with_recycling) {
    World world;
    std::vector<Entity> entities;
    entities.reserve(5000);
    for (u32 i = 0; i < 5000; ++i) {
        const Entity entity = world.create();
        world.emplace<Transform>(entity, make_transform(glm::vec3(static_cast<f32>(i), 0.0f, 0.0f)));
        world.emplace<Name>(entity, Name{std::format("entity_{}", i)});
        entities.push_back(entity);
    }
    ORE_CHECK_EQ(world.entity_count(), 5000u);
    ORE_CHECK_EQ(world.capacity(), 5000u);
    ORE_CHECK_EQ(world.component_count<Transform>(), 5000u);
    ORE_CHECK_EQ(world.component_count<Name>(), 5000u);

    // Destroy every second entity.
    for (usize i = 0; i < entities.size(); i += 2) {
        world.destroy(entities[i]);
    }
    ORE_CHECK_EQ(world.entity_count(), 2500u);
    ORE_CHECK_EQ(world.component_count<Transform>(), 2500u);
    ORE_CHECK_EQ(world.component_count<Name>(), 2500u);

    bool stale_rejected = true;
    for (usize i = 0; i < entities.size(); i += 2) {
        const bool rejected = !world.valid(entities[i]) && world.try_get<Transform>(entities[i]) == nullptr;
        stale_rejected = stale_rejected && rejected;
    }
    ORE_CHECK(stale_rejected);

    // Re-create the same number: every slot must come back from the free list, generation 1.
    u32 highest_generation = 0;
    for (u32 i = 0; i < 2500; ++i) {
        const Entity entity = world.create();
        world.emplace<Transform>(entity);
        world.emplace<Name>(entity, Name{"recycled"});
        highest_generation = std::max(highest_generation, entity.generation);
    }
    ORE_CHECK_EQ(world.entity_count(), 5000u);
    ORE_CHECK_EQ(world.capacity(), 5000u); // no slot was added
    ORE_CHECK_EQ(world.component_count<Transform>(), 5000u);
    ORE_CHECK_EQ(world.component_count<Name>(), 5000u);
    ORE_CHECK_EQ(highest_generation, 1u);

    bool survivors_alive = true;
    for (usize i = 1; i < entities.size(); i += 2) {
        survivors_alive = survivors_alive && world.valid(entities[i]);
    }
    ORE_CHECK(survivors_alive);

    bool still_stale = true;
    for (usize i = 0; i < entities.size(); i += 2) {
        still_stale = still_stale && !world.valid(entities[i]);
    }
    ORE_CHECK(still_stale);

    usize visited = 0;
    bool all_alive = true;
    bool recycled_seen = false;
    world.each<Transform, Name>([&](Entity entity, Transform&, Name&) {
        ++visited;
        all_alive = all_alive && world.valid(entity);
        recycled_seen = recycled_seen || world.get<Name>(entity).value == "recycled";
    });
    ORE_CHECK_EQ(visited, 5000u);
    ORE_CHECK(all_alive);
    ORE_CHECK(recycled_seen);

    update_transforms(world);
    ORE_CHECK_NEAR(world.get<Transform>(entities[1]).world[3][0], 1.0f, 1e-5f);
    ORE_CHECK_NEAR(world.get<Transform>(entities[4999]).world[3][0], 4999.0f, 1e-3f);
}

ORE_TEST_MAIN
