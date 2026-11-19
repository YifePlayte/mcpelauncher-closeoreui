#include "json.h"

#include <atomic>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <fcntl.h>
#include <unistd.h>

namespace coui {
namespace {

constexpr int kMaxDepth = 64;

class Parser {
public:
    Parser(const std::string& text) : text_(text) {}

    bool parse(Json& out, std::string& error) {
        skipWhitespace();
        if (!parseValue(out, 0)) {
            error = error_.empty() ? "syntax error" : error_;
            return false;
        }
        skipWhitespace();
        if (pos_ != text_.size()) {
            error = "trailing data after JSON value";
            return false;
        }
        return true;
    }

private:
    void skipWhitespace() {
        while (pos_ < text_.size()) {
            char c = text_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++pos_;
            } else {
                break;
            }
        }
    }

    bool fail(const char* msg) {
        if (error_.empty()) {
            error_ = std::string(msg) + " at offset " + std::to_string(pos_);
        }
        return false;
    }

    bool parseLiteral(const char* literal) {
        size_t len = strlen(literal);
        if (text_.compare(pos_, len, literal) != 0) {
            return fail("invalid literal");
        }
        pos_ += len;
        return true;
    }

    bool parseValue(Json& out, int depth) {
        if (depth > kMaxDepth) {
            return fail("nesting too deep");
        }
        skipWhitespace();
        if (pos_ >= text_.size()) {
            return fail("unexpected end of input");
        }

        char c = text_[pos_];
        switch (c) {
            case '{':
                return parseObject(out, depth);
            case '[':
                return parseArray(out, depth);
            case '"': {
                std::string value;
                if (!parseString(value)) {
                    return false;
                }
                out = Json::makeString(std::move(value));
                return true;
            }
            case 't':
                if (!parseLiteral("true")) return false;
                out = Json::makeBool(true);
                return true;
            case 'f':
                if (!parseLiteral("false")) return false;
                out = Json::makeBool(false);
                return true;
            case 'n':
                if (!parseLiteral("null")) return false;
                out = Json();
                return true;
            default:
                if (c == '-' || (c >= '0' && c <= '9')) {
                    return parseNumber(out);
                }
                return fail("unexpected character");
        }
    }

    bool parseNumber(Json& out) {
        size_t start = pos_;
        if (text_[pos_] == '-') {
            ++pos_;
        }
        while (pos_ < text_.size() && isdigit(static_cast<unsigned char>(text_[pos_]))) {
            ++pos_;
        }
        if (pos_ < text_.size() && text_[pos_] == '.') {
            ++pos_;
            while (pos_ < text_.size() && isdigit(static_cast<unsigned char>(text_[pos_]))) {
                ++pos_;
            }
        }
        if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-')) {
                ++pos_;
            }
            while (pos_ < text_.size() && isdigit(static_cast<unsigned char>(text_[pos_]))) {
                ++pos_;
            }
        }
        std::string number = text_.substr(start, pos_ - start);
        if (number.empty()) {
            return fail("empty number");
        }
        char* endp = nullptr;
        double value = strtod(number.c_str(), &endp);
        if (endp == nullptr || *endp != '\0') {
            return fail("malformed number");
        }
        out = Json::makeNumber(value);
        return true;
    }

    bool parseString(std::string& out) {
        if (pos_ >= text_.size() || text_[pos_] != '"') {
            return fail("expected string");
        }
        ++pos_;
        out.clear();
        while (pos_ < text_.size()) {
            char c = text_[pos_++];
            if (c == '"') {
                return true;
            }
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (pos_ >= text_.size()) {
                return fail("unterminated escape");
            }
            char esc = text_[pos_++];
            switch (esc) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    if (pos_ + 4 > text_.size()) {
                        return fail("truncated unicode escape");
                    }
                    unsigned code = 0;
                    for (int i = 0; i < 4; ++i) {
                        char h = text_[pos_ + i];
                        code <<= 4;
                        if (h >= '0' && h <= '9') code |= static_cast<unsigned>(h - '0');
                        else if (h >= 'a' && h <= 'f') code |= static_cast<unsigned>(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') code |= static_cast<unsigned>(h - 'A' + 10);
                        else return fail("bad unicode escape");
                    }
                    pos_ += 4;
                    appendUtf8(out, code);
                    break;
                }
                default:
                    return fail("unknown escape");
            }
        }
        return fail("unterminated string");
    }

    static void appendUtf8(std::string& out, unsigned code) {
        if (code < 0x80) {
            out.push_back(static_cast<char>(code));
        } else if (code < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (code >> 6)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xE0 | (code >> 12)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
    }

    bool parseArray(Json& out, int depth) {
        ++pos_;  // '['
        out = Json::makeArray();
        skipWhitespace();
        if (pos_ < text_.size() && text_[pos_] == ']') {
            ++pos_;
            return true;
        }
        while (true) {
            Json element;
            if (!parseValue(element, depth + 1)) {
                return false;
            }
            out.push(std::move(element));
            skipWhitespace();
            if (pos_ >= text_.size()) {
                return fail("unterminated array");
            }
            if (text_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (text_[pos_] == ']') {
                ++pos_;
                return true;
            }
            return fail("expected ',' or ']'");
        }
    }

    bool parseObject(Json& out, int depth) {
        ++pos_;  // '{'
        out = Json::makeObject();
        skipWhitespace();
        if (pos_ < text_.size() && text_[pos_] == '}') {
            ++pos_;
            return true;
        }
        while (true) {
            skipWhitespace();
            std::string key;
            if (!parseString(key)) {
                return false;
            }
            skipWhitespace();
            if (pos_ >= text_.size() || text_[pos_] != ':') {
                return fail("expected ':'");
            }
            ++pos_;
            Json value;
            if (!parseValue(value, depth + 1)) {
                return false;
            }
            out[key] = std::move(value);
            skipWhitespace();
            if (pos_ >= text_.size()) {
                return fail("unterminated object");
            }
            if (text_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (text_[pos_] == '}') {
                ++pos_;
                return true;
            }
            return fail("expected ',' or '}'");
        }
    }

    const std::string& text_;
    size_t pos_ = 0;
    std::string error_;
};

void escapeTo(std::string& out, const std::string& value) {
    out.push_back('"');
    for (char c : value) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
                    out += buf;
                } else {
                    out.push_back(c);
                }
        }
    }
    out.push_back('"');
}

