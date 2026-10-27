// The scene stack: deferred changes, only the top updating, everything drawing, and the callbacks a pause
// and a resume are made of.
#include <mefemma/scene.h>

#include <support/test_support.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

using namespace mefemma;

namespace {

struct Probe {
    int enters = 0;
    int exits = 0;
    int pauses = 0;
    int resumes = 0;
    int updates = 0;
    int draws = 0;
    bool pop_on_update = false;
    std::function<void()> on_update;
};

class TestScene final : public Scene {
public:
    TestScene(std::string tag, Probe* probe) : tag_(std::move(tag)), probe_(probe) {}
    [[nodiscard]] const char* name() const override { return tag_.c_str(); }
    void on_enter(GameServices&) override { ++probe_->enters; }
    void on_exit(GameServices&) override { ++probe_->exits; }
    void on_pause(GameServices&) override { ++probe_->pauses; }
    void on_resume(GameServices&) override { ++probe_->resumes; }
    void update(GameServices&, f32) override {
        ++probe_->updates;
        if (probe_->on_update) probe_->on_update();
    }
    void draw(GameServices&, DrawList& out) const override {
        ++probe_->draws;
        out.rect(Aabb2{Vec2{0.0f, 0.0f}, Vec2{1.0f, 1.0f}}, kWhite);
    }
    const std::string& tag() const { return tag_; }

private:
    std::string tag_;
    Probe* probe_;
};

class TestServices final : public GameServices {
public:
    explicit TestServices(SceneStack& stack) : stack_(&stack) {}
    [[nodiscard]] const InputState& input() const override { return input_; }
    [[nodiscard]] const Settings& settings() const override { return settings_; }
    [[nodiscard]] const t2d::Locale& locale() const override { return locale_; }
    [[nodiscard]] std::string_view text(std::string_view id) const override { return id; }
    [[nodiscard]] t2d::TextMetrics measure_text(std::string_view, t2d::u16) const override { return {}; }
    [[nodiscard]] t2d::Rng& random() override { return rng_; }
    [[nodiscard]] f32 elapsed_seconds() const override { return 0.0f; }
    [[nodiscard]] t2d::u64 frame() const override { return 0; }
    [[nodiscard]] t2d::Vec2 viewport() const override { return t2d::Vec2{1280.0f, 720.0f}; }
    void push_scene(std::unique_ptr<Scene> scene) override { stack_->push(std::move(scene)); }
    void pop_scene() override { stack_->pop(); }
    void replace_scene(std::unique_ptr<Scene> scene) override { stack_->replace(std::move(scene)); }
    void quit() override { quit_called = true; }

    bool quit_called = false;

private:
    SceneStack* stack_;
    InputState input_{};
    Settings settings_{};
    t2d::Locale locale_{};
    t2d::Rng rng_{1};
};

} // namespace

T2D_TEST(a_push_is_deferred_and_enters_at_the_next_frame) {
    SceneStack stack;
    TestServices services(stack);
    Probe probe;
    stack.push(std::make_unique<TestScene>("a", &probe));
    T2D_CHECK(stack.empty());
    T2D_CHECK_EQ(stack.pending(), 1u);
    stack.begin_frame(services);
    T2D_CHECK_EQ(stack.size(), 1u);
    T2D_CHECK_EQ(probe.enters, 1);
    T2D_CHECK_EQ(stack.pending(), 0u);
    T2D_CHECK(stack.top() != nullptr);
    T2D_CHECK_EQ(std::string(stack.top()->name()), std::string("a"));
}

T2D_TEST(only_the_top_scene_updates_and_every_scene_draws) {
    SceneStack stack;
    TestServices services(stack);
    Probe first;
    Probe second;
    stack.push(std::make_unique<TestScene>("first", &first));
    stack.begin_frame(services);
    stack.push(std::make_unique<TestScene>("second", &second));
    stack.begin_frame(services);
    T2D_CHECK_EQ(stack.size(), 2u);
    T2D_CHECK_EQ(first.pauses, 1);
    T2D_CHECK_EQ(second.enters, 1);

    stack.update(services, 1.0f / 60.0f);
    T2D_CHECK_EQ(first.updates, 0);
    T2D_CHECK_EQ(second.updates, 1);

    DrawList list;
    stack.draw(services, list);
    T2D_CHECK_EQ(first.draws, 1);
    T2D_CHECK_EQ(second.draws, 1);
    T2D_CHECK_EQ(list.size(), 2u);
}

T2D_TEST(popping_resumes_the_scene_below_and_replacing_does_not) {
    SceneStack stack;
    TestServices services(stack);
    Probe first;
    Probe second;
    stack.push(std::make_unique<TestScene>("first", &first));
    stack.begin_frame(services);
    stack.push(std::make_unique<TestScene>("second", &second));
    stack.begin_frame(services);

    stack.pop();
    stack.begin_frame(services);
    T2D_CHECK_EQ(stack.size(), 1u);
    T2D_CHECK_EQ(second.exits, 1);
    T2D_CHECK_EQ(first.resumes, 1);

    Probe replacement_probe;
    stack.replace(std::make_unique<TestScene>("replacement", &replacement_probe));
    stack.begin_frame(services);
    T2D_CHECK_EQ(stack.size(), 1u);
    T2D_CHECK_EQ(first.exits, 1);
    T2D_CHECK_EQ(first.resumes, 1);   // a replacement is a new top, not a return to what was below
    T2D_CHECK_EQ(replacement_probe.enters, 1);
}

T2D_TEST(a_scene_can_pop_itself_and_the_change_happens_next_frame) {
    SceneStack stack;
    TestServices services(stack);
    Probe probe;
    probe.on_update = [&] { services.pop_scene(); };
    stack.push(std::make_unique<TestScene>("self", &probe));
    stack.begin_frame(services);
    stack.update(services, 0.016f);
    T2D_CHECK_EQ(stack.size(), 1u);   // the pop is queued, not applied inside the scene
    T2D_CHECK_EQ(stack.pending(), 1u);
    stack.begin_frame(services);
    T2D_CHECK(stack.empty());
    T2D_CHECK_EQ(probe.exits, 1);
}

T2D_TEST(an_empty_stack_updates_draws_and_pops_without_complaint) {
    SceneStack stack;
    TestServices services(stack);
    stack.update(services, 0.016f);
    stack.pop();
    stack.begin_frame(services);
    DrawList list;
    stack.draw(services, list);
    T2D_CHECK(stack.empty());
    T2D_CHECK_EQ(stack.size(), 0u);
    T2D_CHECK(list.empty());
}

T2D_TEST(clear_drops_the_scenes_without_callbacks) {
    SceneStack stack;
    TestServices services(stack);
    Probe probe;
    stack.push(std::make_unique<TestScene>("a", &probe));
    stack.begin_frame(services);
    stack.clear();
    T2D_CHECK(stack.empty());
    T2D_CHECK_EQ(probe.exits, 0);
}

T2D_TEST(the_services_quit_flag_is_the_hosts) {
    SceneStack stack;
    TestServices services(stack);
    T2D_CHECK_FALSE(services.quit_called);
    services.quit();
    T2D_CHECK(services.quit_called);
}

T2D_TEST_MAIN
