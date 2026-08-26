// Release-signing helper for the updater.
//
// The updater verifies an Ed25519 signature over the release manifest with a
// public key compiled into the app (see src/updater.cpp). This produces the
// other half: a keypair, the signature, and the SHA-256 digests the manifest
// carries. It is a build tool, not part of the app.
//
// Usage:
//   lectern-updater-tool keygen
//       Prints a new keypair. The public key goes into the build as
//       -DLECTERN_UPDATE_PUBLIC_KEY=…; the secret key goes into a CI secret
//       and nowhere else.
//
//   lectern-updater-tool sign <file> <secret-key-hex>
//       Prints the detached signature of <file> as hex. Publish it next to the
//       manifest as <manifest>.sig.
//
//   lectern-updater-tool verify <file> <signature-hex> <public-key-hex>
//       Exits 0 when the signature is good. What the app does before trusting
//       a manifest.
//
//   lectern-updater-tool sha256 <file>
//       Prints the file's SHA-256 as hex, for the manifest's per-platform
//       entries.
//
// The secret key is read as an argument for CI convenience, which means it can
// land in a process listing. Prefer passing it as @<path> to read it from a
// file instead.
#include <sodium.h>

#include <array>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

#include "util.h"

namespace {

using namespace lectern;

int usage()
{
    std::cerr
        << "usage:\n"
           "  lectern-updater-tool keygen\n"
           "  lectern-updater-tool sign   <file> <secret-key-hex|@keyfile>\n"
           "  lectern-updater-tool verify <file> <signature-hex> "
           "<public-key-hex>\n"
           "  lectern-updater-tool sha256 <file>\n";
    return 2;
}

std::string to_hex(const unsigned char *data, size_t length)
{
    static const char *digits = "0123456789abcdef";
    std::string out;
    out.reserve(length * 2);
    for (size_t i = 0; i < length; ++i)
    {
        out.push_back(digits[data[i] >> 4]);
        out.push_back(digits[data[i] & 0x0f]);
    }
    return out;
}

/// `@path` reads the value from a file; anything else is the value itself.
std::string resolve_argument(const std::string &value)
{
    if (value.size() > 1 && value.front() == '@')
    {
        const auto contents = util::read_file(value.substr(1));
        if (!contents)
        {
            std::cerr << "cannot read " << value.substr(1) << "\n";
            std::exit(1);
        }
        return util::trim(*contents);
    }
    return util::trim(value);
}

int keygen()
{
    std::array<unsigned char, crypto_sign_PUBLICKEYBYTES> public_key{};
    std::array<unsigned char, crypto_sign_SECRETKEYBYTES> secret_key{};

    if (crypto_sign_keypair(public_key.data(), secret_key.data()) != 0)
    {
        std::cerr << "key generation failed\n";
        return 1;
    }

    std::cout << "public key (build with "
                 "-DLECTERN_UPDATE_PUBLIC_KEY=<this>):\n"
              << to_hex(public_key.data(), public_key.size()) << "\n\n"
              << "secret key (store as a CI secret; it signs every release):\n"
              << to_hex(secret_key.data(), secret_key.size()) << "\n";
    return 0;
}

int sign(const std::string &path, const std::string &secret_key_argument)
{
    const auto payload = util::read_file(path);
    if (!payload)
    {
        std::cerr << "cannot read " << path << "\n";
        return 1;
    }

    const auto secret_key = util::from_hex(resolve_argument(secret_key_argument));
    if (!secret_key || secret_key->size() != crypto_sign_SECRETKEYBYTES)
    {
        std::cerr << "secret key must be " << crypto_sign_SECRETKEYBYTES * 2
                  << " hex characters\n";
        return 1;
    }

    std::array<unsigned char, crypto_sign_BYTES> signature{};
    unsigned long long signature_length = 0;
    if (crypto_sign_detached(
            signature.data(),
            &signature_length,
            reinterpret_cast<const unsigned char *>(payload->data()),
            payload->size(),
            secret_key->data()) != 0)
    {
        std::cerr << "signing failed\n";
        return 1;
    }

    std::cout << to_hex(signature.data(), signature.size()) << "\n";
    return 0;
}

int verify(const std::string &path,
           const std::string &signature_argument,
           const std::string &public_key_argument)
{
    const auto payload = util::read_file(path);
    if (!payload)
    {
        std::cerr << "cannot read " << path << "\n";
        return 1;
    }

    const auto signature = util::from_hex(resolve_argument(signature_argument));
    const auto public_key =
        util::from_hex(resolve_argument(public_key_argument));

    if (!signature || signature->size() != crypto_sign_BYTES ||
        !public_key || public_key->size() != crypto_sign_PUBLICKEYBYTES)
    {
        std::cerr << "bad signature or key encoding\n";
        return 1;
    }

    if (crypto_sign_verify_detached(
            signature->data(),
            reinterpret_cast<const unsigned char *>(payload->data()),
            payload->size(),
            public_key->data()) != 0)
    {
        std::cerr << "SIGNATURE DOES NOT VERIFY\n";
        return 1;
    }

    std::cout << "ok\n";
    return 0;
}

int sha256(const std::string &path)
{
    const std::string digest = util::sha256_file(path);
    if (digest.empty())
    {
        std::cerr << "cannot read " << path << "\n";
        return 1;
    }
    std::cout << digest << "\n";
    return 0;
}

}  // namespace

int main(int argc, char *argv[])
{
    util::init_crypto();

    if (argc < 2)
    {
        return usage();
    }

    const std::string command = argv[1];

    if (command == "keygen" && argc == 2)
    {
        return keygen();
    }
    if (command == "sign" && argc == 4)
    {
        return sign(argv[2], argv[3]);
    }
    if (command == "verify" && argc == 5)
    {
        return verify(argv[2], argv[3], argv[4]);
    }
    if (command == "sha256" && argc == 3)
    {
        return sha256(argv[2]);
    }

    return usage();
}
