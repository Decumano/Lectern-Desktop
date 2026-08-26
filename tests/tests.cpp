// The merge tests from the Rust build's cloud.rs, plus coverage for the diff
// this port had to write from scratch and for the path guard.
#include <catch2/catch_test_macros.hpp>

#include <sodium.h>

#include <array>
#include <stdexcept>
#include <string>
#include <string_view>

#include "diff.h"
#include "paths.h"
#include "updater.h"
#include "util.h"
#include "version.h"
#include "workfolder.h"

using namespace lectern;

namespace {

std::string clean(const std::string &base,
                  const std::string &local,
                  const std::string &remote)
{
    const auto result = diff::three_way_merge(base, local, remote);
    REQUIRE(result.clean);
    return result.text;
}

bool conflicts(const std::string &base,
               const std::string &local,
               const std::string &remote)
{
    return !diff::three_way_merge(base, local, remote).clean;
}

}  // namespace

// ── Three-way merge (ported from src-tauri/src/cloud.rs) ──

TEST_CASE("disjoint edits merge cleanly", "[merge]")
{
    const std::string base = "one\ntwo\nthree\nfour\nfive\n";
    const std::string local = "one\nTWO\nthree\nfour\nfive\n";
    const std::string remote = "one\ntwo\nthree\nfour\nFIVE\n";
    REQUIRE(clean(base, local, remote) == "one\nTWO\nthree\nfour\nFIVE\n");
}

TEST_CASE("identical edit on both sides is not a conflict", "[merge]")
{
    const std::string base = "one\ntwo\nthree\n";
    const std::string edited = "one\nTWO\nthree\n";
    REQUIRE(clean(base, edited, edited) == "one\nTWO\nthree\n");
}

TEST_CASE("overlapping edits conflict", "[merge]")
{
    REQUIRE(conflicts("one\ntwo\nthree\n",
                      "one\nTWO-LOCAL\nthree\n",
                      "one\nTWO-REMOTE\nthree\n"));
}

TEST_CASE("insert only on one side merges cleanly", "[merge]")
{
    const std::string base = "one\ntwo\nthree\n";
    REQUIRE(clean(base, base, "one\ntwo\ntwo-and-a-half\nthree\n") ==
            "one\ntwo\ntwo-and-a-half\nthree\n");
}

TEST_CASE("identical insertion on both sides is not duplicated", "[merge]")
{
    const std::string both = "one\ntwo\nthree\n";
    REQUIRE(clean("one\ntwo\n", both, both) == "one\ntwo\nthree\n");
}

TEST_CASE("differing insertions at the same position conflict", "[merge]")
{
    REQUIRE(conflicts("one\ntwo\n", "one\ntwo\nLOCAL\n", "one\ntwo\nREMOTE\n"));
}

TEST_CASE("identical mid-file insertions are not duplicated", "[merge]")
{
    const std::string both = "one\ntwo\nthree\n";
    REQUIRE(clean("one\nthree\n", both, both) == "one\ntwo\nthree\n");
}

TEST_CASE("a deletion on one side applies cleanly", "[merge]")
{
    const std::string base = "one\ntwo\nthree\n";
    REQUIRE(clean(base, "one\nthree\n", base) == "one\nthree\n");
    REQUIRE(clean(base, base, "one\nthree\n") == "one\nthree\n");
}

TEST_CASE("an unchanged file merges to itself", "[merge]")
{
    const std::string base = "one\ntwo\nthree\n";
    REQUIRE(clean(base, base, base) == base);
}

TEST_CASE("a file with no trailing newline round-trips", "[merge]")
{
    // split_lines keeps terminators attached, so re-joining must be exact.
    const std::string base = "one\ntwo";
    REQUIRE(clean(base, "ONE\ntwo", base) == "ONE\ntwo");
}

// ── The diff underneath it ──

TEST_CASE("split_lines keeps terminators and handles the empty string",
          "[diff]")
{
    REQUIRE(diff::split_lines("").empty());
    const auto lines = diff::split_lines("a\nb\n");
    REQUIRE(lines.size() == 2);
    REQUIRE(lines[0] == "a\n");
    REQUIRE(lines[1] == "b\n");

    const auto ragged = diff::split_lines("a\nb");
    REQUIRE(ragged.size() == 2);
    REQUIRE(ragged[1] == "b");
}

