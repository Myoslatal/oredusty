#include <mine/types/building.h>

namespace mine::types {

const char* building_category_name(BuildingCategory category) {
    switch (category) {
        case BuildingCategory::None: return "none";
        case BuildingCategory::Storage: return "storage";
        case BuildingCategory::Processing: return "processing";
        case BuildingCategory::Extraction: return "extraction";
    }
    return "none";
}

std::optional<BuildingCategory> building_category_from_name(std::string_view name) {
    if (name == "storage") return BuildingCategory::Storage;
    if (name == "processing") return BuildingCategory::Processing;
    if (name == "extraction") return BuildingCategory::Extraction;
    return std::nullopt;
}

} // namespace mine::types
