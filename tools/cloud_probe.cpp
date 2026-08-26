// Connectivity probe for cloud sync.
//
// "It won't connect" is hard to diagnose from inside the app: the failure
// surfaces as one alert() in the webview, with no way to see which request
// failed or what the server actually said. This runs the exact same code path
// the `cloud_connect` command runs — same HTTP client, same endpoints, same
// order — and prints each step.
//
// Usage:
//   lectern-cloud-probe <server-url> <email> <password> [work-folder]
//
// The work folder defaults to a throwaway temp directory, so probing does not
// touch a real workspace. Pass one explicitly to reproduce a sync failure
// against actual files.
#include <cpr/cpr.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <iostream>
#include <string>

#include "cloud.h"
#include "paths.h"
#include "util.h"

namespace fs = std::filesystem;
using json = nlohmann::json;
using namespace lectern;

namespace {

void step(const std::string &what)
{
    std::cout << "\n== " << what << "\n";
}

/// Prints enough of a response to tell a wrong URL from a wrong password from
/// a server that isn't there at all.
void report(const cpr::Response &response)
{
    std::cout << "   status : " << response.status_code << "\n";
    if (response.error.code != cpr::ErrorCode::OK)
    {
        std::cout << "   error  : " << response.error.message << "\n";
    }
    if (!response.text.empty())
    {
        std::cout << "   body   : " << response.text.substr(0, 400) << "\n";
    }
}

}  // namespace

int main(int argc, char *argv[])
{
    if (argc < 4)
    {
        std::cerr << "usage: lectern-cloud-probe <server-url> <email> "
                     "<password> [work-folder]\n";
        return 2;
    }

    // Unbuffered: if a later step crashes or hangs, the output up to that
    // point is what tells you where it got to.
    std::cout << std::unitbuf;

    util::init_crypto();

    std::string server = argv[1];
    while (!server.empty() && server.back() == '/')
    {
        server.pop_back();
    }
    const std::string email = argv[2];
    const std::string password = argv[3];

    std::error_code ec;
    fs::path root = argc > 4
                        ? fs::path(argv[4])
                        : fs::temp_directory_path(ec) /
                              ("lectern-probe-" + util::random_hex(6));
    fs::create_directories(root, ec);

    std::cout << "server      : " << server << "\n"
              << "email       : " << email << "\n"
              << "work folder : " << root.string() << "\n";

    // 1. Is anything listening, and is it the frontend or the API?
    step("GET / (is the server reachable?)");
    const auto root_response =
        cpr::Get(cpr::Url{server + "/"}, cpr::Timeout{10000});
    report(root_response);
    if (root_response.error.code != cpr::ErrorCode::OK)
    {
        std::cout << "\nThe server could not be reached at all. Check the URL, "
                     "the port, and whether the server is bound to localhost "
                     "only.\n";
        return 1;
    }

    // 2. The call cloud::connect makes first.
    step("POST /api/auth/login");
    const auto login = cpr::Post(
        cpr::Url{server + "/api/auth/login"},
        cpr::Header{{"Content-Type", "application/json"}},
        cpr::Body{json{{"email", email}, {"password", password}}.dump()},
        cpr::Timeout{30000});
    report(login);

    if (login.status_code < 200 || login.status_code >= 300)
    {
        std::cout << "\nLogin failed. 401 means wrong credentials; 404 means "
                     "this is not a Lectern server (or is behind a path "
                     "prefix); 429 means the rate limiter is holding you off.\n";
        return 1;
    }

    const auto user = json::parse(login.text, nullptr, false);
    const std::string token =
        user.is_object() ? user.value("apiToken", "") : "";
    std::cout << "   apiToken: " << (token.empty() ? "(missing!)" : "present")
              << "\n";
    if (token.empty())
    {
        std::cout << "\nThe server accepted the login but returned no "
                     "apiToken, so every later request would be "
                     "unauthenticated.\n";
        return 1;
    }

    // 3. The first authenticated call the sync engine makes.
    step("GET /api/workspace (bearer auth)");
    const auto list = cpr::Get(cpr::Url{server + "/api/workspace"},
                               cpr::Header{{"Authorization", "Bearer " + token}},
                               cpr::Timeout{30000});
    report(list);

    // 4. Optional endpoint; an older server returns 404 and sync still works.
    step("GET /api/workspace/deleted (tombstones, optional)");
    report(cpr::Get(cpr::Url{server + "/api/workspace/deleted"},
                    cpr::Header{{"Authorization", "Bearer " + token}},
                    cpr::Timeout{30000}));

    // 5. The whole thing, through the real engine.
    step("cloud::connect (full path, including the first sync)");
    try
    {
        paths::load_authorized_roots();
        paths::authorize_root(root);
        const json status = cloud::connect(server, email, password,
                                           root.string());
        std::cout << "   " << status.dump(2) << "\n\nConnected.\n";
        cloud::disconnect();
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cout << "   FAILED: " << error.what() << "\n";
        return 1;
    }
}
