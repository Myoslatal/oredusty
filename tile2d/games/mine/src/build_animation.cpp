#include <mine/build_animation.h>

#include <algorithm>

namespace mine {

void BuildAnimation::start(types::SceneTile* plot, t2d::f32 seconds) {
    if (plot == nullptr || !(seconds > 0.0f)) return;
    const auto already = std::find_if(builds_.begin(), builds_.end(),
                                      [plot](const Build& build) { return build.plot == plot; });
    if (already != builds_.end()) return;   // one sweep per plot: placing it twice does not build it twice
    // The plot is on the map and drawn, but it does nothing until the sweep is over (types/scene_tile.h).
    plot->set_under_construction(true);
    builds_.push_back(Build{plot, 0.0f, seconds});
}

void BuildAnimation::advance(t2d::f32 delta_seconds) {
    if (builds_.empty()) return;
    for (Build& build : builds_) build.elapsed += delta_seconds;
    std::erase_if(builds_, [](const Build& build) {
        if (build.progress() < 1.0f) return false;
        // Done: the plot is finished and may do everything a finished plot does. The null guard is for a
        // caller that filled a Build by hand - start() never takes one.
        if (build.plot != nullptr) build.plot->set_under_construction(false);
        return true;
    });
}

bool BuildAnimation::restore(types::SceneTile* plot, t2d::f32 seconds, t2d::f32 elapsed) {
    // A sweep in progress is exactly what this list holds: no plot, no time, a time that already ran out,
    // or a negative/NaN elapsed is not one (the caller keeps such a plot finished and warns).
    if (plot == nullptr || !(seconds > 0.0f) || !(elapsed >= 0.0f) || !(elapsed < seconds)) return false;
    const auto already = std::find_if(builds_.begin(), builds_.end(),
                                      [plot](const Build& build) { return build.plot == plot; });
    if (already != builds_.end()) return false;   // one sweep per plot, restored or started
    builds_.push_back(Build{plot, elapsed, seconds});
    return true;
}

void BuildAnimation::clear() {
    for (const Build& build : builds_) {
        // A plot outlives the build it was on: leaving the layer finishes it rather than leaving it
        // "under construction" for ever.
        if (build.plot != nullptr) build.plot->set_under_construction(false);
    }
    builds_.clear();
}

t2d::f32 BuildAnimation::progress_of(const types::SceneTile* plot) const {
    for (const Build& build : builds_) {
        if (build.plot == plot) return build.progress();
    }
    return 1.0f;
}

} // namespace mine
