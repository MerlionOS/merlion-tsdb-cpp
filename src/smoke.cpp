// Smoke test for the toolchain: exercises C++23 library features that we
// rely on from Homebrew LLVM's libc++. If this compiles and runs, the
// toolchain swap worked.

#include <expected>
#include <flat_map>
#include <print>
#include <ranges>
#include <string>
#include <vector>

namespace {

enum class ParseError { Empty, NotANumber };

std::expected<int, ParseError> parse_positive(std::string_view s) {
    if (s.empty()) return std::unexpected(ParseError::Empty);
    int n = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return std::unexpected(ParseError::NotANumber);
        n = n * 10 + (c - '0');
    }
    return n;
}

}  // namespace

int main() {
    std::flat_map<std::string, int> labels;
    labels.insert({"__name__", 1});
    labels.insert({"job", 2});
    labels.insert({"instance", 3});

    std::println("libc++ version: {}", _LIBCPP_VERSION);
    std::println("flat_map size: {}", labels.size());
    for (const auto& [k, v] : labels) {
        std::println("  {} -> {}", k, v);
    }

    auto good = parse_positive("42");
    auto bad  = parse_positive("nope");
    std::println("parse(\"42\") = {}", good.value_or(-1));
    std::println("parse(\"nope\") has_value = {}", bad.has_value());

    auto squares = std::views::iota(1, 6)
                 | std::views::transform([](int x) { return x * x; });
    std::print("squares:");
    for (int s : squares) std::print(" {}", s);
    std::println();

    return 0;
}
