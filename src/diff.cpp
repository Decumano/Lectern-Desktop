#include "diff.h"

#include <algorithm>
#include <optional>

namespace lectern::diff {

namespace {

/// Ceiling on the edit distance Myers will search. Beyond this the two texts
/// share almost nothing, and the answer the sync engine wants is the same
/// either way: one hunk replacing everything, which the merge then treats as
/// an overlapping edit and reports as a conflict for the user to resolve. The
/// cap keeps the O(D^2) trace from turning a pathological diff into hundreds
/// of megabytes.
constexpr size_t kMaxEditDistance = 4000;

enum class OpKind
{
    Equal,
    Delete,  ///< a line present in base, absent from other
    Insert,  ///< a line present in other, absent from base
};

struct Op
{
    OpKind kind;
    size_t base_index;   ///< index into base (Equal, Delete)
    size_t other_index;  ///< index into other (Equal, Insert)
};

/// Myers' O(ND) diff with a recorded trace. Returns nullopt when the edit
/// distance exceeds kMaxEditDistance.
std::optional<std::vector<Op>> myers(const std::vector<std::string_view> &base,
                                     const std::vector<std::string_view> &other)
{
    const auto n = static_cast<int>(base.size());
    const auto m = static_cast<int>(other.size());
    const int max_d =
        std::min<int>(n + m, static_cast<int>(kMaxEditDistance));

    // v[k] is the furthest-right base index reached on diagonal k. Stored
    // offset so negative k is addressable.
    std::vector<int> v(static_cast<size_t>(2 * max_d + 3), 0);
    const int offset = max_d + 1;

    // Each level keeps only the diagonals it can reach, so the trace is
    // O(D^2) rather than O(D * (N + M)).
    std::vector<std::vector<int>> trace;
    trace.reserve(static_cast<size_t>(max_d) + 1);

    int found_d = -1;
    for (int d = 0; d <= max_d; ++d)
    {
        trace.emplace_back(v.begin() + (offset - d), v.begin() + (offset + d + 1));

        for (int k = -d; k <= d; k += 2)
        {
            int x = 0;
            if (k == -d ||
                (k != d && v[static_cast<size_t>(k - 1 + offset)] <
                               v[static_cast<size_t>(k + 1 + offset)]))
            {
                x = v[static_cast<size_t>(k + 1 + offset)];  // moved down
            }
            else
            {
                x = v[static_cast<size_t>(k - 1 + offset)] + 1;  // moved right
            }
            int y = x - k;

            while (x < n && y < m &&
                   base[static_cast<size_t>(x)] == other[static_cast<size_t>(y)])
            {
                ++x;
                ++y;
            }

            v[static_cast<size_t>(k + offset)] = x;

            if (x >= n && y >= m)
            {
                found_d = d;
                break;
            }
        }

        if (found_d >= 0)
        {
            break;
        }
    }

    if (found_d < 0)
    {
        return std::nullopt;  // hit the cap
    }

    // Walk the trace backwards, emitting the edit script in reverse.
    std::vector<Op> ops;
    int x = n;
    int y = m;

    for (int d = found_d; d > 0; --d)
    {
        const std::vector<int> &level = trace[static_cast<size_t>(d)];
        const int k = x - y;

        const auto at = [&](int diagonal) {
            return level[static_cast<size_t>(diagonal + d)];
        };

        int prev_k = 0;
        if (k == -d || (k != d && at(k - 1) < at(k + 1)))
        {
            prev_k = k + 1;
        }
        else
        {
            prev_k = k - 1;
        }

        const int prev_x = at(prev_k);
        const int prev_y = prev_x - prev_k;

        while (x > prev_x && y > prev_y)
        {
            --x;
            --y;
            ops.push_back({OpKind::Equal,
                           static_cast<size_t>(x),
                           static_cast<size_t>(y)});
        }

        if (x == prev_x)
        {
            --y;
            ops.push_back({OpKind::Insert,
                           static_cast<size_t>(x),
                           static_cast<size_t>(y)});
        }
        else
        {
            --x;
            ops.push_back({OpKind::Delete,
                           static_cast<size_t>(x),
                           static_cast<size_t>(y)});
        }
    }

    // d == 0 is the common prefix the first snake consumed.
    while (x > 0 && y > 0)
    {
        --x;
        --y;
        ops.push_back(
            {OpKind::Equal, static_cast<size_t>(x), static_cast<size_t>(y)});
    }

    std::reverse(ops.begin(), ops.end());
    return ops;
}

}  // namespace

std::vector<std::string_view> split_lines(std::string_view text)
{
    std::vector<std::string_view> lines;
    if (text.empty())
    {
        return lines;
    }

    size_t start = 0;
    while (start < text.size())
    {
        const size_t newline = text.find('\n', start);
        if (newline == std::string_view::npos)
        {
            lines.push_back(text.substr(start));
            break;
        }
        lines.push_back(text.substr(start, newline - start + 1));
        start = newline + 1;
    }
    return lines;
}

std::vector<Hunk> hunks_from_diff(const std::vector<std::string_view> &base,
                                  const std::vector<std::string_view> &other)
{
    const auto ops = myers(base, other);
    if (!ops)
    {
        // Past the cap: one hunk replacing the whole file.
        Hunk whole;
        whole.start = 0;
        whole.end = base.size();
        whole.lines.reserve(other.size());
        for (const auto line : other)
        {
            whole.lines.emplace_back(line);
        }
        return {whole};
    }

    std::vector<Hunk> hunks;
    size_t i = 0;
    size_t base_pos = 0;
    size_t other_pos = 0;

    while (i < ops->size())
    {
        const Op &op = (*ops)[i];
        if (op.kind == OpKind::Equal)
        {
            ++base_pos;
            ++other_pos;
            ++i;
            continue;
        }

        // A maximal run of non-equal ops becomes one hunk, which is the same
        // grouping `similar`'s ops() produced.
        Hunk hunk;
        hunk.start = base_pos;
        while (i < ops->size() && (*ops)[i].kind != OpKind::Equal)
        {
            if ((*ops)[i].kind == OpKind::Delete)
            {
                ++base_pos;
            }
            else
            {
                hunk.lines.emplace_back(other[other_pos]);
                ++other_pos;
            }
            ++i;
        }
        hunk.end = base_pos;
        hunks.push_back(std::move(hunk));
    }

    return hunks;
}

MergeResult three_way_merge(std::string_view base,
                            std::string_view local,
                            std::string_view remote)
{
    const auto base_lines = split_lines(base);
    const auto local_lines = split_lines(local);
    const auto remote_lines = split_lines(remote);

    const auto local_hunks = hunks_from_diff(base_lines, local_lines);
    const auto remote_hunks = hunks_from_diff(base_lines, remote_lines);

    enum class Side
    {
        Local,
        Remote,
    };
    struct Tagged
    {
        Side side;
        size_t index;
    };

    std::vector<Tagged> tagged;
    tagged.reserve(local_hunks.size() + remote_hunks.size());
    for (size_t i = 0; i < local_hunks.size(); ++i)
    {
        tagged.push_back({Side::Local, i});
    }
    for (size_t i = 0; i < remote_hunks.size(); ++i)
    {
        tagged.push_back({Side::Remote, i});
    }

    const auto hunk_of = [&](const Tagged &item) -> const Hunk & {
        return item.side == Side::Local ? local_hunks[item.index]
                                        : remote_hunks[item.index];
    };

    std::stable_sort(tagged.begin(),
                     tagged.end(),
                     [&](const Tagged &a, const Tagged &b) {
                         return hunk_of(a).start < hunk_of(b).start;
                     });

    // Cluster hunks that touch the same region of base.
    std::vector<std::vector<Tagged>> clusters;
    std::vector<Tagged> current;
    size_t current_end = 0;
    std::optional<std::pair<size_t, size_t>> previous_range;

    for (const auto &item : tagged)
    {
        const Hunk &hunk = hunk_of(item);
        const size_t start = hunk.start;
        const size_t end = hunk.end;

        // Pure insertions are zero-width (start == end), so two insertions at
        // the same base position never satisfy `start < current_end`; without
        // the second check they'd land in separate clusters and both be
        // applied — duplicating the text when both sides inserted the same
        // lines.
        const bool overlaps =
            start < current_end ||
            (start == end &&
             previous_range == std::optional<std::pair<size_t, size_t>>(
                                   {start, end}));
        previous_range = std::pair<size_t, size_t>{start, end};

        if (!current.empty() && overlaps)
        {
            current.push_back(item);
            current_end = std::max(current_end, end);
        }
        else
        {
            if (!current.empty())
            {
                clusters.push_back(std::move(current));
                current.clear();
            }
            current.push_back(item);
            current_end = end;
        }
    }
    if (!current.empty())
    {
        clusters.push_back(std::move(current));
    }

    struct Resolved
    {
        size_t start;
        size_t end;
        const std::vector<std::string> *lines;
    };
    std::vector<Resolved> resolved;

    for (const auto &cluster : clusters)
    {
        std::vector<const Hunk *> locals;
        std::vector<const Hunk *> remotes;
        for (const auto &item : cluster)
        {
            (item.side == Side::Local ? locals : remotes)
                .push_back(&hunk_of(item));
        }

        if (remotes.empty() && locals.size() == 1)
        {
            resolved.push_back(
                {locals[0]->start, locals[0]->end, &locals[0]->lines});
        }
        else if (locals.empty() && remotes.size() == 1)
        {
            resolved.push_back(
                {remotes[0]->start, remotes[0]->end, &remotes[0]->lines});
        }
        else if (locals.size() == 1 && remotes.size() == 1 &&
                 locals[0]->start == remotes[0]->start &&
                 locals[0]->end == remotes[0]->end &&
                 locals[0]->lines == remotes[0]->lines)
        {
            resolved.push_back(
                {locals[0]->start, locals[0]->end, &locals[0]->lines});
        }
        else
        {
            return {false, {}};
        }
    }

    std::string merged;
    size_t position = 0;
    for (const auto &item : resolved)
    {
        for (size_t i = position; i < item.start; ++i)
        {
            merged.append(base_lines[i]);
        }
        for (const auto &line : *item.lines)
        {
            merged.append(line);
        }
        position = item.end;
    }
    for (size_t i = position; i < base_lines.size(); ++i)
    {
        merged.append(base_lines[i]);
    }

    return {true, merged};
}

}  // namespace lectern::diff
