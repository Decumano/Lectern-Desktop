// Generated from version.h.in by CMake. Single source of truth is the
// project() version in CMakeLists.txt.
#pragma once

namespace lectern {

inline constexpr const char *cVersion = "0.12.11";

/// Ed25519 public key (hex) the updater verifies release manifests against.
/// Empty means no key was configured at build time, which disables the
/// updater outright — an unsigned manifest is never trusted.
inline constexpr const char *cUpdatePublicKey = "";

/// Where the updater looks for the signed manifest. Empty disables checking.
inline constexpr const char *cUpdateManifestUrl = "";

}  // namespace lectern
