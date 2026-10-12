#include "Expr.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <sstream>
#include <utility>

namespace Slic3r {
namespace ModHost {

struct Expr::Node
{
    enum class Kind { Literal, Name, Unary, Binary, Call };
    Kind                               kind;
    ExprValue                          literal;
    std::string                        name;  // Name: the name; Unary/Binary: the operator; Call: the function
    std::vector<std::shared_ptr<Node>> args;  // Name: optional index; Unary: operand; Binary: lhs, rhs; Call: arguments
    size_t                             pos = 0;
};

namespace {

using NodePtr = std::shared_ptr<Expr::Node>;

struct Token
{
    enum class Kind { Number, String, Name, Op, End };
    Kind        kind;
    std::string text;
    double      number = 0.;
    size_t      pos    = 0;
};

[[noreturn]] void fail(const std::string &what, size_t pos)
{
    throw ExprError(what + " at position " + std::to_string(pos + 1));
}

std::vector<Token> tokenize(const std::string &s)
{
    std::vector<Token> out;
    size_t             i = 0;
    while (i < s.size()) {
        const char c = s[i];
        if (std::isspace(static_cast<unsigned char>(c))) {
            ++i;
        } else if (std::isdigit(static_cast<unsigned char>(c)) || (c == '.' && i + 1 < s.size() && std::isdigit(static_cast<unsigned char>(s[i + 1])))) {
            const char *begin = s.c_str() + i;
            char       *end   = nullptr;
            const double v    = std::strtod(begin, &end);
            out.push_back({Token::Kind::Number, std::string(begin, size_t(end - begin)), v, i});
            i += size_t(end - begin);
        } else if (c == '"') {
            std::string text;
            size_t      start = i++;
            while (i < s.size() && s[i] != '"') {
                if (s[i] == '\\' && i + 1 < s.size())
                    ++i;
                text += s[i++];
            }
            if (i >= s.size())
                fail("unterminated string", start);
            ++i;
            out.push_back({Token::Kind::String, text, 0., start});
        } else if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            size_t start = i;
            // A dot joins a mod id to its parameter: "mymod.join_distance".
            while (i < s.size() && (std::isalnum(static_cast<unsigned char>(s[i])) || s[i] == '_' || s[i] == '.'))
                ++i;
            out.push_back({Token::Kind::Name, s.substr(start, i - start), 0., start});
        } else {
            static const char *two_char_ops[] = {"<=", ">=", "==", "!=", "&&", "||"};
            std::string        op(1, c);
            if (i + 1 < s.size())
                for (const char *t : two_char_ops)
                    if (s.compare(i, 2, t) == 0)
                        op = t;
            if (op.size() == 1 && std::string("+-*/%<>!()[],?:").find(c) == std::string::npos)
                fail(std::string("unexpected character '") + c + "'", i);
            out.push_back({Token::Kind::Op, op, 0., i});
            i += op.size();
        }
    }
    out.push_back({Token::Kind::End, "", 0., s.size()});
    return out;
}

class Parser
{
public:
    Parser(std::vector<Token> tokens, std::vector<std::string> &names) : m_tokens(std::move(tokens)), m_names(names) {}

    NodePtr parse_all()
    {
        NodePtr n = parse_binary(0);
        if (peek().kind != Token::Kind::End)
            fail("unexpected '" + peek().text + "'", peek().pos);
        return n;
    }

private:
    const Token &peek() const { return m_tokens[m_pos]; }
    const Token &next() { return m_tokens[m_pos++]; }
    bool accept_op(const char *op)
    {
        if (peek().kind == Token::Kind::Op && peek().text == op) {
            ++m_pos;
            return true;
        }
        return false;
    }
    void expect_op(const char *op)
    {
        if (!accept_op(op))
            fail(std::string("expected '") + op + "'", peek().pos);
    }

    // Binding power of a binary operator; 0 when the token is not one.
    static int precedence(const Token &t, std::string &op)
    {
        if (t.kind == Token::Kind::Name) {
            if (t.text == "or") { op = "||"; return 1; }
            if (t.text == "and") { op = "&&"; return 2; }
            return 0;
        }
        if (t.kind != Token::Kind::Op)
            return 0;
        op = t.text;
        if (op == "||") return 1;
        if (op == "&&") return 2;
        if (op == "==" || op == "!=") return 3;
        if (op == "<" || op == "<=" || op == ">" || op == ">=") return 4;
        if (op == "+" || op == "-") return 5;
        if (op == "*" || op == "/" || op == "%") return 6;
        return 0;
    }

