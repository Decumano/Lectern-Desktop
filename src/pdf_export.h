#pragma once

#include <filesystem>
#include <string>

namespace lectern::pdf {

/// Whether this build can write PDFs natively. When false, fRender throws and
/// the frontend falls back to the webview's print dialog.
bool fSupported();

/// Lays out `html` in a hidden webview and writes it to `out` as a PDF, on paper
/// of exactly this size and with no margins of its own: the HTML draws its own
/// pages (see the frontend's fPaginateDocument). Must run on the UI thread; it
/// spins a nested main loop until the file is written. Throws on failure.
void fRender(const std::string &html, const std::filesystem::path &out,
             double width_mm, double height_mm);

} // namespace lectern::pdf
