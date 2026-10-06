#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <span>

namespace lowdoc {

class XmlOptimizer {
public:
    static bool is_well_formed(std::string_view xml);
    static std::string minify(std::string_view xml);
    static std::string strip_comments(std::string_view xml);
    static std::string remove_attributes_matching(std::string_view xml, const std::vector<std::string>& attr_names);
    static std::string consolidate_text_runs(std::string_view xml);
    static std::string deduplicate_namespaces(std::string_view xml);
};

}
