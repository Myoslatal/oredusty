#include <mine/types/click.h>

namespace mine::types {

bool Click::receive(bool on_it) {
    if (!on_it) {
        // A click somewhere else closes the panel: there is one pointer, so there is one thing selected.
        open_ = false;
        return false;
    }
    ++clicks_;
    open_ = !open_;
    return true;
}

} // namespace mine::types
