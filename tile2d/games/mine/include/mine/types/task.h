// Mine - the tasks a task block carries: what the designer wrote, and what a plot answers for them.
//
// A task is a **step of a chain**: the content entry declares "tasks::" in execution order, each step
// asks for one thing, and the plot works through them one at a time (docs/MODS.md, docs/GAME_DESIGN.md
// sections 1.52 and 1.53). Two types: **deliver** - the step is done when its item sits in the plot's
// own container, in the count the data names; and **rate** - the step is done when at least the needed
// count of its item arrived within the last "window" seconds. In both, completing consumes exactly the
// items the step asked for and leaves any surplus in the container.
//
// Three halves, as everywhere in the runtime: the definition (TaskSpec, read from the content file), the
// shared record the plot holds (runtime::content::TaskShared: the steps by kind and id), and the
// plot's own state (runtime::content::TaskData: how many are done). This header is the C++ view over the
// last two; it owns nothing and keeps no pointer past the call that made it.
#pragma once

#include <mine/registry.h>
#include <mine/runtime/content/components.h>

#include <optional>
#include <string>
#include <string_view>

namespace mine::types {

/// What a task asks the player to do. The list grows with the designer's tasks; a name the engine does
/// not know is refused while the content file is read, never guessed at.
enum class TaskType : t2d::u8 { Deliver = 1, Rate = 2 };

/// The engine's step types, which are the values TaskStep::type carries.
static_assert(static_cast<t2d::u8>(TaskType::Deliver) == runtime::content::kTaskTypeDeliver,
              "the task type enum and the runtime step type must agree");
static_assert(static_cast<t2d::u8>(TaskType::Rate) == runtime::content::kTaskTypeRate,
              "the task type enum and the runtime step type must agree");

/// The name the data writes for \p type ("deliver", "rate"). A type with no name yet is "".
[[nodiscard]] inline std::string_view task_type_name(TaskType type) {
    switch (type) {
    case TaskType::Deliver: return "deliver";
    case TaskType::Rate: return "rate";
    }
    return {};
}

/// The type a name stands for, or nullopt when it is not one of them.
[[nodiscard]] inline std::optional<TaskType> task_type_from_name(std::string_view name) {
    if (name == "deliver") return TaskType::Deliver;
    if (name == "rate") return TaskType::Rate;
    return std::nullopt;
}

/// One task as the designer wrote it ("tasks::" entry). The name is the entry's key: for reports and
/// logs, never for UI text - the panel says what the type says (docs/TEXT.md).
struct TaskSpec {
    std::string name;
    TaskType type = TaskType::Deliver;
    std::string item; ///< the item content name the step asks for
    t2d::usize count = 0; ///< deliver: how many; rate: how many the window needs
    /// rate: items per second the player has to sustain. 0 for a deliver step.
    t2d::f32 rate = 0.0f;
    /// rate: the measured window, in seconds (1 second when the entry does not say).
    t2d::f32 window = 1.0f;
    /// rate: how long the window's condition has to hold **continuously**, in seconds (0 when the entry
    /// does not say, which keeps "satisfied now is done now").
    t2d::f32 hold = 0.0f;
};

/// One step as the runtime holds it: what to deliver, by kind and id in the running registry.
struct TaskStepView {
    TaskType type = TaskType::Deliver;
    ContentKind kind = ContentKind::Item;
    ContentId id = kNoContent;
    t2d::usize count = 0; ///< deliver: how many; rate: ceil(rate * window), at least one
    t2d::f32 rate = 0.0f;   ///< rate steps: the required items per second
    t2d::f32 window = 0.0f; ///< rate steps: the measured window, in seconds
    t2d::f32 hold = 0.0f;   ///< rate steps: seconds the condition has to keep holding (0 = no hold)
};

/// The chain of one plot, read through its TaskFunctions table. A view: it does not own the object and
/// the caller must not keep it past it.
class Task {
public:
    Task(void* data, const void* shared, const runtime::content::TaskFunctions* functions)
        : data_(data), shared_(shared), functions_(functions) {}

    /// False for a plot with no task component, or one whose table is missing: every question below is
    /// answered as an empty chain.
    [[nodiscard]] bool valid() const {
        return data_ != nullptr && shared_ != nullptr && functions_ != nullptr;
    }
    /// How many steps the content declares.
    [[nodiscard]] t2d::usize count() const {
        return valid() ? static_cast<t2d::usize>(functions_->count(shared_, data_)) : 0;
    }
    /// The index of the current step: the number of steps done so far, clamped to count().
    [[nodiscard]] t2d::usize current() const {
        return valid() ? static_cast<t2d::usize>(functions_->current(shared_, data_)) : 0;
    }
    /// True when there is nothing left to do - including a plot whose content declares no tasks.
    [[nodiscard]] bool complete() const { return current() >= count(); }
    /// The step at \p index, or nothing when the chain has no such step.
    [[nodiscard]] std::optional<TaskStepView> step(t2d::usize index) const {
        if (!valid()) return std::nullopt;
        runtime::content::TaskStep raw{};
        if (functions_->step(shared_, data_, static_cast<t2d::u32>(index), &raw) == 0) return std::nullopt;
        TaskStepView view;
        view.type = static_cast<TaskType>(raw.type);
        view.kind = static_cast<ContentKind>(raw.kind);
        view.id = raw.id;
        view.count = static_cast<t2d::usize>(raw.count);
        view.rate = raw.rate;
        view.window = raw.window;
        view.hold = raw.hold;
        return view;
    }
    /// How many of the current step's item arrived within its measured window ending at \p now: what a
    /// **rate** step's panel shows (0 for any other step, and for a plot with no task component).
    [[nodiscard]] t2d::u32 window_progress(t2d::f32 now) const {
        return valid() ? functions_->window_progress(shared_, data_, now) : 0;
    }
    /// How long the current rate step's condition has been continuously true, in seconds (0 when the
    /// step has no hold, or is not a rate step).
    [[nodiscard]] t2d::f32 held() const { return valid() ? functions_->held(shared_, data_) : 0.0f; }

private:
    void* data_ = nullptr;
    const void* shared_ = nullptr;
    const runtime::content::TaskFunctions* functions_ = nullptr;
};

} // namespace mine::types