TEST_CASE("hunks address the base text", "[diff]")
{
    const auto base = diff::split_lines("one\ntwo\nthree\n");
    const auto other = diff::split_lines("one\nTWO\nthree\n");

    const auto hunks = diff::hunks_from_diff(base, other);
    REQUIRE(hunks.size() == 1);
    REQUIRE(hunks[0].start == 1);
    REQUIRE(hunks[0].end == 2);
    REQUIRE(hunks[0].lines == std::vector<std::string>{"TWO\n"});
}

TEST_CASE("a pure insertion is a zero-width hunk", "[diff]")
{
    const auto base = diff::split_lines("one\nthree\n");
    const auto other = diff::split_lines("one\ntwo\nthree\n");

    const auto hunks = diff::hunks_from_diff(base, other);
    REQUIRE(hunks.size() == 1);
    REQUIRE(hunks[0].start == 1);
    REQUIRE(hunks[0].end == 1);
    REQUIRE(hunks[0].lines == std::vector<std::string>{"two\n"});
}

TEST_CASE("identical inputs produce no hunks", "[diff]")
{
    const auto lines = diff::split_lines("one\ntwo\n");
    REQUIRE(diff::hunks_from_diff(lines, lines).empty());
}

// ── The guard every work-folder command depends on ──

TEST_CASE("safe_rel_path accepts ordinary relative paths", "[paths]")
{
    REQUIRE(paths::safe_rel_path("notes.mdp") == "notes.mdp");
    REQUIRE(paths::safe_rel_path("world/city.mdp") == "world/city.mdp");
    REQUIRE(paths::safe_rel_path("world\\city.mdp") == "world/city.mdp");
    REQUIRE(paths::safe_rel_path("world//./city.mdp") == "world/city.mdp");
}

TEST_CASE("safe_rel_path refuses anything that could escape the root",
          "[paths]")
{
    REQUIRE_THROWS_AS(paths::safe_rel_path(""), std::invalid_argument);
    REQUIRE_THROWS_AS(paths::safe_rel_path("/etc/passwd"),
                      std::invalid_argument);
    REQUIRE_THROWS_AS(paths::safe_rel_path("\\windows\\system32"),
                      std::invalid_argument);
    REQUIRE_THROWS_AS(paths::safe_rel_path("../secrets.mdp"),
                      std::invalid_argument);
    REQUIRE_THROWS_AS(paths::safe_rel_path("world/../../secrets.mdp"),
                      std::invalid_argument);
    REQUIRE_THROWS_AS(paths::safe_rel_path("C:/Windows/win.ini"),
                      std::invalid_argument);
    REQUIRE_THROWS_AS(paths::safe_rel_path("notes.mdp:hidden"),
                      std::invalid_argument);
}

TEST_CASE("is_work_file recognises exactly the eight app extensions",
          "[workfolder]")
{
    for (const auto ext : workfolder::kWorkFileExtensions)
    {
        REQUIRE(workfolder::is_work_file("doc." + std::string(ext)));
    }
    REQUIRE(workfolder::is_work_file("DOC.MDP"));
    REQUIRE_FALSE(workfolder::is_work_file("notes.txt"));
    REQUIRE_FALSE(workfolder::is_work_file("_lktpl.json"));
}

// ── Updater ──
//
// The signature check is the whole security boundary of the update path: it is
// what stops whoever controls the manifest URL, a CDN, or a hostile network
// from pointing the app at an installer of their choosing.

namespace {

struct SodiumFixture
{
    SodiumFixture()
    {
        util::init_crypto();
    }
};

// A real public key, from `lectern-updater-tool keygen`.
constexpr const char *kPublicKey =
    "75b852a5e20b9a03f8782d082dc908c1e7fe78a7192a40ea9e630bd2e32a1631";

constexpr const char *kPayload = "lectern update manifest";

// Well-formed hex of the right length, but not a signature anybody produced —
// the shape an attacker can trivially supply. It must be rejected.
constexpr const char *kSignature =
    "3b7bdbd8f4c1cf6e1c3e2f6a9c4d8e0b5a7f13d2c6e8091a4b3d5f7092a1c3e5"
    "d4f6081b2c3e5a7f90b1d2e4f6a8c0b2d4e6f8091a3b5c7d9e0f2a4b6c8d0e2f";

}  // namespace

