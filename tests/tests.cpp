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

std::string fClean(const std::string &base,
                  const std::string &local,
                  const std::string &remote)
{
    const auto result = diff::fThreeWayMerge(base, local, remote);
    REQUIRE(result.clean);
    return result.text;
}

bool fConflicts(const std::string &base,
               const std::string &local,
               const std::string &remote)
{
    return !diff::fThreeWayMerge(base, local, remote).clean;
}

}  // namespace

// ── Three-way merge (ported from src-tauri/src/cloud.rs) ──

TEST_CASE("disjoint edits merge cleanly", "[merge]")
{
    const std::string base = "one\ntwo\nthree\nfour\nfive\n";
    const std::string local = "one\nTWO\nthree\nfour\nfive\n";
    const std::string remote = "one\ntwo\nthree\nfour\nFIVE\n";
    REQUIRE(fClean(base, local, remote) == "one\nTWO\nthree\nfour\nFIVE\n");
}

TEST_CASE("identical edit on both sides is not a conflict", "[merge]")
{
    const std::string base = "one\ntwo\nthree\n";
    const std::string edited = "one\nTWO\nthree\n";
    REQUIRE(fClean(base, edited, edited) == "one\nTWO\nthree\n");
}

TEST_CASE("overlapping edits conflict", "[merge]")
{
    REQUIRE(fConflicts("one\ntwo\nthree\n",
                      "one\nTWO-LOCAL\nthree\n",
                      "one\nTWO-REMOTE\nthree\n"));
}

TEST_CASE("insert only on one side merges cleanly", "[merge]")
{
    const std::string base = "one\ntwo\nthree\n";
    REQUIRE(fClean(base, base, "one\ntwo\ntwo-and-a-half\nthree\n") ==
            "one\ntwo\ntwo-and-a-half\nthree\n");
}

TEST_CASE("identical insertion on both sides is not duplicated", "[merge]")
{
    const std::string both = "one\ntwo\nthree\n";
    REQUIRE(fClean("one\ntwo\n", both, both) == "one\ntwo\nthree\n");
}

TEST_CASE("differing insertions at the same position conflict", "[merge]")
{
    REQUIRE(fConflicts("one\ntwo\n", "one\ntwo\nLOCAL\n", "one\ntwo\nREMOTE\n"));
}

TEST_CASE("identical mid-file insertions are not duplicated", "[merge]")
{
    const std::string both = "one\ntwo\nthree\n";
    REQUIRE(fClean("one\nthree\n", both, both) == "one\ntwo\nthree\n");
}

TEST_CASE("a deletion on one side applies cleanly", "[merge]")
{
    const std::string base = "one\ntwo\nthree\n";
    REQUIRE(fClean(base, "one\nthree\n", base) == "one\nthree\n");
    REQUIRE(fClean(base, base, "one\nthree\n") == "one\nthree\n");
}

TEST_CASE("an unchanged file merges to itself", "[merge]")
{
    const std::string base = "one\ntwo\nthree\n";
    REQUIRE(fClean(base, base, base) == base);
}

TEST_CASE("a file with no trailing newline round-trips", "[merge]")
{
    // fSplitLines keeps terminators attached, so re-joining must be exact.
    const std::string base = "one\ntwo";
    REQUIRE(fClean(base, "ONE\ntwo", base) == "ONE\ntwo");
}

// ── The diff underneath it ──

TEST_CASE("fSplitLines keeps terminators and handles the empty string",
          "[diff]")
{
    REQUIRE(diff::fSplitLines("").empty());
    const auto lines = diff::fSplitLines("a\nb\n");
    REQUIRE(lines.size() == 2);
    REQUIRE(lines[0] == "a\n");
    REQUIRE(lines[1] == "b\n");

    const auto ragged = diff::fSplitLines("a\nb");
    REQUIRE(ragged.size() == 2);
    REQUIRE(ragged[1] == "b");
}

