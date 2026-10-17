// Mine - the plots that are going up, and how far along each one is (docs/GAME_DESIGN.md section 1.37).
//
// A building is placed on the map at once and then **built**: for as long as its entry's `time` says, a scan
// line sweeps the plot from the bottom up and the part below the line is the part that is up. This is the
// half of that which is not pixels - who is being built, how long it has been going, and when it is done -
// so the sweep can be tested without a renderer, the way the sidebar's layout is (mine/build_sidebar.h).
#pragma once

#include <mine/types/scene_tile.h>

#include <t2d/core/types.h>

#include <vector>

namespace mine {

/// What is being built right now, in the order the builds started.
class BuildAnimation {
public:
    struct Build {
        types::SceneTile* plot = nullptr;
        t2d::f32 elapsed = 0.0f;   ///< seconds since it was placed
        t2d::f32 seconds = 0.0f;   ///< what its entry said it takes

        /// How much of it is up, from 0 (just placed) to 1 (done). A build with no time is done at once,
        /// which is what an entry that says `time:0` means.
        [[nodiscard]] t2d::f32 progress() const {
            if (!(seconds > 0.0f)) return 1.0f;
            const t2d::f32 done = elapsed / seconds;
            return done < 0.0f ? 0.0f : (done > 1.0f ? 1.0f : done);
        }
    };

    /// Starts building \\p plot over \\p seconds. A plot that takes no time, one that is already being built,
    /// and a null plot are all ignored: what this holds is exactly "plots with a sweep left to show".
    void start(types::SceneTile* plot, t2d::f32 seconds);

    /// Moves every build on by \\p delta_seconds and forgets the ones that are done.
    void advance(t2d::f32 delta_seconds);

    /// Forgets everything: the layer those plots were on is not the layer on screen any more, so their
    /// sweeps are about plots nobody is looking at.
    void clear() { builds_.clear(); }

    [[nodiscard]] bool empty() const { return builds_.empty(); }
    [[nodiscard]] usize size() const { return builds_.size(); }
    [[nodiscard]] const std::vector<Build>& builds() const { return builds_; }

    /// How much of \\p plot is up. 1 for a plot that is not being built, which is what a drawing pass wants
    /// to hear about everything else on the map.
    [[nodiscard]] t2d::f32 progress_of(const types::SceneTile* plot) const;

private:
    std::vector<Build> builds_;
};

} // namespace mine
