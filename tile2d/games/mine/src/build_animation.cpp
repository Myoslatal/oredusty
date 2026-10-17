#include <mine/build_animation.h>

#include <algorithm>

namespace mine {

void BuildAnimation::start(types::SceneTile* plot, t2d::f32 seconds) {
    if (plot == nullptr || !(seconds > 0.0f)) return;
    const auto already = std::find_if(builds_.begin(), builds_.end(),
                                      [plot](const Build& build) { return build.plot == plot; });
    if (already != builds_.end()) return;   // one sweep per plot: placing it twice does not build it twice
    builds_.push_back(Build{plot, 0.0f, seconds});
}

void BuildAnimation::advance(t2d::f32 delta_seconds) {
    if (builds_.empty()) return;
    for (Build& build : builds_) build.elapsed += delta_seconds;
    std::erase_if(builds_, [](const Build& build) { return build.progress() >= 1.0f; });
}

t2d::f32 BuildAnimation::progress_of(const types::SceneTile* plot) const {
    for (const Build& build : builds_) {
        if (build.plot == plot) return build.progress();
    }
    return 1.0f;
}

} // namespace mine
