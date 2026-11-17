#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace coui {

// Minimal, dependency-free JSON model. Only what a mod config file needs:
// booleans, numbers, strings, arrays and objects. Objects keep insertion order so
// a rewritten config reads back the same way the user wrote it.
class Json {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Json() = default;
    static Json makeBool(bool v);
    static Json makeString(std::string v);
    static Json makeNumber(double v);
    static Json makeObject();
    static Json makeArray();

    Type type() const { return type_; }
    bool isNull() const { return type_ == Type::Null; }
    bool isBool() const { return type_ == Type::Bool; }
    bool isObject() const { return type_ == Type::Object; }
    bool isArray() const { return type_ == Type::Array; }
    bool isString() const { return type_ == Type::String; }

    bool asBool(bool fallback = false) const;
    std::string asString(const std::string& fallback = {}) const;

    // Object access. Returns nullptr when absent or when this is not an object.
    const Json* find(const std::string& key) const;
    Json& operator[](const std::string& key);
    const std::vector<std::pair<std::string, Json>>& members() const { return members_; }

    // Array access.
    const std::vector<Json>& items() const { return items_; }
    void push(Json value);

    // Loose truthiness: accepts bool, numbers, and the strings true/false/1/0/
    // yes/no/on/off/enabled/disabled, matching what hand-edited files contain.
    bool toBool(bool* ok = nullptr) const;

    std::string dump(int indent = 4) const;

    static bool parse(const std::string& text, Json& out, std::string& error);

private:
    void dumpTo(std::string& out, int indent, int depth) const;

    Type type_ = Type::Null;
    bool bool_ = false;
    double number_ = 0;
    std::string string_;
    std::vector<Json> items_;
    std::vector<std::pair<std::string, Json>> members_;
};

// Reads a whole file. Returns false on I/O errors.
bool readFile(const std::string& path, std::string& out, std::string& error);
bool writeFileAtomically(const std::string& path, const std::string& content, std::string& error);

}  // namespace coui
