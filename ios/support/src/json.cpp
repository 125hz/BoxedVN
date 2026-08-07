/*
 *  BoxedVN - iOS/iPadOS port of Boxedwine
 *  Copyright (C) 2026  The BoxedWine Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.  See license.txt.
 */

#include "boxedvn/json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace boxedvn {
namespace json {
namespace {

class Parser {
public:
    explicit Parser(const std::string& text) : text_(text) {}

    ParseResult run() {
        ParseResult result;
        skipWhitespace();
        ValuePtr value = parseValue();
        if (!error_.empty()) {
            result.error = error_;
            return result;
        }
        skipWhitespace();
        if (pos_ != text_.size()) {
            result.error = "Trailing content at byte " + std::to_string(pos_) + ".";
            return result;
        }
        result.ok = true;
        result.value = value;
        return result;
    }

private:
    void fail(const std::string& message) {
        if (error_.empty()) {
            error_ = message + " (at byte " + std::to_string(pos_) + ")";
        }
    }

    bool atEnd() const { return pos_ >= text_.size(); }
    char peek() const { return text_[pos_]; }

    void skipWhitespace() {
        while (!atEnd()) {
            const char c = text_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                pos_++;
            } else {
                break;
            }
        }
    }

    bool expect(char c) {
        if (atEnd() || text_[pos_] != c) {
            fail(std::string("Expected '") + c + "'");
            return false;
        }
        pos_++;
        return true;
    }

    bool literal(const char* word) {
        const size_t length = std::char_traits<char>::length(word);
        if (text_.compare(pos_, length, word) != 0) {
            return false;
        }
        pos_ += length;
        return true;
    }

    ValuePtr parseValue() {
        if (!error_.empty()) {
            return nullptr;
        }
        if (depth_ > kMaxDepth) {
            fail("JSON nesting is deeper than " + std::to_string(kMaxDepth) +
                 " levels");
            return nullptr;
        }
        skipWhitespace();
        if (atEnd()) {
            fail("Unexpected end of input");
            return nullptr;
        }

        switch (peek()) {
            case '{': return parseObject();
            case '[': return parseArray();
            case '"': return parseString();
            case 't': {
                if (!literal("true")) { fail("Invalid literal"); return nullptr; }
                auto v = std::make_shared<Value>();
                v->type = Type::Bool;
                v->boolValue = true;
                return v;
            }
            case 'f': {
                if (!literal("false")) { fail("Invalid literal"); return nullptr; }
                auto v = std::make_shared<Value>();
                v->type = Type::Bool;
                v->boolValue = false;
                return v;
            }
            case 'n': {
                if (!literal("null")) { fail("Invalid literal"); return nullptr; }
                return std::make_shared<Value>();
            }
            default: return parseNumber();
        }
    }

    ValuePtr parseObject() {
        depth_++;
        if (!expect('{')) { depth_--; return nullptr; }
        auto object = std::make_shared<Value>();
        object->type = Type::Object;

        skipWhitespace();
        if (!atEnd() && peek() == '}') {
            pos_++;
            depth_--;
            return object;
        }

        while (true) {
            skipWhitespace();
            ValuePtr key = parseString();
            if (!error_.empty()) { depth_--; return nullptr; }
            skipWhitespace();
            if (!expect(':')) { depth_--; return nullptr; }
            ValuePtr value = parseValue();
            if (!error_.empty()) { depth_--; return nullptr; }
            object->objectValue.emplace_back(key->stringValue, value);

            skipWhitespace();
            if (atEnd()) { fail("Unterminated object"); depth_--; return nullptr; }
            if (peek() == ',') { pos_++; continue; }
            if (peek() == '}') { pos_++; break; }
            fail("Expected ',' or '}' in object");
            depth_--;
            return nullptr;
        }
        depth_--;
        return object;
    }

    ValuePtr parseArray() {
        depth_++;
        if (!expect('[')) { depth_--; return nullptr; }
        auto array = std::make_shared<Value>();
        array->type = Type::Array;

        skipWhitespace();
        if (!atEnd() && peek() == ']') {
            pos_++;
            depth_--;
            return array;
        }

        while (true) {
            ValuePtr value = parseValue();
            if (!error_.empty()) { depth_--; return nullptr; }
            array->arrayValue.push_back(value);

            skipWhitespace();
            if (atEnd()) { fail("Unterminated array"); depth_--; return nullptr; }
            if (peek() == ',') { pos_++; continue; }
            if (peek() == ']') { pos_++; break; }
            fail("Expected ',' or ']' in array");
            depth_--;
            return nullptr;
        }
        depth_--;
        return array;
    }

    void appendUtf8(std::string& out, uint32_t codepoint) {
        if (codepoint < 0x80) {
            out.push_back(static_cast<char>(codepoint));
        } else if (codepoint < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
            out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        } else if (codepoint < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
            out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
            out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        }
    }

    bool parseHex4(uint32_t* out) {
        if (pos_ + 4 > text_.size()) {
            return false;
        }
        uint32_t value = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = text_[pos_ + i];
            value <<= 4;
            if (c >= '0' && c <= '9') {
                value |= static_cast<uint32_t>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                value |= static_cast<uint32_t>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                value |= static_cast<uint32_t>(c - 'A' + 10);
            } else {
                return false;
            }
        }
        pos_ += 4;
        *out = value;
        return true;
    }

