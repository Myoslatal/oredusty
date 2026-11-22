#include <mine/types/click.h>

#include <mine/runtime/content/components.h>

namespace mine::types {

u32 Click::clicks() const {
    if (data_ == nullptr) return 0;
    return static_cast<const runtime::content::ClickData*>(data_)->clicks;
}

} // namespace mine::types