TEST_CASE("version comparison orders releases", "[updater]")
{
    REQUIRE(updater::compare_versions("0.13.0", "0.12.11") > 0);
    REQUIRE(updater::compare_versions("0.12.11", "0.13.0") < 0);
    REQUIRE(updater::compare_versions("0.12.11", "0.12.11") == 0);
    // Shorter versions pad with zeroes rather than comparing as strings,
    // where "0.9" would sort above "0.10".
    REQUIRE(updater::compare_versions("0.10.0", "0.9.0") > 0);
    REQUIRE(updater::compare_versions("1.0", "1.0.0") == 0);
    REQUIRE(updater::compare_versions("2.0.0", "1.99.99") > 0);
    // A pre-release suffix is ignored, not treated as garbage.
    REQUIRE(updater::compare_versions("0.13.0-beta.1", "0.12.11") > 0);
}

TEST_CASE_METHOD(SodiumFixture,
                 "a well-formed but forged signature is rejected",
                 "[updater]")
{
    REQUIRE_FALSE(
        updater::verify_signature(kPayload, kSignature, kPublicKey));
}

TEST_CASE_METHOD(SodiumFixture,
                 "malformed signatures and keys are refused, not crashed on",
                 "[updater]")
{
    // Every one of these is a shape an attacker could supply.
    REQUIRE_FALSE(updater::verify_signature(kPayload, "", kPublicKey));
    REQUIRE_FALSE(updater::verify_signature(kPayload, kSignature, ""));
    REQUIRE_FALSE(updater::verify_signature(kPayload, "zz", kPublicKey));
    REQUIRE_FALSE(updater::verify_signature(kPayload, "abc", kPublicKey));
    REQUIRE_FALSE(
        updater::verify_signature(kPayload, kSignature, "not-hex-at-all"));
    // Right encoding, wrong length.
    REQUIRE_FALSE(updater::verify_signature(kPayload, "aabb", kPublicKey));
}

TEST_CASE_METHOD(SodiumFixture,
                 "a signature verifies only over the exact bytes signed",
                 "[updater]")
{
    // Generate a keypair here so this exercises the real signing path.
    std::array<unsigned char, 32> public_key{};
    std::array<unsigned char, 64> secret_key{};
    REQUIRE(crypto_sign_keypair(public_key.data(), secret_key.data()) == 0);

    const std::string manifest = R"({"version":"0.13.0"})";

    std::array<unsigned char, 64> signature{};
    unsigned long long length = 0;
    REQUIRE(crypto_sign_detached(
                signature.data(),
                &length,
                reinterpret_cast<const unsigned char *>(manifest.data()),
                manifest.size(),
                secret_key.data()) == 0);

    const auto hex = [](const unsigned char *data, size_t size) {
        static const char *digits = "0123456789abcdef";
        std::string out;
        for (size_t i = 0; i < size; ++i)
        {
            out.push_back(digits[data[i] >> 4]);
            out.push_back(digits[data[i] & 0x0f]);
        }
        return out;
    };

    const std::string signature_hex = hex(signature.data(), signature.size());
    const std::string public_key_hex = hex(public_key.data(), public_key.size());

    REQUIRE(updater::verify_signature(manifest, signature_hex, public_key_hex));

    // One byte changed anywhere in the manifest invalidates it.
    std::string tampered = manifest;
    tampered.back() = ' ';
    REQUIRE_FALSE(
        updater::verify_signature(tampered, signature_hex, public_key_hex));

    // A different key does not accept it either.
    std::array<unsigned char, 32> other_public{};
    std::array<unsigned char, 64> other_secret{};
    REQUIRE(crypto_sign_keypair(other_public.data(), other_secret.data()) == 0);
    REQUIRE_FALSE(updater::verify_signature(
        manifest, signature_hex, hex(other_public.data(), other_public.size())));
}

TEST_CASE("the updater is disabled without a compiled-in key", "[updater]")
{
    // A build with no key must never accept a manifest. This is the default
    // for every developer build, and the test documents that.
    if (std::string_view(lectern::kUpdatePublicKey).empty())
    {
        REQUIRE_FALSE(updater::enabled());
    }
}
