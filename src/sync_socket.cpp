#include "sync_socket.h"

#include <curl/curl.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <poll.h>
#endif

using namespace std::chrono_literals;

namespace lectern::cloud {

namespace {

constexpr auto cFirstRetry = 1s;
constexpr auto cLongestRetry = 30s;
/// Cadence of the heartbeat that notices a connection that died silently
/// (a pulled cable, a NAT that forgot us). It sends one tiny frame; it does not
/// look at any files.
constexpr auto cHeartbeat = 25s;
/// How often a blocked wait gives up to look at deadlines and stop requests.
/// Purely local: nothing is sent.
constexpr int cWatchdogMs = 1000;

bool fWaitReadable(curl_socket_t socket, int timeout_ms)
{
#ifdef _WIN32
    WSAPOLLFD fd{};
    fd.fd = socket;
    fd.events = POLLRDNORM;
    return WSAPoll(&fd, 1, timeout_ms) > 0;
#else
    pollfd fd{};
    fd.fd = socket;
    fd.events = POLLIN;
    return ::poll(&fd, 1, timeout_ms) > 0;
#endif
}

/// Sends one text frame. Frames here are a few dozen bytes, so a partial send
/// is treated as a broken connection rather than resumed.
void fSendText(CURL *curl, const std::string &text)
{
    size_t sent = 0;
    for (int attempt = 0; attempt < 50; ++attempt)
    {
        const CURLcode code =
            curl_ws_send(curl, text.data(), text.size(), &sent, 0, CURLWS_TEXT);
        if (code == CURLE_OK)
        {
            if (sent != text.size())
            {
                throw std::runtime_error("short WebSocket send");
            }
            return;
        }
        if (code != CURLE_AGAIN)
        {
            throw std::runtime_error(curl_easy_strerror(code));
        }
        std::this_thread::sleep_for(100ms);
    }
    throw std::runtime_error("WebSocket send would block");
}

}  // namespace

std::string fSyncSocketUrl(const std::string &server_url)
{
    std::string base = server_url;
    while (!base.empty() && base.back() == '/')
    {
        base.pop_back();
    }

    const auto starts_with = [&](const char *prefix) {
        return base.rfind(prefix, 0) == 0;
    };
    if (starts_with("https://"))
    {
        return "wss://" + base.substr(8) + "/api/sync/ws";
    }
    if (starts_with("http://"))
    {
        return "ws://" + base.substr(7) + "/api/sync/ws";
    }
    return {};
}

SyncSocket::SyncSocket(Hooks hooks) : hooks_(std::move(hooks)) {}

SyncSocket::~SyncSocket()
{
    fStop();
}

bool SyncSocket::fStopping() const
{
    std::lock_guard<std::mutex> lock(state_mutex_);
    return stop_;
}

void SyncSocket::fStart()
{
    std::lock_guard<std::mutex> lifecycle(lifecycle_mutex_);

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (running_)
        {
            // Already connected (or retrying): make it reconnect so it picks up
            // the settings as they are now, e.g. after logging in again.
            reconnect_ = true;
            state_cv_.notify_all();
            return;
        }
        running_ = true;
        stop_ = false;
        reconnect_ = false;
        reconcile_requested_ = false;
        reconcile_everything_ = false;
        reconcile_paths_.clear();
    }

    // A previous run that ended by itself (the user disconnected) leaves its
    // threads to be joined before new ones can be started.
    if (socket_thread_.joinable())
    {
        socket_thread_.join();
    }
    if (worker_thread_.joinable())
    {
        worker_thread_.join();
    }

    socket_thread_ = std::thread([this] { fSocketThread(); });
    worker_thread_ = std::thread([this] { fWorkerThread(); });
}

void SyncSocket::fStop()
{
    std::lock_guard<std::mutex> lifecycle(lifecycle_mutex_);

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        stop_ = true;
        state_cv_.notify_all();
    }
    if (socket_thread_.joinable())
    {
        socket_thread_.join();
    }
    if (worker_thread_.joinable())
    {
        worker_thread_.join();
    }

    std::lock_guard<std::mutex> lock(state_mutex_);
    running_ = false;
    stop_ = false;
}

void SyncSocket::fPause(std::chrono::milliseconds duration)
{
    std::unique_lock<std::mutex> lock(state_mutex_);
    state_cv_.wait_for(lock, duration, [this] { return stop_ || reconnect_; });
}

void SyncSocket::fRequestReconcile(const SyncProtocol::Reaction &reaction)
{
    std::lock_guard<std::mutex> lock(state_mutex_);
    reconcile_requested_ = true;
    reconcile_everything_ = reconcile_everything_ || reaction.everything;
    reconcile_paths_.insert(reaction.paths.begin(), reaction.paths.end());
    state_cv_.notify_all();
}

bool SyncSocket::fLive() const
{
    std::lock_guard<std::mutex> lock(io_mutex_);
    return curl_ != nullptr && protocol_ && protocol_->fReady();
}

void SyncSocket::fAnnounceLocalChange(const std::vector<std::string> &paths)
{
    // Called from the UI thread after a save, so it never waits on the network:
    // the socket is non-blocking and the frame is tiny. If the server isn't
    // there to answer, nothing is pushed.
    std::lock_guard<std::mutex> lock(io_mutex_);
    if (curl_ == nullptr || !protocol_)
    {
        return;
    }

    const auto frame = protocol_->fAnnounce(paths);
    if (!frame)
    {
        return;
    }
    try
    {
        fSendText(curl_, *frame);
    }
    catch (const std::exception &error)
    {
        // The deadline on the announcement will drop the connection.
        spdlog::debug("sync: could not announce a change: {}", error.what());
    }
}