    NodePtr parse_binary(int min_prec)
    {
        NodePtr lhs = parse_unary();
        for (;;) {
            std::string op;
            const int   prec = precedence(peek(), op);
            if (prec == 0 || prec <= min_prec)
                return lhs;
            const size_t pos = next().pos;
            NodePtr      rhs = parse_binary(prec);
            auto         n   = std::make_shared<Expr::Node>();
            n->kind          = Expr::Node::Kind::Binary;
            n->name          = op;
            n->args          = {lhs, rhs};
            n->pos           = pos;
            lhs              = n;
        }
    }

    NodePtr parse_unary()
    {
        const Token &t = peek();
        if ((t.kind == Token::Kind::Op && (t.text == "-" || t.text == "!")) || (t.kind == Token::Kind::Name && t.text == "not")) {
            const size_t pos = next().pos;
            auto         n   = std::make_shared<Expr::Node>();
            n->kind          = Expr::Node::Kind::Unary;
            n->name          = t.text == "-" ? "-" : "!";
            n->args          = {parse_unary()};
            n->pos           = pos;
            return n;
        }
        return parse_primary();
    }

    NodePtr parse_primary()
    {
        const Token t = next();
        auto        n = std::make_shared<Expr::Node>();
        n->pos        = t.pos;
        switch (t.kind) {
        case Token::Kind::Number:
            n->kind    = Expr::Node::Kind::Literal;
            n->literal = t.number;
            return n;
        case Token::Kind::String:
            n->kind    = Expr::Node::Kind::Literal;
            n->literal = t.text;
            return n;
        case Token::Kind::Op:
            if (t.text == "(") {
                NodePtr inner = parse_binary(0);
                expect_op(")");
                return inner;
            }
            fail("unexpected '" + t.text + "'", t.pos);
        case Token::Kind::End:
            fail("expression ends too early", t.pos);
        case Token::Kind::Name:
            break;
        }
        if (t.text == "true" || t.text == "false") {
            n->kind    = Expr::Node::Kind::Literal;
            n->literal = t.text == "true";
            return n;
        }
        if (accept_op("(")) {
            n->kind = Expr::Node::Kind::Call;
            n->name = t.text;
            if (!accept_op(")")) {
                do
                    n->args.push_back(parse_binary(0));
                while (accept_op(","));
                expect_op(")");
            }
            check_call(*n);
            return n;
        }
        n->kind = Expr::Node::Kind::Name;
        n->name = t.text;
        if (std::find(m_names.begin(), m_names.end(), t.text) == m_names.end())
            m_names.push_back(t.text);
        if (accept_op("[")) {
            n->args.push_back(parse_binary(0));
            expect_op("]");
        }
        return n;
    }

    static void check_call(const Expr::Node &n)
    {
        const size_t argc = n.args.size();
        if (n.name == "if") {
            if (argc != 3)
                fail("if() takes 3 arguments", n.pos);
        } else if (n.name == "min" || n.name == "max") {
            if (argc < 2)
                fail(n.name + "() takes at least 2 arguments", n.pos);
        } else if (n.name == "abs" || n.name == "round" || n.name == "floor" || n.name == "ceil") {
            if (argc != 1)
                fail(n.name + "() takes 1 argument", n.pos);
        } else {
            fail("unknown function '" + n.name + "'", n.pos);
        }
    }

