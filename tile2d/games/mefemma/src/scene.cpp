#include <mefemma/scene.h>

namespace mefemma {

void SceneStack::push(std::unique_ptr<Scene> scene) {
    if (scene == nullptr) return;
    pending_.push_back(Op{OpKind::Push, std::move(scene)});
}

void SceneStack::pop() {
    if (scenes_.empty()) return;
    pending_.push_back(Op{OpKind::Pop, nullptr});
}

void SceneStack::replace(std::unique_ptr<Scene> scene) {
    if (scene == nullptr) return;
    pending_.push_back(Op{OpKind::Replace, std::move(scene)});
}

void SceneStack::begin_frame(GameServices& services) { apply(services); }

void SceneStack::update(GameServices& services, f32 delta_seconds) {
    apply(services);
    if (scenes_.empty()) return;
    scenes_.back()->update(services, delta_seconds);
}

void SceneStack::draw(GameServices& services, DrawList& out) const {
    for (const std::unique_ptr<Scene>& scene : scenes_) scene->draw(services, out);
}

void SceneStack::clear() {
    pending_.clear();
    scenes_.clear();
}

Scene* SceneStack::top() { return scenes_.empty() ? nullptr : scenes_.back().get(); }

const Scene* SceneStack::top() const { return scenes_.empty() ? nullptr : scenes_.back().get(); }

void SceneStack::apply(GameServices& services) {
    // A callback may ask for another change (a scene that replaces itself as it enters). The loop takes the
    // queue as it grows; the guard keeps the nested begin_frame() a callback might make from applying the
    // same requests twice.
    if (applying_) return;
    applying_ = true;
    while (!pending_.empty()) {
        Op op = std::move(pending_.front());
        pending_.erase(pending_.begin());
        switch (op.kind) {
            case OpKind::Push: {
                if (!scenes_.empty()) scenes_.back()->on_pause(services);
                scenes_.push_back(std::move(op.scene));
                scenes_.back()->on_enter(services);
                break;
            }
            case OpKind::Pop: {
                if (scenes_.empty()) break;
                scenes_.back()->on_exit(services);
                scenes_.pop_back();
                if (!scenes_.empty()) scenes_.back()->on_resume(services);
                break;
            }
            case OpKind::Replace: {
                if (!scenes_.empty()) {
                    scenes_.back()->on_exit(services);
                    scenes_.pop_back();
                }
                // The scene below was paused when the replaced scene was pushed, and stays paused: a
                // replacement is a new top, not a return to what was underneath.
                scenes_.push_back(std::move(op.scene));
                scenes_.back()->on_enter(services);
                break;
            }
        }
    }
    applying_ = false;
}

} // namespace mefemma
