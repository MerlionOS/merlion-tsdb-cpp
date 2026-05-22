#include "merlion_tsdb/model/matcher.hpp"

namespace merlion_tsdb::model {

Matcher::Matcher(MatchType type, std::string name, std::string value)
    : type_(type), name_(std::move(name)), value_(std::move(value)) {}

Matcher::Matcher(MatchType type, std::string name, std::string value,
                 std::shared_ptr<std::regex> compiled)
    : type_(type),
      name_(std::move(name)),
      value_(std::move(value)),
      compiled_(std::move(compiled)) {}

Matcher Matcher::equal(std::string name, std::string value) {
    return Matcher{MatchType::Eq, std::move(name), std::move(value)};
}

Matcher Matcher::not_equal(std::string name, std::string value) {
    return Matcher{MatchType::Neq, std::move(name), std::move(value)};
}

namespace {

std::expected<std::shared_ptr<std::regex>, std::error_code>
compile_anchored(const std::string& pattern) {
    // Upstream Prometheus wraps user patterns in `^(?:...)$`. We use
    // `^(...)$` instead — std::regex (ECMAScript flavor) accepts the
    // non-capturing form too but the capturing form is more portable.
    try {
        return std::make_shared<std::regex>(
            "^(" + pattern + ")$",
            std::regex::ECMAScript | std::regex::optimize);
    } catch (const std::regex_error&) {
        return std::unexpected(std::make_error_code(std::errc::invalid_argument));
    }
}

}  // namespace

std::expected<Matcher, std::error_code>
Matcher::regex(std::string name, std::string pattern) {
    auto re = compile_anchored(pattern);
    if (!re) return std::unexpected(re.error());
    return Matcher{MatchType::Re, std::move(name), std::move(pattern), std::move(*re)};
}

std::expected<Matcher, std::error_code>
Matcher::not_regex(std::string name, std::string pattern) {
    auto re = compile_anchored(pattern);
    if (!re) return std::unexpected(re.error());
    return Matcher{MatchType::Nre, std::move(name), std::move(pattern), std::move(*re)};
}

bool Matcher::regex_matches(std::string_view label_value) const {
    if (!compiled_) return false;
    return std::regex_match(label_value.begin(), label_value.end(), *compiled_);
}

bool Matcher::matches(std::string_view label_value) const {
    switch (type_) {
        case MatchType::Eq:  return label_value == value_;
        case MatchType::Neq: return label_value != value_;
        case MatchType::Re:
            return compiled_ &&
                   std::regex_match(label_value.begin(), label_value.end(), *compiled_);
        case MatchType::Nre:
            return compiled_ &&
                   !std::regex_match(label_value.begin(), label_value.end(), *compiled_);
    }
    return false;
}

}  // namespace merlion_tsdb::model