    std::vector<Token>        m_tokens;
    size_t                    m_pos = 0;
    std::vector<std::string> &m_names;
};

double as_number(const ExprValue &v, size_t pos)
{
    if (const double *d = std::get_if<double>(&v))
        return *d;
    if (const bool *b = std::get_if<bool>(&v))
        return *b ? 1. : 0.;
    fail("a string is used as a number", pos);
}

ExprValue eval(const Expr::Node &n, const ExprLookup &lookup)
{
    using Kind = Expr::Node::Kind;
    switch (n.kind) {
    case Kind::Literal: return n.literal;
    case Kind::Name: {
        std::optional<size_t> index;
        if (!n.args.empty()) {
            const double i = as_number(eval(*n.args.front(), lookup), n.args.front()->pos);
            if (i < 0. || i != std::floor(i))
                fail("index of '" + n.name + "' is not a whole number >= 0", n.pos);
            index = size_t(i);
        }
        std::optional<ExprValue> v = lookup(n.name, index);
        if (!v)
            fail("unknown name '" + n.name + "'" + (index ? "[" + std::to_string(*index) + "]" : std::string()), n.pos);
        return *v;
    }
    case Kind::Unary: {
        const ExprValue v = eval(*n.args.front(), lookup);
        if (n.name == "!")
            return !expr_truthy(v);
        return -as_number(v, n.pos);
    }
    case Kind::Binary: {
        const std::string &op = n.name;
        // and/or short-circuit, so a guard such as `has_x and x > 1` never reads a missing value.
        if (op == "&&") {
            if (!expr_truthy(eval(*n.args[0], lookup)))
                return false;
            return expr_truthy(eval(*n.args[1], lookup));
        }
        if (op == "||") {
            if (expr_truthy(eval(*n.args[0], lookup)))
                return true;
            return expr_truthy(eval(*n.args[1], lookup));
        }
        const ExprValue a = eval(*n.args[0], lookup);
        const ExprValue b = eval(*n.args[1], lookup);
        if (op == "==" || op == "!=") {
            bool equal;
            if (std::holds_alternative<std::string>(a) || std::holds_alternative<std::string>(b))
                equal = expr_to_string(a) == expr_to_string(b);
            else
                equal = as_number(a, n.pos) == as_number(b, n.pos);
            return op == "==" ? equal : !equal;
        }
        if (op == "+" && std::holds_alternative<std::string>(a) && std::holds_alternative<std::string>(b))
            return std::get<std::string>(a) + std::get<std::string>(b);
        const double x = as_number(a, n.args[0]->pos);
        const double y = as_number(b, n.args[1]->pos);
        if (op == "+") return x + y;
        if (op == "-") return x - y;
        if (op == "*") return x * y;
        if (op == "/") {
            if (y == 0.)
                fail("division by zero", n.pos);
            return x / y;
        }
        if (op == "%") {
            if (y == 0.)
                fail("division by zero", n.pos);
            return std::fmod(x, y);
        }
        if (op == "<") return x < y;
        if (op == "<=") return x <= y;
        if (op == ">") return x > y;
        return x >= y;
    }
    case Kind::Call: {
        const std::string &f = n.name;
        if (f == "if")
            return expr_truthy(eval(*n.args[0], lookup)) ? eval(*n.args[1], lookup) : eval(*n.args[2], lookup);
        if (f == "min" || f == "max") {
            double r = as_number(eval(*n.args[0], lookup), n.args[0]->pos);
            for (size_t i = 1; i < n.args.size(); ++i) {
                const double v = as_number(eval(*n.args[i], lookup), n.args[i]->pos);
                r              = f == "min" ? std::min(r, v) : std::max(r, v);
            }
            return r;
        }
        const double v = as_number(eval(*n.args[0], lookup), n.args[0]->pos);
        if (f == "abs") return std::abs(v);
        if (f == "round") return std::round(v);
        if (f == "floor") return std::floor(v);
        return std::ceil(v);
    }
    }
    fail("bad expression node", n.pos);
}

} // namespace

Expr Expr::parse(const std::string &text)
{
    Expr e;
    e.m_text = text;
    Parser parser(tokenize(text), e.m_names);
    e.m_root = parser.parse_all();
    return e;
}

ExprValue Expr::evaluate(const ExprLookup &lookup) const { return eval(*m_root, lookup); }

bool expr_truthy(const ExprValue &value)
{
    if (const bool *b = std::get_if<bool>(&value))
        return *b;
    if (const double *d = std::get_if<double>(&value))
        return *d != 0.;
    return !std::get<std::string>(value).empty();
}

std::string expr_to_string(const ExprValue &value)
{
    if (const std::string *s = std::get_if<std::string>(&value))
        return *s;
    if (const bool *b = std::get_if<bool>(&value))
        return *b ? "true" : "false";
    std::ostringstream ss;
    ss.imbue(std::locale::classic());
    ss.precision(17);
    ss << std::get<double>(value);
    return ss.str();
}

} // namespace ModHost
} // namespace Slic3r
