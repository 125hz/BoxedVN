/*
 *  BoxedVN - iOS/iPadOS port of Boxedwine
 *  Copyright (C) 2026  The BoxedWine Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.  See license.txt.
 *
 *  A minimal JSON reader/writer.  BoxedVN needs JSON only for its own
 *  manifests and dependency lock files, so this deliberately supports just the
 *  subset those use rather than pulling in another third-party dependency into
 *  a GPLv2 tree.
 */

#ifndef BOXEDVN_JSON_H
#define BOXEDVN_JSON_H

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace boxedvn {
namespace json {

class Value;
using ValuePtr = std::shared_ptr<Value>;

enum class Type { Null, Bool, Number, String, Array, Object };

class Value {
public:
    Type type = Type::Null;
    bool boolValue = false;
    double numberValue = 0.0;
    std::string stringValue;
    std::vector<ValuePtr> arrayValue;

    // Insertion-ordered object, so serialisation round-trips stably.
    std::vector<std::pair<std::string, ValuePtr>> objectValue;

    const Value* find(const std::string& key) const;

    bool isObject() const { return type == Type::Object; }
    bool isArray() const { return type == Type::Array; }
    bool isString() const { return type == Type::String; }
    bool isNumber() const { return type == Type::Number; }
    bool isBool() const { return type == Type::Bool; }
};

// Escapes a string as a JSON string literal, including surrounding quotes.
std::string escapeString(const std::string& input);

struct ParseResult {
    bool ok = false;
    std::string error;   // includes a byte offset when parsing failed
    ValuePtr value;
};

ParseResult parse(const std::string& text);

// A tiny writer that produces stable, indented output.
class Writer {
public:
    explicit Writer(int indentWidth = 2) : indentWidth_(indentWidth) {}

    void beginObject();
    void endObject();
    void beginArray();
    void endArray();
    void key(const std::string& name);
    void value(const std::string& text);
    void value(const char* text);
    void value(int64_t number);
    void value(double number);
    void value(bool flag);
    void nullValue();

    std::string str() const { return out_; }

private:
    void prepareValue();
    void newlineIndent();

    std::string out_;
    int indentWidth_;
    int depth_ = 0;
    bool needComma_ = false;
    bool afterKey_ = false;
};

}  // namespace json
}  // namespace boxedvn

#endif  // BOXEDVN_JSON_H
