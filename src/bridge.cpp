#include "bridge.h"

namespace lectern::bridge {

serializer::~serializer() = default;

std::string serializer::script() const
{
    // Nothing to inject: saucer's own bootstrap already defines the call
    // protocol, and JSON.stringify below is all this serializer needs from
    // the page side.
    return {};
}

std::string serializer::js_serializer() const
{
    return "JSON.stringify";
}

serializer::parse_result serializer::parse(const std::string &data) const
{
    const auto message = nlohmann::json::parse(data, nullptr, false);
    if (message.is_discarded() || !message.is_object())
    {
        return std::monostate{};
    }

    if (message.contains("saucer:call"))
    {
        auto call = std::make_unique<function_data>();
        call->id = message.value("id", std::uint64_t{0});
        call->name = message.value("name", std::string{});
        // The page stringifies the whole envelope, so `params` is a nested
        // JSON array here — not a string. Re-dumping it gives the argument
        // text `interface::parse` expects.
        call->params =
            message.contains("params") ? message["params"].dump() : "[]";
        return call;
    }

    if (message.contains("saucer:resolve"))
    {
        auto resolved = std::make_unique<result_data>();
        resolved->id = message.value("id", std::uint64_t{0});
        resolved->result =
            message.contains("result") ? message["result"].dump() : "null";
        return resolved;
    }

    return std::monostate{};
}

}  // namespace lectern::bridge