TEST_CASE("hunks address the base text", "[diff]")
{
    const auto base = diff::fSplitLines("one\ntwo\nthree\n");
    const auto other = diff::fSplitLines("one\nTWO\nthree\n");

    const auto hunks = diff::fHunksFromDiff(base, other);
    REQUIRE(hunks.size() == 1);
    REQUIRE(hunks[0].start == 1);
    REQUIRE(hunks[0].end == 2);
    REQUIRE(hunks[0].lines == std::vector<std::string>{"TWO\n"});
}

TEST_CASE("a pure insertion is a zero-width hunk", "[diff]")
{
    const auto base = diff::fSplitLines("one\nthree\n");
    const auto other = diff::fSplitLines("one\ntwo\nthree\n");

    const auto hunks = diff::fHunksFromDiff(base, other);
    REQUIRE(hunks.size() == 1);
    REQUIRE(hunks[0].start == 1);
    REQUIRE(hunks[0].end == 1);
    REQUIRE(hunks[0].lines == std::vector<std::string>{"two\n"});
}

TEST_CASE("identical inputs produce no hunks", "[diff]")
{
    const auto lines = diff::fSplitLines("one\ntwo\n");
    REQUIRE(diff::fHunksFromDiff(lines, lines).empty());
}

// ── The guard every work-folder command depends on ──

TEST_CASE("fSafeRelPath accepts ordinary relative paths", "[paths]")
{
    REQUIRE(paths::fSafeRelPath("notes.mdp") == "notes.mdp");
    REQUIRE(paths::fSafeRelPath("world/city.mdp") == "world/city.mdp");
    REQUIRE(paths::fSafeRelPath("world\\city.mdp") == "world/city.mdp");
    REQUIRE(paths::fSafeRelPath("world//./city.mdp") == "world/city.mdp");
}

TEST_CASE("fSafeRelPath refuses anything that could escape the root",
          "[paths]")
{
    REQUIRE_THROWS_AS(paths::fSafeRelPath(""), std::invalid_argument);
    REQUIRE_THROWS_AS(paths::fSafeRelPath("/etc/passwd"),
                      std::invalid_argument);
    REQUIRE_THROWS_AS(paths::fSafeRelPath("\\windows\\system32"),
                      std::invalid_argument);
    REQUIRE_THROWS_AS(paths::fSafeRelPath("../secrets.mdp"),
                      std::invalid_argument);
    REQUIRE_THROWS_AS(paths::fSafeRelPath("world/../../secrets.mdp"),
                      std::invalid_argument);
    REQUIRE_THROWS_AS(paths::fSafeRelPath("C:/Windows/win.ini"),
                      std::invalid_argument);
    REQUIRE_THROWS_AS(paths::fSafeRelPath("notes.mdp:hidden"),
                      std::invalid_argument);
}

TEST_CASE("fIsWorkFile recognises exactly the nine app extensions",
          "[workfolder]")
{
    for (const auto ext : workfolder::cWorkFileExtensions)
    {
        REQUIRE(workfolder::fIsWorkFile("doc." + std::string(ext)));
    }
    REQUIRE(workfolder::fIsWorkFile("DOC.MDP"));
    REQUIRE_FALSE(workfolder::fIsWorkFile("notes.txt"));
    REQUIRE_FALSE(workfolder::fIsWorkFile("_lktpl.json"));
}

TEST_CASE("fFetchRemote only accepts http and https URLs", "[workfolder]")
{
    REQUIRE(workfolder::fIsFetchableUrl("https://example.com/a.png"));
    REQUIRE(workfolder::fIsFetchableUrl("HTTP://example.com/a.png"));
    REQUIRE_FALSE(workfolder::fIsFetchableUrl("file:///C:/Windows/win.ini"));
    REQUIRE_FALSE(workfolder::fIsFetchableUrl("ftp://example.com/a.png"));
    REQUIRE_FALSE(workfolder::fIsFetchableUrl("https://"));
    REQUIRE_FALSE(workfolder::fIsFetchableUrl("https://exa mple.com/a.png"));
    REQUIRE_FALSE(workfolder::fIsFetchableUrl("javascript:alert(1)"));
    REQUIRE_THROWS(workfolder::fFetchRemote("file:///etc/passwd"));
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
        util::fInitCrypto();
    }
};

// A real public key, from `lectern-updater-tool fKeygen`.
constexpr const char *cPublicKey =
    "75b852a5e20b9a03f8782d082dc908c1e7fe78a7192a40ea9e630bd2e32a1631";

