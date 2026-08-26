// The JavaScript ↔ C++ bridge serializer.
//
// saucer ships a glaze-backed serializer, but saucer 6.0.1's `glaze.inl`
// spells its concepts `glz::write_supported<Format, T>` while glaze ≥ 5.1
// declares them `write_supported<T, Format>` — so every `expose()` fails with
// "T should be serializable" no matter what type it returns. That is a
// version skew between two vcpkg ports, not something this app can configure
// away.
//
// Rather than pin an old glaze, this plugs a serializer of our own into
// saucer's `generic::serializer`, which is the documented extension point.
// It uses nlohmann::json — already a dependency, and already the type every
// command in main.cpp builds its answer with — so the JSON never has to make
// a round trip through a second library.
//
// The wire format is saucer's, unchanged: a call arrives as
// `{"saucer:call": …, "id": N, "name": "…", "params": "<json array as a
// string>"}` and a promise result as `{"saucer:resolve": …, "id": N,
// "result": "<json as a string>"}`.
#pragma once

#include <saucer/serializers/generic/generic.hpp>

#include <nlohmann/json.hpp>

#include <expected>
#include <memory>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>

namespace lectern::bridge {

/// A JSON document that is already serialised. Returning one hands the text to
/// the webview verbatim, so a command's answer arrives in JavaScript as a real
/// object or array rather than a quoted string.
struct raw_json
{
    std::string text;

    raw_json() = default;

    explicit raw_json(std::string value) : text(std::move(value)) {}

    explicit raw_json(const nlohmann::json &value) : text(value.dump()) {}
};

struct function_data : saucer::function_data
{
    /// The arguments array, as JSON text.
    std::string params;
};

struct result_data : saucer::result_data
{
    /// The resolved value, as JSON text.
    std::string result;
};

namespace detail {

template <typename T>
struct is_tuple : std::false_type
{
};

template <typename... Ts>
struct is_tuple<std::tuple<Ts...>> : std::true_type
{
};

template <typename T>
std::expected<T, std::string> convert(const nlohmann::json &value);

/// Fills one tuple element, recording the first failure.
template <size_t I, typename T>
bool assign_element(T &out, const nlohmann::json &array, std::string &error)
{
    using Element = std::tuple_element_t<I, T>;
    auto converted = convert<Element>(array[I]);
    if (!converted)
    {
        error = "parameter " + std::to_string(I) + ": " + converted.error();
        return false;
    }
    std::get<I>(out) = std::move(*converted);
    return true;
}

template <typename T>
std::expected<T, std::string> convert(const nlohmann::json &value)
{
    if constexpr (is_tuple<T>::value)
    {
        constexpr size_t arity = std::tuple_size_v<T>;
        if (!value.is_array())
        {
            return std::unexpected<std::string>("expected an argument array");
        }
        if (value.size() != arity)
        {
            return std::unexpected<std::string>(
                "expected " + std::to_string(arity) + " argument(s), got " +
                std::to_string(value.size()));
        }

        T out{};
        std::string error;
        [&]<size_t... I>(std::index_sequence<I...>) {
            // Short-circuits on the first element that doesn't convert.
            (void)(assign_element<I>(out, value, error) && ...);
        }(std::make_index_sequence<arity>{});

        if (!error.empty())
        {
            return std::unexpected(error);
        }
        return out;
    }
    else
    {
        // An argument the caller omitted arrives as JSON null. Reading it as
        // the type's empty value keeps the page-side shim dumb and matches
        // what serde's `#[serde(default)]` did on the Rust side.
        if (value.is_null())
        {
            return T{};
        }
        try
        {
            return value.get<T>();
        }
        catch (const std::exception &)
        {
            return std::unexpected<std::string>("value has the wrong type");
        }
    }
}

}  // namespace detail

class interface
{
    template <typename T>
    using result = std::expected<T, std::string>;

  public:
    template <typename T>
    static result<T> parse(const std::string &data)
    {
        auto parsed = nlohmann::json::parse(data, nullptr, false);
        if (parsed.is_discarded())
        {
            return std::unexpected<std::string>("malformed JSON payload");
        }
        return detail::convert<T>(parsed);
    }

    template <typename T>
    static result<T> parse(const result_data &data)
    {
        return parse<T>(data.result);
    }

    template <typename T>
    static result<T> parse(const function_data &data)
    {
        return parse<T>(data.params);
    }

    template <typename T>
    static std::string serialize(T &&value)
    {
        using Decayed = std::decay_t<T>;
        if constexpr (std::is_same_v<Decayed, raw_json>)
        {
            return value.text.empty() ? "null" : value.text;
        }
        else
        {
            try
            {
                return nlohmann::json(std::forward<T>(value)).dump();
            }
            catch (const std::exception &)
            {
                return "null";
            }
        }
    }
};

struct serializer
    : saucer::serializers::generic::serializer<function_data,
                                               result_data,
                                               interface>
{
    ~serializer() override;

    [[nodiscard]] std::string script() const override;
    [[nodiscard]] std::string js_serializer() const override;
    [[nodiscard]] parse_result parse(const std::string &data) const override;
};

}  // namespace lectern::bridge
