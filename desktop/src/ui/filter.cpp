// filter.cpp — the one filter affordance + column sort (see filter.h).
#include "ui/filter.h"

#include <algorithm>
#include <cctype>
#include <numeric>

#include "ui/theme.h"

namespace asmdesk {

namespace {
std::string lower(const std::string &s) {
    std::string r = s;
    for (char &c : r)
        c = static_cast<char>(std::tolower((unsigned char)c));
    return r;
}
bool blank(const std::string &s) {
    for (char c : s)
        if (!std::isspace((unsigned char)c))
            return false;
    return true;
}
} // namespace

bool dt_filter_match(const std::string &query, const std::string &item) {
    if (blank(query))
        return true;
    return lower(item).find(lower(query)) != std::string::npos;
}

int dt_filter_count(const std::string &query,
                    const std::vector<std::string> &items, int *total) {
    if (total != nullptr)
        *total = static_cast<int>(items.size());
    int n = 0;
    for (const std::string &it : items)
        if (dt_filter_match(query, it))
            n++;
    return n;
}

std::vector<int> dt_sorted_order(const std::vector<double> &keys,
                                 bool ascending) {
    std::vector<int> idx(keys.size());
    std::iota(idx.begin(), idx.end(), 0);
    std::stable_sort(idx.begin(), idx.end(), [&](int a, int b) {
        return ascending ? keys[a] < keys[b] : keys[a] > keys[b];
    });
    return idx;
}

std::string dt_filter_bar(dt_filter_state &st,
                          const std::vector<std::string> &items,
                          const char *label) {
    ImGui::SetNextItemWidth(220);
    ImGui::InputTextWithHint("##dt-filter", label, st.buf, sizeof(st.buf));
    std::string query(st.buf);
    int total = 0;
    int n = dt_filter_count(query, items, &total);
    ImGui::SameLine();
    ImGui::TextColored(dt_dim_col(), "showing %d of %d", n, total);
    return query;
}

} // namespace asmdesk