std::string trim(const std::string& value) {
    size_t begin = 0;
    size_t end = value.size();
    while (begin < end && isspace(static_cast<unsigned char>(value[begin]))) ++begin;
    while (end > begin && isspace(static_cast<unsigned char>(value[end - 1]))) --end;
    return value.substr(begin, end - begin);
}

std::string lowercase(std::string value) {
    for (char& c : value) {
        c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    }
    return value;
}

}  // namespace

Json Json::makeBool(bool v) {
    Json j;
    j.type_ = Type::Bool;
    j.bool_ = v;
    return j;
}

Json Json::makeString(std::string v) {
    Json j;
    j.type_ = Type::String;
    j.string_ = std::move(v);
    return j;
}

Json Json::makeNumber(double v) {
    Json j;
    j.type_ = Type::Number;
    j.number_ = v;
    return j;
}

Json Json::makeObject() {
    Json j;
    j.type_ = Type::Object;
    return j;
}

Json Json::makeArray() {
    Json j;
    j.type_ = Type::Array;
    return j;
}

bool Json::asBool(bool fallback) const {
    return type_ == Type::Bool ? bool_ : fallback;
}

std::string Json::asString(const std::string& fallback) const {
    return type_ == Type::String ? string_ : fallback;
}

const Json* Json::find(const std::string& key) const {
    if (type_ != Type::Object) {
        return nullptr;
    }
    for (const auto& member : members_) {
        if (member.first == key) {
            return &member.second;
        }
    }
    return nullptr;
}

Json& Json::operator[](const std::string& key) {
    if (type_ != Type::Object) {
        *this = makeObject();
    }
    for (auto& member : members_) {
        if (member.first == key) {
            return member.second;
        }
    }
    members_.emplace_back(key, Json());
    return members_.back().second;
}

void Json::push(Json value) {
    if (type_ != Type::Array) {
        *this = makeArray();
    }
    items_.push_back(std::move(value));
}

bool Json::toBool(bool* ok) const {
    bool result = false;
    bool valid = true;
    switch (type_) {
        case Type::Bool:
            result = bool_;
            break;
        case Type::Number:
            result = number_ != 0.0;
            break;
        case Type::String: {
            std::string v = lowercase(trim(string_));
            if (v == "true" || v == "1" || v == "yes" || v == "on" || v == "enabled") {
                result = true;
            } else if (v == "false" || v == "0" || v == "no" || v == "off" ||
                       v == "disabled" || v.empty()) {
                result = false;
            } else {
                valid = false;
            }
            break;
        }
        case Type::Object: {
            // {"value": ...} / {"enabled": ...} wrappers, plus {"disabled": ...}.
            for (const char* key : {"value", "enabled", "enable", "state", "default"}) {
                if (const Json* inner = find(key)) {
                    bool innerOk = false;
                    bool innerValue = inner->toBool(&innerOk);
                    if (innerOk) {
                        result = innerValue;
                        if (ok) *ok = true;
                        return result;
                    }
                }
            }
            if (const Json* disabled = find("disabled")) {
                bool innerOk = false;
                bool innerValue = disabled->toBool(&innerOk);
                if (innerOk) {
                    result = !innerValue;
                    if (ok) *ok = true;
                    return result;
                }
            }
            valid = false;
            break;
        }
        default:
            valid = false;
            break;
    }
    if (ok) {
        *ok = valid;
    }
    return result;
}