    ValuePtr parseString() {
        if (!expect('"')) {
            return nullptr;
        }
        auto value = std::make_shared<Value>();
        value->type = Type::String;

        while (true) {
            if (atEnd()) {
                fail("Unterminated string");
                return nullptr;
            }
            const char c = text_[pos_++];
            if (c == '"') {
                break;
            }
            if (c != '\\') {
                value->stringValue.push_back(c);
                continue;
            }
            if (atEnd()) {
                fail("Unterminated escape sequence");
                return nullptr;
            }
            const char escape = text_[pos_++];
            switch (escape) {
                case '"':  value->stringValue.push_back('"');  break;
                case '\\': value->stringValue.push_back('\\'); break;
                case '/':  value->stringValue.push_back('/');  break;
                case 'b':  value->stringValue.push_back('\b'); break;
                case 'f':  value->stringValue.push_back('\f'); break;
                case 'n':  value->stringValue.push_back('\n'); break;
                case 'r':  value->stringValue.push_back('\r'); break;
                case 't':  value->stringValue.push_back('\t'); break;
                case 'u': {
                    uint32_t codepoint = 0;
                    if (!parseHex4(&codepoint)) {
                        fail("Malformed \\u escape");
                        return nullptr;
                    }
                    if (codepoint >= 0xD800 && codepoint <= 0xDBFF) {
                        // High surrogate; a low surrogate must follow.
                        if (pos_ + 1 < text_.size() && text_[pos_] == '\\' &&
                            text_[pos_ + 1] == 'u') {
                            pos_ += 2;
                            uint32_t low = 0;
                            if (!parseHex4(&low) || low < 0xDC00 || low > 0xDFFF) {
                                fail("Malformed surrogate pair");
                                return nullptr;
                            }
                            codepoint = 0x10000 +
                                        ((codepoint - 0xD800) << 10) +
                                        (low - 0xDC00);
                        } else {
                            fail("Unpaired high surrogate");
                            return nullptr;
                        }
                    }
                    appendUtf8(value->stringValue, codepoint);
                    break;
                }
                default:
                    fail("Unknown escape sequence");
                    return nullptr;
            }
        }
        return value;
    }

    ValuePtr parseNumber() {
        const size_t start = pos_;
        if (!atEnd() && peek() == '-') {
            pos_++;
        }
        bool sawDigit = false;
        while (!atEnd() && peek() >= '0' && peek() <= '9') {
            pos_++;
            sawDigit = true;
        }
        if (!atEnd() && peek() == '.') {
            pos_++;
            while (!atEnd() && peek() >= '0' && peek() <= '9') {
                pos_++;
                sawDigit = true;
            }
        }
        if (!atEnd() && (peek() == 'e' || peek() == 'E')) {
            pos_++;
            if (!atEnd() && (peek() == '+' || peek() == '-')) {
                pos_++;
            }
            while (!atEnd() && peek() >= '0' && peek() <= '9') {
                pos_++;
            }
        }
        if (!sawDigit) {
            fail("Expected a value");
            return nullptr;
        }
        auto value = std::make_shared<Value>();
        value->type = Type::Number;
        value->numberValue = std::strtod(text_.substr(start, pos_ - start).c_str(),
                                         nullptr);
        return value;
    }

    static constexpr int kMaxDepth = 64;

    const std::string& text_;
    size_t pos_ = 0;
    int depth_ = 0;
    std::string error_;
};

}  // namespace

const Value* Value::find(const std::string& key) const {
    for (const auto& entry : objectValue) {
        if (entry.first == key) {
            return entry.second.get();
        }
    }
    return nullptr;
}

std::string escapeString(const std::string& input) {
    std::string out;
    out.reserve(input.size() + 2);
    out.push_back('"');
    for (unsigned char c : input) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    char buffer[8];
                    std::snprintf(buffer, sizeof(buffer), "\\u%04X", c);
                    out += buffer;
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    out.push_back('"');
    return out;
}

ParseResult parse(const std::string& text) {
    Parser parser(text);
    return parser.run();
}

void Writer::newlineIndent() {
    out_.push_back('\n');
    out_.append(static_cast<size_t>(depth_ * indentWidth_), ' ');
}

void Writer::prepareValue() {
    if (afterKey_) {
        afterKey_ = false;
        return;
    }
    if (needComma_) {
        out_.push_back(',');
    }
    if (!out_.empty()) {
        newlineIndent();
    }
    needComma_ = true;
}

void Writer::beginObject() {
    prepareValue();
    out_.push_back('{');
    depth_++;
    needComma_ = false;
}

void Writer::endObject() {
    depth_--;
    if (needComma_) {
        newlineIndent();
    }
    out_.push_back('}');
    needComma_ = true;
}

void Writer::beginArray() {
    prepareValue();
    out_.push_back('[');
    depth_++;
    needComma_ = false;
}

void Writer::endArray() {
    depth_--;
    if (needComma_) {
        newlineIndent();
    }
    out_.push_back(']');
    needComma_ = true;
}

void Writer::key(const std::string& name) {
    if (needComma_) {
        out_.push_back(',');
    }
    newlineIndent();
    out_ += escapeString(name);
    out_ += ": ";
    needComma_ = true;
    afterKey_ = true;
}

void Writer::value(const std::string& text) {
    prepareValue();
    out_ += escapeString(text);
}

void Writer::value(const char* text) {
    value(std::string(text != nullptr ? text : ""));
}

void Writer::value(int64_t number) {
    prepareValue();
    out_ += std::to_string(number);
}

void Writer::value(double number) {
    prepareValue();
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.17g", number);
    out_ += buffer;
}

void Writer::value(bool flag) {
    prepareValue();
    out_ += flag ? "true" : "false";
}

void Writer::nullValue() {
    prepareValue();
    out_ += "null";
}

}  // namespace json
}  // namespace boxedvn
