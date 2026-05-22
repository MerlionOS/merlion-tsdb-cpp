#pragma once

#include <expected>
#include <memory>
#include <regex>
#include <string>
#include <string_view>
#include <system_error>

// Label matcher — the predicate type used by querier::select to filter
// series during posting-list intersection and chunk decoding. Mirrors
// upstream Prometheus' `labels.Matcher`:
//
//   - Eq:  name == value
//   - Neq: name != value
//   - Re:  name matches regex(value)        — fully anchored
//   - Nre: name does NOT match regex(value) — fully anchored
//
// Empty-value semantics follow Prometheus: a series without label N is
// treated as if it had N="". So Eq("foo", "") matches series WITHOUT
// label `foo`, and Neq("foo", "") matches series that DO have a
// non-empty `foo`.
//
// Re/Nre patterns are fully anchored — upstream PromQL wraps user input
// in `^(...)$`. We do the same so matcher semantics are independent of
// whether the caller anchored the pattern themselves.
namespace merlion_tsdb::model {

enum class MatchType : std::uint8_t {
    Eq,
    Neq,
    Re,
    Nre,
};

class Matcher {
public:
    // Literal matchers — always succeed.
    [[nodiscard]] static Matcher equal(std::string name, std::string value);
    [[nodiscard]] static Matcher not_equal(std::string name, std::string value);

    // Regex matchers — compile-time may fail; pattern is fully anchored
    // by wrapping in `^(...)$` before compilation.
    [[nodiscard]] static std::expected<Matcher, std::error_code>
    regex(std::string name, std::string pattern);
    [[nodiscard]] static std::expected<Matcher, std::error_code>
    not_regex(std::string name, std::string pattern);

    [[nodiscard]] MatchType         type()  const noexcept { return type_; }
    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    [[nodiscard]] const std::string& value() const noexcept { return value_; }

    // True iff `label_value` satisfies this matcher's predicate. For
    // Re/Nre this evaluates the precompiled regex. Eq("", "") matches
    // empty strings; Neq("", "x") matches anything that isn't "x".
    [[nodiscard]] bool matches(std::string_view label_value) const;

    // True iff `label_value` matches the underlying anchored regex,
    // ignoring the Re/Nre polarity. Only meaningful for regex matchers
    // (returns false for Eq/Neq). Used by the block querier when
    // walking candidate values for Nre — there, we need to know which
    // values the regex DOES match so we can take the complement.
    [[nodiscard]] bool regex_matches(std::string_view label_value) const;

private:
    Matcher(MatchType type, std::string name, std::string value);
    Matcher(MatchType type, std::string name, std::string value,
            std::shared_ptr<std::regex> compiled);

    MatchType                    type_;
    std::string                  name_;
    std::string                  value_;
    // shared_ptr so Matcher remains copyable; std::regex is heavy and
    // we don't want each copy to recompile.
    std::shared_ptr<std::regex>  compiled_;
};

}  // namespace merlion_tsdb::model