constexpr const char *cPayload = "lectern update manifest";

// Well-formed hex of the right length, but not a signature anybody produced —
// the shape an attacker can trivially supply. It must be rejected.
constexpr const char *cSignature =
    "3b7bdbd8f4c1cf6e1c3e2f6a9c4d8e0b5a7f13d2c6e8091a4b3d5f7092a1c3e5"
    "d4f6081b2c3e5a7f90b1d2e4f6a8c0b2d4e6f8091a3b5c7d9e0f2a4b6c8d0e2f";

}  // namespace

TEST_CASE("version comparison orders releases", "[updater]")
{
    REQUIRE(updater::fCompareVersions("0.13.0", "0.12.11") > 0);
    REQUIRE(updater::fCompareVersions("0.12.11", "0.13.0") < 0);
    REQUIRE(updater::fCompareVersions("0.12.11", "0.12.11") == 0);
    // Shorter versions pad with zeroes rather than comparing as strings,
    // where "0.9" would sort above "0.10".
    REQUIRE(updater::fCompareVersions("0.10.0", "0.9.0") > 0);
    REQUIRE(updater::fCompareVersions("1.0", "1.0.0") == 0);
    REQUIRE(updater::fCompareVersions("2.0.0", "1.99.99") > 0);
    // A pre-release suffix is ignored, not treated as garbage.
    REQUIRE(updater::fCompareVersions("0.13.0-beta.1", "0.12.11") > 0);
}

TEST_CASE_METHOD(SodiumFixture,
                 "a well-formed but forged signature is rejected",
                 "[updater]")
{
    REQUIRE_FALSE(
        updater::fVerifySignature(cPayload, cSignature, cPublicKey));
}

TEST_CASE_METHOD(SodiumFixture,
                 "malformed signatures and keys are refused, not crashed on",
                 "[updater]")
{
    // Every one of these is a shape an attacker could supply.
    REQUIRE_FALSE(updater::fVerifySignature(cPayload, "", cPublicKey));
    REQUIRE_FALSE(updater::fVerifySignature(cPayload, cSignature, ""));
    REQUIRE_FALSE(updater::fVerifySignature(cPayload, "zz", cPublicKey));
    REQUIRE_FALSE(updater::fVerifySignature(cPayload, "abc", cPublicKey));
    REQUIRE_FALSE(
        updater::fVerifySignature(cPayload, cSignature, "not-hex-at-all"));
    // Right encoding, wrong length.
    REQUIRE_FALSE(updater::fVerifySignature(cPayload, "aabb", cPublicKey));
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

    const auto fHex = [](const unsigned char *data, size_t size) {
        static const char *digits = "0123456789abcdef";
        std::string out;
        for (size_t i = 0; i < size; ++i)
        {
            out.push_back(digits[data[i] >> 4]);
            out.push_back(digits[data[i] & 0x0f]);
        }
        return out;
    };

    const std::string signature_hex = fHex(signature.data(), signature.size());
    const std::string public_key_hex = fHex(public_key.data(), public_key.size());

    REQUIRE(updater::fVerifySignature(manifest, signature_hex, public_key_hex));

    // One byte changed anywhere in the manifest invalidates it.
    std::string tampered = manifest;
    tampered.back() = ' ';
    REQUIRE_FALSE(
        updater::fVerifySignature(tampered, signature_hex, public_key_hex));

    // A different key does not accept it either.
    std::array<unsigned char, 32> other_public{};
    std::array<unsigned char, 64> other_secret{};
    REQUIRE(crypto_sign_keypair(other_public.data(), other_secret.data()) == 0);
    REQUIRE_FALSE(updater::fVerifySignature(
        manifest, signature_hex, fHex(other_public.data(), other_public.size())));
}

TEST_CASE("the updater is disabled without a compiled-in key", "[updater]")
{
    // A build with no key must never accept a manifest. This is the default
    // for every developer build, and the test documents that.
    if (std::string_view(lectern::cUpdatePublicKey).empty())
    {
        REQUIRE_FALSE(updater::fEnabled());
    }
}
