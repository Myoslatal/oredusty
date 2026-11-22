// Mine - a view of one object's Click component.
//
// The open flag and the count live in ClickData inside the object; this class carries the pointers the
// component table reads. It is deliberately tiny: what a click means is the ClickFunctions table's
// business, and the panel that shows the container belongs to the screen.
#pragma once

#include <t2d/core/types.h>

#include <mine/runtime/content/components.h>

namespace mine::types {

using t2d::u32;

class Click {
public:
    Click() = default;
    Click(void* data, const runtime::content::ClickFunctions* functions)
        : data_(data), functions_(functions) {}

    [[nodiscard]] bool valid() const { return data_ != nullptr && functions_ != nullptr; }
    /// A click arrived; \p on_it says whether it landed on the thing. Returns whether it took the click.
    bool receive(bool on_it) {
        return functions_ != nullptr && functions_->receive(nullptr, data_, on_it ? 1 : 0) != 0;
    }
    [[nodiscard]] bool open() const { return functions_ != nullptr && functions_->open(nullptr, data_) != 0; }
    [[nodiscard]] u32 clicks() const;

private:
    void* data_ = nullptr;
    const runtime::content::ClickFunctions* functions_ = nullptr;
};

} // namespace mine::types