void SyncSocket::fWorkerThread()
{
    while (true)
    {
        std::vector<std::string> paths;
        {
            std::unique_lock<std::mutex> lock(state_mutex_);
            state_cv_.wait(lock, [this] { return stop_ || !running_ || reconcile_requested_; });
            if (stop_ || !running_)
            {
                return;
            }
            // Requests that arrive while a pass runs are served by the next
            // loop: one pass covers any number of them, and a request for
            // everything swallows the lists of files.
            reconcile_requested_ = false;
            if (!reconcile_everything_)
            {
                paths.assign(reconcile_paths_.begin(), reconcile_paths_.end());
            }
            reconcile_everything_ = false;
            reconcile_paths_.clear();
        }

        try
        {
            hooks_.reconcile(paths);
        }
        catch (const std::exception &error)
        {
            spdlog::debug("sync pass failed: {}", error.what());
        }
    }
}

void SyncSocket::fSocketThread()
{
    auto retry = cFirstRetry;

    while (!fStopping())
    {
        const auto endpoint = hooks_.endpoint();
        if (!endpoint)
        {
            break;  // disconnected
        }

        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            reconnect_ = false;
        }

        if (fRunSession(*endpoint))
        {
            retry = cFirstRetry;
        }
        else
        {
            retry = std::min<std::chrono::seconds>(retry * 2, cLongestRetry);
        }
        fPause(std::chrono::duration_cast<std::chrono::milliseconds>(retry));
    }

    // Leaving for any reason (stopped, or the user disconnected) ends the
    // worker too, and lets a later fStart() begin afresh.
    std::lock_guard<std::mutex> lock(state_mutex_);
    running_ = false;
    state_cv_.notify_all();
}

bool SyncSocket::fRunSession(const SyncEndpoint &endpoint)
{
    const std::string url = fSyncSocketUrl(endpoint.server_url);
    if (url.empty())
    {
        return false;
    }

    CURL *curl = curl_easy_init();
    if (curl == nullptr)
    {
        return false;
    }

    curl_slist *headers = nullptr;
    headers = curl_slist_append(headers, ("Authorization: Bearer " + endpoint.api_token).c_str());
    headers = curl_slist_append(headers, ("X-Lectern-Device: " + endpoint.device_id).c_str());

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_CONNECT_ONLY, 2L);  // WebSocket: handshake, then raw frames
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);

    bool established = false;
    const auto cleanup = [&] {
        {
            std::lock_guard<std::mutex> lock(io_mutex_);
            if (protocol_)
            {
                protocol_->fOnDisconnected();
            }
            curl_ = nullptr;
        }
        curl_easy_cleanup(curl);
        curl_slist_free_all(headers);
    };

    const CURLcode handshake = curl_easy_perform(curl);
    if (handshake != CURLE_OK)
    {
        spdlog::debug("sync: could not connect to {}: {}", url, curl_easy_strerror(handshake));
        cleanup();
        return false;
    }

    curl_socket_t socket = CURL_SOCKET_BAD;
    if (curl_easy_getinfo(curl, CURLINFO_ACTIVESOCKET, &socket) != CURLE_OK ||
        socket == CURL_SOCKET_BAD)
    {
        cleanup();
        return false;
    }

    try
    {
        {
            std::lock_guard<std::mutex> lock(io_mutex_);
            protocol_ = std::make_unique<SyncProtocol>(endpoint.device_id);
            curl_ = curl;
            fSendText(curl, protocol_->fOnConnected());
        }
        established = true;
        spdlog::debug("sync: connected to {}", url);

        std::string message;
        std::vector<char> buffer(64 * 1024);
        auto last_heartbeat = std::chrono::steady_clock::now();
        bool open = true;

        while (open && !fStopping())
        {
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                if (reconnect_)
                {
                    break;
                }
            }

            // Drain whatever has arrived; the waiting below is only for more.
            while (open)
            {
                std::lock_guard<std::mutex> lock(io_mutex_);
                size_t received = 0;
                const curl_ws_frame *meta = nullptr;
                const CURLcode code =
                    curl_ws_recv(curl, buffer.data(), buffer.size(), &received, &meta);

                if (code == CURLE_AGAIN)
                {
                    break;
                }
                if (code != CURLE_OK || meta == nullptr || (meta->flags & CURLWS_CLOSE))
                {
                    open = false;
                    break;
                }
                if (!(meta->flags & (CURLWS_TEXT | CURLWS_CONT)))
                {
                    continue;  // binary or control frames mean nothing here
                }

                message.append(buffer.data(), received);
                if (meta->bytesleft > 0 || (meta->flags & CURLWS_CONT))
                {
                    continue;  // the rest of this message is still coming
                }

                const auto reaction = protocol_->fOnFrame(message);
                message.clear();
                for (const auto &frame : reaction.send)
                {
                    fSendText(curl, frame);
                }
                if (reaction.reconcile)
                {
                    fRequestReconcile(reaction);
                }
            }

            if (!open)
            {
                break;
            }

            {
                std::lock_guard<std::mutex> lock(io_mutex_);
                if (protocol_->fExpired())
                {
                    spdlog::debug("sync: the server stopped answering; reconnecting");
                    break;
                }
                if (std::chrono::steady_clock::now() - last_heartbeat >= cHeartbeat)
                {
                    fSendText(curl, protocol_->fPing());
                    last_heartbeat = std::chrono::steady_clock::now();
                }
            }

            fWaitReadable(socket, cWatchdogMs);
        }
    }
    catch (const std::exception &error)
    {
        spdlog::debug("sync: connection lost: {}", error.what());
    }

    cleanup();
    return established;
}

}  // namespace lectern::cloud