std::string Json::dump(int indent) const {
    std::string out;
    dumpTo(out, indent, 0);
    return out;
}

void Json::dumpTo(std::string& out, int indent, int depth) const {
    const std::string pad(static_cast<size_t>(indent) * static_cast<size_t>(depth), ' ');
    const std::string padInner(static_cast<size_t>(indent) * static_cast<size_t>(depth + 1), ' ');
    const bool pretty = indent > 0;

    switch (type_) {
        case Type::Null:
            out += "null";
            break;
        case Type::Bool:
            out += bool_ ? "true" : "false";
            break;
        case Type::Number: {
            char buf[32];
            if (number_ == static_cast<double>(static_cast<long long>(number_)) &&
                std::fabs(number_) < 1e15) {
                snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(number_));
            } else {
                snprintf(buf, sizeof(buf), "%.17g", number_);
            }
            out += buf;
            break;
        }
        case Type::String:
            escapeTo(out, string_);
            break;
        case Type::Array: {
            if (items_.empty()) {
                out += "[]";
                break;
            }
            out += '[';
            for (size_t i = 0; i < items_.size(); ++i) {
                if (i > 0) out += ',';
                if (pretty) {
                    out += '\n';
                    out += padInner;
                }
                items_[i].dumpTo(out, indent, depth + 1);
            }
            if (pretty) {
                out += '\n';
                out += pad;
            }
            out += ']';
            break;
        }
        case Type::Object: {
            if (members_.empty()) {
                out += "{}";
                break;
            }
            out += '{';
            for (size_t i = 0; i < members_.size(); ++i) {
                if (i > 0) out += ',';
                if (pretty) {
                    out += '\n';
                    out += padInner;
                }
                escapeTo(out, members_[i].first);
                out += pretty ? ": " : ":";
                members_[i].second.dumpTo(out, indent, depth + 1);
            }
            if (pretty) {
                out += '\n';
                out += pad;
            }
            out += '}';
            break;
        }
    }
}

bool Json::parse(const std::string& text, Json& out, std::string& error) {
    Parser parser(text);
    return parser.parse(out, error);
}

bool readFile(const std::string& path, std::string& out, std::string& error) {
    FILE* file = fopen(path.c_str(), "rb");
    if (file == nullptr) {
        error = "cannot open " + path;
        return false;
    }
    out.clear();
    char buffer[8192];
    while (true) {
        size_t read = fread(buffer, 1, sizeof(buffer), file);
        if (read > 0) {
            out.append(buffer, read);
        }
        if (read < sizeof(buffer)) {
            break;
        }
    }
    bool bad = ferror(file) != 0;
    fclose(file);
    if (bad) {
        error = "read error on " + path;
        return false;
    }
    return true;
}

bool writeFileAtomically(const std::string& path, const std::string& content,
                         std::string& error) {
    // A private same-directory file prevents concurrent launcher instances from
    // truncating or renaming each other's temporary output.
    // mcpelauncher's synthetic libc does not export mkstemp. Use its supported
    // open(O_EXCL) API, with per-process monotonic names and collision retries.
    static std::atomic<uint64_t> nextTemporary{0};
    const std::string prefix = path + ".tmp." +
            std::to_string(static_cast<uint64_t>(getpid())) + ".";
    std::string temp;
    int fd = -1;
    for (unsigned attempt = 0; attempt < 64; ++attempt) {
        temp = prefix + std::to_string(nextTemporary.fetch_add(1, std::memory_order_relaxed));
        fd = open(temp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd >= 0 || errno != EEXIST) break;
    }
    if (fd < 0) {
        error = "cannot create temporary file for " + path + ": " + strerror(errno);
        return false;
    }
    FILE* file = fdopen(fd, "wb");
    if (file == nullptr) {
        int savedError = errno;
        close(fd);
        unlink(temp.c_str());
        error = "cannot write " + path + ": " + strerror(savedError);
        return false;
    }
    size_t written = content.empty() ? 0 : fwrite(content.data(), 1, content.size(), file);
    bool ok = written == content.size();
    if (fflush(file) != 0) ok = false;
    if (ok && fsync(fd) != 0) ok = false;
    ok = (fclose(file) == 0) && ok;
    if (!ok) {
        unlink(temp.c_str());
        error = "short write on " + temp;
        return false;
    }
    if (rename(temp.c_str(), path.c_str()) != 0) {
        // Never fall back to an in-place copy: every failure must preserve the
        // existing configuration, including when the temporary file disappeared.
        int savedError = errno;
        unlink(temp.c_str());
        error = "cannot replace " + path + ": " + strerror(savedError);
        return false;
    }
    return true;
}

}  // namespace coui
