#include "lowdoc/xml_optimizer.hpp"
#include <stack>
#include <cctype>
#include <algorithm>
#include <unordered_map>
#include <iostream>

namespace lowdoc {

bool XmlOptimizer::is_well_formed(std::string_view xml) {
    std::stack<std::string> tag_stack;
    size_t i = 0;
    const size_t n = xml.size();

    while (i < n) {
        if (xml[i] == '<') {
            if (i + 1 >= n) return false;

            if (xml[i + 1] == '?') {
                size_t end = xml.find("?>", i + 2);
                if (end == std::string_view::npos) return false;
                i = end + 2;
                continue;
            }

            if (xml.substr(i, 4) == "<!--") {
                size_t end = xml.find("-->", i + 4);
                if (end == std::string_view::npos) return false;
                i = end + 3;
                continue;
            }

            if (xml.substr(i, 9) == "<![CDATA[") {
                size_t end = xml.find("]]>", i + 9);
                if (end == std::string_view::npos) return false;
                i = end + 3;
                continue;
            }

            if (xml[i + 1] == '!') {
                size_t end = xml.find('>', i + 2);
                if (end == std::string_view::npos) return false;
                i = end + 1;
                continue;
            }

            if (xml[i + 1] == '/') {
                size_t start_name = i + 2;
                while (start_name < n && std::isspace(static_cast<unsigned char>(xml[start_name]))) {
                    start_name++;
                }
                size_t end_name = start_name;
                while (end_name < n && !std::isspace(static_cast<unsigned char>(xml[end_name])) && xml[end_name] != '>') {
                    end_name++;
                }
                std::string tag_name(xml.substr(start_name, end_name - start_name));
                size_t close_bracket = xml.find('>', end_name);
                if (close_bracket == std::string_view::npos) return false;

                if (tag_stack.empty() || tag_stack.top() != tag_name) {
                    return false;
                }
                tag_stack.pop();
                i = close_bracket + 1;
                continue;
            }

            size_t start_name = i + 1;
            while (start_name < n && std::isspace(static_cast<unsigned char>(xml[start_name]))) {
                start_name++;
            }
            size_t end_name = start_name;
            while (end_name < n && !std::isspace(static_cast<unsigned char>(xml[end_name])) && xml[end_name] != '>' && xml[end_name] != '/') {
                end_name++;
            }
            std::string tag_name(xml.substr(start_name, end_name - start_name));

            bool in_quote = false;
            char quote_char = 0;
            size_t tag_end = end_name;
            bool is_self_closing = false;

            while (tag_end < n) {
                char c = xml[tag_end];
                if (!in_quote && (c == '"' || c == '\'')) {
                    in_quote = true;
                    quote_char = c;
                } else if (in_quote && c == quote_char) {
                    in_quote = false;
                } else if (!in_quote && c == '/' && tag_end + 1 < n && xml[tag_end + 1] == '>') {
                    is_self_closing = true;
                    tag_end += 2;
                    break;
                } else if (!in_quote && c == '>') {
                    tag_end++;
                    break;
                }
                tag_end++;
            }

            if (!is_self_closing) {
                tag_stack.push(tag_name);
            }
            i = tag_end;
        } else {
            i++;
        }
    }

    return tag_stack.empty();
}

std::string XmlOptimizer::strip_comments(std::string_view xml) {
    std::string out;
    out.reserve(xml.size());
    size_t i = 0;
    const size_t n = xml.size();

    while (i < n) {
        if (xml.substr(i, 4) == "<!--") {
            size_t end = xml.find("-->", i + 4);
            if (end != std::string_view::npos) {
                i = end + 3;
            } else {
                break;
            }
        } else {
            out.push_back(xml[i]);
            i++;
        }
    }
    return out;
}

static std::string normalize_numeric_attr(std::string_view val) {
    if (val.empty() || val.find(':') != std::string_view::npos || val.find('/') != std::string_view::npos) {
        return std::string(val);
    }

    std::string res;
    res.reserve(val.size());
    size_t i = 0;
    const size_t n = val.size();

    while (i < n) {
        if ((std::isdigit(static_cast<unsigned char>(val[i])) ||
             ((val[i] == '-' || val[i] == '+') && i + 1 < n && std::isdigit(static_cast<unsigned char>(val[i + 1])))) &&
            (i == 0 || !std::isalnum(static_cast<unsigned char>(val[i - 1])))) {

            size_t num_start = i;
            if (val[i] == '-' || val[i] == '+') i++;
            while (i < n && std::isdigit(static_cast<unsigned char>(val[i]))) i++;

            if (i < n && val[i] == '.' && i + 1 < n && std::isdigit(static_cast<unsigned char>(val[i + 1]))) {
                size_t dot_pos = i;
                i++;
                size_t frac_start = i;
                while (i < n && std::isdigit(static_cast<unsigned char>(val[i]))) i++;
                size_t frac_end = i;

                size_t unit_start = i;
                while (i < n && (std::isalpha(static_cast<unsigned char>(val[i])) || val[i] == '%')) i++;
                std::string_view unit = val.substr(unit_start, i - unit_start);

                bool is_valid_unit = unit.empty() || unit == "cm" || unit == "mm" || unit == "in" ||
                                     unit == "pt" || unit == "pc" || unit == "px" || unit == "%" ||
                                     unit == "em" || unit == "rem";

                if (is_valid_unit) {
                    size_t non_zero = frac_end;
                    while (non_zero > frac_start && val[non_zero - 1] == '0') {
                        non_zero--;
                    }

                    std::string_view int_part = val.substr(num_start, dot_pos - num_start);
                    if (int_part == "-0" && non_zero == frac_start) {
                        res.push_back('0');
                    } else {
                        res.append(int_part);
                    }

                    if (non_zero > frac_start) {
                        res.push_back('.');
                        res.append(val.substr(frac_start, non_zero - frac_start));
                    }
                    res.append(unit);
                    continue;
                }
            }
            res.append(val.substr(num_start, i - num_start));
        } else {
            res.push_back(val[i++]);
        }
    }
    return res;
}

std::string XmlOptimizer::minify(std::string_view xml) {
    std::string cleaned = strip_comments(xml);
    std::string_view sv(cleaned);
    std::string out;
    out.reserve(cleaned.size());

    size_t i = 0;
    const size_t n = sv.size();
    int preserve_space_depth = 0;

    while (i < n) {
        if (sv[i] == '<') {
            if (sv.substr(i, 9) == "<![CDATA[") {
                size_t end = sv.find("]]>", i + 9);
                if (end != std::string_view::npos) {
                    out.append(sv.substr(i, end + 3 - i));
                    i = end + 3;
                } else {
                    out.append(sv.substr(i));
                    break;
                }
                continue;
            }

            if (i + 1 < n && sv[i + 1] == '?') {
                size_t end = sv.find("?>", i + 2);
                if (end != std::string_view::npos) {
                    std::string_view pi = sv.substr(i, end + 2 - i);
                    if (pi.starts_with("<?xml")) {
                        out.append("<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>");
                    } else {
                        out.append(pi);
                    }
                    i = end + 2;
                } else {
                    out.append(sv.substr(i));
                    break;
                }
                continue;
            }

            bool in_quote = false;
            char quote_char = 0;
            size_t tag_end = i + 1;
            while (tag_end < n) {
                char c = sv[tag_end];
                if (!in_quote && (c == '"' || c == '\'')) {
                    in_quote = true;
                    quote_char = c;
                } else if (in_quote && c == quote_char) {
                    in_quote = false;
                } else if (!in_quote && c == '>') {
                    tag_end++;
                    break;
                }
                tag_end++;
            }

            std::string_view tag_content = sv.substr(i, tag_end - i);
            if (tag_content.find("xml:space=\"preserve\"") != std::string_view::npos ||
                tag_content.find("xml:space='preserve'") != std::string_view::npos) {
                preserve_space_depth++;
            } else if (tag_content.starts_with("</") && preserve_space_depth > 0) {
                if (tag_content.find("t>") != std::string_view::npos || preserve_space_depth > 0) {
                    preserve_space_depth--;
                }
            }

            std::string minified_tag;
            minified_tag.reserve(tag_content.size());
            bool last_was_space = false;
            std::string last_attr_name;

            for (size_t t = 0; t < tag_content.size(); ++t) {
                char tc = tag_content[t];
                if (tc == '"' || tc == '\'') {
                    size_t q_end = tag_content.find(tc, t + 1);
                    if (q_end != std::string_view::npos) {
                        std::string_view val = tag_content.substr(t + 1, q_end - (t + 1));
                        minified_tag.push_back('"');
                        if (last_attr_name != "version" && last_attr_name != "encoding" &&
                            last_attr_name != "id" && last_attr_name != "name" &&
                            last_attr_name.find("xmlns") == std::string::npos) {
                            minified_tag.append(normalize_numeric_attr(val));
                        } else {
                            minified_tag.append(val);
                        }
                        minified_tag.push_back('"');
                        t = q_end;
                        last_was_space = false;
                        last_attr_name.clear();
                        continue;
                    }
                }

                if (std::isspace(static_cast<unsigned char>(tc))) {
                    if (!last_was_space && !minified_tag.empty() && minified_tag.back() != '<' && minified_tag.back() != '=' && minified_tag.back() != '/') {
                        minified_tag.push_back(' ');
                        last_was_space = true;
                    }
                } else if (tc == '=') {
                    if (last_was_space && !minified_tag.empty() && minified_tag.back() == ' ') {
                        minified_tag.pop_back();
                    }
                    size_t name_end = minified_tag.size();
                    size_t name_start = name_end;
                    while (name_start > 0 && minified_tag[name_start - 1] != ' ' && minified_tag[name_start - 1] != '<') {
                        name_start--;
                    }
                    last_attr_name = minified_tag.substr(name_start, name_end - name_start);
                    minified_tag.push_back('=');
                    last_was_space = false;
                } else if (tc == '/' && t + 1 < tag_content.size() && tag_content[t + 1] == '>') {
                    if (last_was_space && !minified_tag.empty() && minified_tag.back() == ' ') {
                        minified_tag.pop_back();
                    }
                    minified_tag.push_back('/');
                    last_was_space = false;
                } else {
                    minified_tag.push_back(tc);
                    last_was_space = false;
                }
            }

            out.append(minified_tag);
            i = tag_end;
        } else {
            size_t next_tag = sv.find('<', i);
            if (next_tag == std::string_view::npos) next_tag = n;
            std::string_view text_segment = sv.substr(i, next_tag - i);

            if (preserve_space_depth > 0) {
                out.append(text_segment);
            } else {
                bool all_whitespace = true;
                for (char tc : text_segment) {
                    if (!std::isspace(static_cast<unsigned char>(tc))) {
                        all_whitespace = false;
                        break;
                    }
                }

                if (!all_whitespace) {
                    bool in_space = false;
                    for (char tc : text_segment) {
                        if (std::isspace(static_cast<unsigned char>(tc))) {
                            if (!in_space) {
                                out.push_back(' ');
                                in_space = true;
                            }
                        } else {
                            out.push_back(tc);
                            in_space = false;
                        }
                    }
                }
            }
            i = next_tag;
        }
    }

    return out;
}

std::string XmlOptimizer::remove_attributes_matching(std::string_view xml, const std::vector<std::string>& attr_names) {
    if (attr_names.empty()) return std::string(xml);

    std::string out;
    out.reserve(xml.size());
    size_t i = 0;
    const size_t n = xml.size();

    while (i < n) {
        if (xml[i] == '<') {
            if (xml.substr(i, 4) == "<!--" || xml.substr(i, 9) == "<![CDATA[" || (i + 1 < n && xml[i + 1] == '/')) {
                size_t close = xml.find('>', i);
                if (close == std::string_view::npos) {
                    out.append(xml.substr(i));
                    break;
                }
                out.append(xml.substr(i, close + 1 - i));
                i = close + 1;
                continue;
            }

            size_t tag_end = i + 1;
            bool in_q = false;
            char q_char = 0;
            while (tag_end < n) {
                char c = xml[tag_end];
                if (!in_q && (c == '"' || c == '\'')) {
                    in_q = true;
                    q_char = c;
                } else if (in_q && c == q_char) {
                    in_q = false;
                } else if (!in_q && c == '>') {
                    tag_end++;
                    break;
                }
                tag_end++;
            }

            std::string_view tag_span = xml.substr(i, tag_end - i);
            bool matched_any = false;
            for (const auto& attr : attr_names) {
                if (tag_span.find(attr) != std::string_view::npos) {
                    matched_any = true;
                    break;
                }
            }

            if (!matched_any) {
                out.append(tag_span);
                i = tag_end;
                continue;
            }

            std::string rebuilt_tag;
            rebuilt_tag.reserve(tag_span.size());
            size_t t = 0;
            const size_t tn = tag_span.size();

            while (t < tn && std::isspace(static_cast<unsigned char>(tag_span[t]))) t++;
            if (t < tn && tag_span[t] == '<') rebuilt_tag.push_back(tag_span[t++]);
            if (t < tn && (tag_span[t] == '?' || tag_span[t] == '!')) rebuilt_tag.push_back(tag_span[t++]);
            while (t < tn && !std::isspace(static_cast<unsigned char>(tag_span[t])) && tag_span[t] != '>' && tag_span[t] != '/') {
                rebuilt_tag.push_back(tag_span[t++]);
            }

            while (t < tn) {
                while (t < tn && std::isspace(static_cast<unsigned char>(tag_span[t]))) t++;
                if (t >= tn) break;

                if (tag_span[t] == '/' || tag_span[t] == '>') {
                    rebuilt_tag.push_back(tag_span[t++]);
                    continue;
                }

                size_t name_start = t;
                while (t < tn && !std::isspace(static_cast<unsigned char>(tag_span[t])) && tag_span[t] != '=' && tag_span[t] != '>' && tag_span[t] != '/') {
                    t++;
                }
                std::string_view attr_name = tag_span.substr(name_start, t - name_start);

                while (t < tn && std::isspace(static_cast<unsigned char>(tag_span[t]))) t++;
                if (t < tn && tag_span[t] == '=') {
                    t++;
                    while (t < tn && std::isspace(static_cast<unsigned char>(tag_span[t]))) t++;
                    if (t < tn && (tag_span[t] == '"' || tag_span[t] == '\'')) {
                        char qc = tag_span[t++];
                        size_t val_start = t;
                        while (t < tn && tag_span[t] != qc) t++;
                        size_t val_end = t;
                        if (t < tn && tag_span[t] == qc) t++;

                        bool should_remove = false;
                        for (const auto& a : attr_names) {
                            if (attr_name == a) {
                                should_remove = true;
                                break;
                            }
                        }

                        if (!should_remove) {
                            if (!rebuilt_tag.empty() && rebuilt_tag.back() != '<') {
                                rebuilt_tag.push_back(' ');
                            }
                            rebuilt_tag.append(attr_name);
                            rebuilt_tag.push_back('=');
                            rebuilt_tag.push_back(qc);
                            rebuilt_tag.append(tag_span.substr(val_start, val_end - val_start));
                            rebuilt_tag.push_back(qc);
                        }
                        continue;
                    }
                }
            }

            out.append(rebuilt_tag);
            i = tag_end;
        } else {
            out.push_back(xml[i]);
            i++;
        }
    }

    return out;
}

std::string XmlOptimizer::deduplicate_namespaces(std::string_view xml) {
    if (xml.empty()) return std::string();
    size_t root_start = 0;
    while (root_start < xml.size()) {
        if (xml[root_start] == '<') {
            if (root_start + 1 < xml.size() && (xml[root_start + 1] == '?' || xml[root_start + 1] == '!')) {
                size_t next_gt = xml.find('>', root_start + 1);
                if (next_gt == std::string_view::npos) return std::string(xml);
                root_start = next_gt + 1;
                continue;
            }
            break;
        }
        root_start++;
    }
    if (root_start >= xml.size()) return std::string(xml);

    size_t root_tag_end = xml.find('>', root_start);
    if (root_tag_end == std::string_view::npos) return std::string(xml);
    std::string_view root_tag = xml.substr(root_start, root_tag_end - root_start + 1);

    std::unordered_map<std::string, std::string> root_ns;
    size_t p = 0;
    while (p < root_tag.size()) {
        size_t xmlns_pos = root_tag.find("xmlns", p);
        if (xmlns_pos == std::string_view::npos) break;
        if (xmlns_pos > 0 && !std::isspace(static_cast<unsigned char>(root_tag[xmlns_pos - 1]))) {
            p = xmlns_pos + 5;
            continue;
        }
        size_t eq_pos = root_tag.find('=', xmlns_pos);
        if (eq_pos == std::string_view::npos) break;
        std::string attr_name(root_tag.substr(xmlns_pos, eq_pos - xmlns_pos));
        while (!attr_name.empty() && std::isspace(static_cast<unsigned char>(attr_name.back()))) {
            attr_name.pop_back();
        }
        size_t quote_pos = eq_pos + 1;
        while (quote_pos < root_tag.size() && std::isspace(static_cast<unsigned char>(root_tag[quote_pos]))) {
            quote_pos++;
        }
        if (quote_pos < root_tag.size() && (root_tag[quote_pos] == '"' || root_tag[quote_pos] == '\'')) {
            char qc = root_tag[quote_pos];
            size_t val_end = root_tag.find(qc, quote_pos + 1);
            if (val_end != std::string_view::npos) {
                std::string val(root_tag.substr(quote_pos + 1, val_end - quote_pos - 1));
                root_ns[attr_name] = val;
                p = val_end + 1;
                continue;
            }
        }
        p = eq_pos + 1;
    }

    if (root_ns.empty()) return std::string(xml);

    std::string out;
    out.reserve(xml.size());
    out.append(xml.substr(0, root_tag_end + 1));
    size_t i = root_tag_end + 1;
    const size_t n = xml.size();

    while (i < n) {
        if (xml[i] == '<') {
            if (i + 1 < n && (xml[i + 1] == '/' || xml[i + 1] == '?' || xml[i + 1] == '!')) {
                out.push_back(xml[i++]);
                continue;
            }
            size_t tag_end = i;
            bool in_q = false;
            char qc = 0;
            while (tag_end < n) {
                char c = xml[tag_end];
                if (!in_q && (c == '"' || c == '\'')) {
                    in_q = true;
                    qc = c;
                } else if (in_q && c == qc) {
                    in_q = false;
                } else if (!in_q && c == '>') {
                    tag_end++;
                    break;
                }
                tag_end++;
            }
            if (tag_end <= i) {
                out.push_back(xml[i++]);
                continue;
            }

            std::string_view tag_span = xml.substr(i, tag_end - i);
            if (tag_span.find("xmlns") == std::string_view::npos) {
                out.append(tag_span);
                i = tag_end;
                continue;
            }

            size_t name_end = 1;
            while (name_end < tag_span.size() && !std::isspace(static_cast<unsigned char>(tag_span[name_end])) && tag_span[name_end] != '>' && tag_span[name_end] != '/') {
                name_end++;
            }

            std::string rebuilt_tag;
            rebuilt_tag.reserve(tag_span.size());
            rebuilt_tag.append(tag_span.substr(0, name_end));

            size_t ap = name_end;
            while (ap < tag_span.size()) {
                while (ap < tag_span.size() && std::isspace(static_cast<unsigned char>(tag_span[ap]))) {
                    ap++;
                }
                if (ap >= tag_span.size() || tag_span[ap] == '>' || tag_span[ap] == '/') break;

                size_t an_start = ap;
                while (ap < tag_span.size() && !std::isspace(static_cast<unsigned char>(tag_span[ap])) && tag_span[ap] != '=' && tag_span[ap] != '>' && tag_span[ap] != '/') {
                    ap++;
                }
                std::string attr_name(tag_span.substr(an_start, ap - an_start));

                while (ap < tag_span.size() && std::isspace(static_cast<unsigned char>(tag_span[ap]))) {
                    ap++;
                }
                if (ap < tag_span.size() && tag_span[ap] == '=') {
                    ap++;
                    while (ap < tag_span.size() && std::isspace(static_cast<unsigned char>(tag_span[ap]))) {
                        ap++;
                    }
                    if (ap < tag_span.size() && (tag_span[ap] == '"' || tag_span[ap] == '\'')) {
                        char quote_char = tag_span[ap++];
                        size_t val_start = ap;
                        while (ap < tag_span.size() && tag_span[ap] != quote_char) {
                            ap++;
                        }
                        size_t val_end = ap;
                        if (ap < tag_span.size()) ap++;

                        std::string_view val = tag_span.substr(val_start, val_end - val_start);
                        auto it = root_ns.find(attr_name);
                        if (it != root_ns.end() && it->second == val) {
                            continue;
                        }

                        if (!rebuilt_tag.empty() && rebuilt_tag.back() != '<') {
                            rebuilt_tag.push_back(' ');
                        }
                        rebuilt_tag.append(attr_name);
                        rebuilt_tag.push_back('=');
                        rebuilt_tag.push_back(quote_char);
                        rebuilt_tag.append(val);
                        rebuilt_tag.push_back(quote_char);
                        continue;
                    }
                }
            }
            if (tag_span.size() >= 2 && tag_span[tag_span.size() - 2] == '/') {
                rebuilt_tag.append("/>");
            } else {
                rebuilt_tag.push_back('>');
            }

            out.append(rebuilt_tag);
            i = tag_end;
        } else {
            out.push_back(xml[i++]);
        }
    }

    if (is_well_formed(out)) {
        return out;
    }
    return std::string(xml);
}

std::string XmlOptimizer::consolidate_text_runs(std::string_view xml) {
    if (xml.find("<w:r") == std::string_view::npos || xml.find("<w:p") == std::string_view::npos) {
        return std::string(xml);
    }

    std::string out;
    out.reserve(xml.size());
    size_t i = 0;
    const size_t n = xml.size();

    while (i < n) {
        size_t p_start = xml.find("<w:p", i);
        if (p_start == std::string_view::npos) {
            out.append(xml.substr(i));
            break;
        }

        size_t p_tag_end = xml.find('>', p_start);
        if (p_tag_end == std::string_view::npos) {
            out.append(xml.substr(i));
            break;
        }

        if (p_tag_end > 0 && xml[p_tag_end - 1] == '/') {
            out.append(xml.substr(i, p_tag_end + 1 - i));
            i = p_tag_end + 1;
            continue;
        }

        size_t p_end = xml.find("</w:p>", p_tag_end);
        if (p_end == std::string_view::npos) {
            out.append(xml.substr(i));
            break;
        }

        out.append(xml.substr(i, p_start - i));

        std::string_view p_content = xml.substr(p_start, (p_end + 6) - p_start);

        struct ParsedRun {
            size_t start = 0;
            size_t end = 0;
            std::string rPr;
            std::string text;
            bool preserve = false;
            bool mergeable = false;
        };

        std::vector<ParsedRun> runs;
        size_t r_pos = p_content.find("<w:r", 0);
        while (r_pos != std::string_view::npos && r_pos < p_content.size()) {
            if (r_pos + 4 < p_content.size() && p_content[r_pos + 4] != '>' && !std::isspace(static_cast<unsigned char>(p_content[r_pos + 4])) && p_content[r_pos + 4] != '/') {
                r_pos = p_content.find("<w:r", r_pos + 4);
                continue;
            }

            size_t r_open_end = p_content.find('>', r_pos);
            if (r_open_end == std::string_view::npos) break;
            if (p_content[r_open_end - 1] == '/') {
                r_pos = p_content.find("<w:r", r_open_end + 1);
                continue;
            }

            size_t r_close = p_content.find("</w:r>", r_open_end);
            if (r_close == std::string_view::npos) break;
            size_t r_end = r_close + 6;

            std::string_view inner = p_content.substr(r_open_end + 1, r_close - (r_open_end + 1));

            ParsedRun pr;
            pr.start = r_pos;
            pr.end = r_end;

            bool disallowed = false;
            const char* bad_tags[] = {"<w:drawing", "<w:pict", "<w:tab", "<w:br", "<w:cr", "<w:sym", "<w:fldChar", "<w:footnote", "<w:comment", "<w:noBreak", "<w:softHyphen", "<m:", "<v:"};
            for (const char* bt : bad_tags) {
                if (inner.find(bt) != std::string_view::npos) {
                    disallowed = true;
                    break;
                }
            }

            if (!disallowed) {
                size_t rpr_start = inner.find("<w:rPr");
                if (rpr_start != std::string_view::npos) {
                    size_t rpr_end = inner.find("</w:rPr>", rpr_start);
                    if (rpr_end != std::string_view::npos) {
                        pr.rPr = std::string(inner.substr(rpr_start, (rpr_end + 8) - rpr_start));
                    } else {
                        size_t rpr_sc = inner.find("/>", rpr_start);
                        if (rpr_sc != std::string_view::npos) {
                            pr.rPr = std::string(inner.substr(rpr_start, (rpr_sc + 2) - rpr_start));
                        } else {
                            disallowed = true;
                        }
                    }
                }

                size_t t_first = inner.find("<w:t");
                if (t_first != std::string_view::npos) {
                    size_t t_second = inner.find("<w:t", t_first + 4);
                    if (t_second == std::string_view::npos) {
                        size_t t_open_end = inner.find('>', t_first);
                        size_t t_close = inner.find("</w:t>", t_first);
                        if (t_open_end != std::string_view::npos && t_close != std::string_view::npos && t_open_end < t_close) {
                            std::string_view t_tag = inner.substr(t_first, t_open_end - t_first + 1);
                            if (t_tag.find("xml:space") != std::string_view::npos && t_tag.find("preserve") != std::string_view::npos) {
                                pr.preserve = true;
                            }
                            pr.text = std::string(inner.substr(t_open_end + 1, t_close - (t_open_end + 1)));
                            pr.mergeable = true;
                        }
                    }
                }
            }

            runs.push_back(std::move(pr));
            r_pos = p_content.find("<w:r", r_end);
        }

        bool any_merged = false;
        std::vector<ParsedRun> merged_runs;
        for (size_t ri = 0; ri < runs.size(); ++ri) {
            if (merged_runs.empty() || !merged_runs.back().mergeable || !runs[ri].mergeable) {
                merged_runs.push_back(runs[ri]);
                continue;
            }

            ParsedRun& prev = merged_runs.back();
            const ParsedRun& curr = runs[ri];

            std::string_view between = p_content.substr(prev.end, curr.start - prev.end);
            bool only_ws = true;
            for (char c : between) {
                if (!std::isspace(static_cast<unsigned char>(c))) {
                    only_ws = false;
                    break;
                }
            }

            if (only_ws && prev.rPr == curr.rPr) {
                prev.text += curr.text;
                if (prev.preserve || curr.preserve || (!prev.text.empty() && (prev.text.front() == ' ' || prev.text.back() == ' ')) || prev.text.find("  ") != std::string::npos) {
                    prev.preserve = true;
                }
                prev.end = curr.end;
                any_merged = true;
            } else {
                merged_runs.push_back(curr);
            }
        }

        if (any_merged) {
            std::string new_p;
            new_p.reserve(p_content.size());
            size_t cur_pos = 0;
            for (const auto& mr : merged_runs) {
                new_p.append(p_content.substr(cur_pos, mr.start - cur_pos));
                if (mr.mergeable) {
                    new_p.append("<w:r>");
                    if (!mr.rPr.empty()) {
                        new_p.append(mr.rPr);
                    }
                    if (mr.preserve) {
                        new_p.append("<w:t xml:space=\"preserve\">");
                    } else {
                        new_p.append("<w:t>");
                    }
                    new_p.append(mr.text);
                    new_p.append("</w:t></w:r>");
                } else {
                    new_p.append(p_content.substr(mr.start, mr.end - mr.start));
                }
                cur_pos = mr.end;
            }
            new_p.append(p_content.substr(cur_pos));
            out.append(new_p);
        } else {
            out.append(p_content);
        }

        i = p_end + 6;
    }

    if (is_well_formed(out)) {
        return out;
    }
    return std::string(xml);
}

}
