// Line diffing and the Git-style three-way merge the sync engine resolves
// concurrent edits with.
//
// The Rust build got its diff from the `similar` crate. There is no equivalent
// C++ dependency worth taking for one algorithm, so this is a direct Myers
// implementation producing the same shape of output: hunks addressed as a
// half-open range in the *base* text plus the lines that replace it, which is
// exactly what the merge below needs.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace lectern::diff {

/// Splits keeping the line terminator attached, so re-joining is exact and a
/// file with no trailing newline round-trips unchanged (Rust's
/// `split_inclusive('\n')`). An empty input yields no lines at all.
std::vector<std::string_view> split_lines(std::string_view text);

/// A replacement: base lines in `[start, end)` become `lines`.
struct Hunk
{
    size_t start = 0;
    size_t end = 0;
    std::vector<std::string> lines;
};

/// Every non-equal region between `base` and `other`.
std::vector<Hunk> hunks_from_diff(const std::vector<std::string_view> &base,
                                  const std::vector<std::string_view> &other);

struct MergeResult
{
    bool clean = false;
    std::string text;  ///< only meaningful when `clean`
};

/// Git-style three-way merge: hunks that touch disjoint regions of `base`
/// combine automatically; hunks that overlap are only auto-resolved if both
/// sides made the exact same edit, otherwise this reports a conflict rather
/// than guessing.
MergeResult three_way_merge(std::string_view base,
                            std::string_view local,
                            std::string_view remote);

}  // namespace lectern::diff
