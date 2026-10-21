#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace Slic3r {
namespace ModHost {

// A value an expression produces: a number (integers included), a boolean or a string.
using ExprValue = std::variant<double, bool, std::string>;

class ExprError : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

// Supplies the value of a name an expression reads: a setting ("layer_height"), an element of a
// vector setting ("nozzle_diameter", index 0), or a mod parameter ("mymod.join_distance").
// Returns nothing when the name is unknown or the index is out of range.
using ExprLookup = std::function<std::optional<ExprValue>(const std::string &name, std::optional<size_t> index)>;

// A parsed expression. The syntax follows Orca's G-code placeholder expressions: numbers, "strings",
// true/false, names with an optional [index], + - * / %, comparisons, == !=, and/or/not (also && || !),
// parentheses, and the functions if(cond, a, b), min, max, abs, round, floor and ceil. Evaluation has
// no side effects, so the same inputs always give the same result.
class Expr
{
public:
    // Throws ExprError with the position of the first problem.
    static Expr parse(const std::string &text);

    ExprValue evaluate(const ExprLookup &lookup) const;

    // Every name the expression reads, without indices, in first-seen order.
    const std::vector<std::string> &names() const { return m_names; }
    const std::string &text() const { return m_text; }

    struct Node;

private:
    std::string               m_text;
    std::shared_ptr<Node>     m_root;
    std::vector<std::string>  m_names;
};

// Truthiness used by conditions: false, 0 and "" are false.
bool expr_truthy(const ExprValue &value);
std::string expr_to_string(const ExprValue &value);

// std::strtod with a decimal point whatever the thread's C locale (mod manifests, stored mod settings and legacy
// keys always use one): skips leading white space and a sign, and sets *end past the number, or to text when
// there is none.
double parse_decimal(const char *text, const char **end);
// The shortest text with a decimal point that parse_decimal reads back as `value` (6 to 17 significant digits).
std::string format_decimal(double value);

} // namespace ModHost
} // namespace Slic3r
