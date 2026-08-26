// Reading the frontend out of the executable itself.
//
// A portable single-file build has no `frontend/` directory to read from, so
// the assets are packed (tools/pack_frontend.cpp) and linked into the binary:
// as an RCDATA resource on Windows, via `.incbin` everywhere else. This is the
// reader.
//
// When the build did not embed anything, `available()` is false and the caller
// falls back to reading from disk — which is what a development build and the
// installer-based packages do.
#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace lectern::embedded {

/// True when this build has a frontend pack linked into it.
bool available();

/// One file's bytes, or nullopt when the pack has no such entry.
/// `name` is a relative path with forward slashes, e.g. "vendor/katex.css".
std::optional<std::string_view> read(std::string_view name);

/// Number of files in the pack; 0 when there is none. For diagnostics.
size_t count();

}  // namespace lectern::embedded
