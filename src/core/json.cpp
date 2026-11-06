#include "mcpsync/json.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace mcpsync::json {

namespace {

Value const& nullValue() {
    static Value v;
    return v;
}

void appendEscaped(std::string& out, std::string_view s) {
    out.push_back('"');
    for (unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof buf, "\\u%04x", static_cast<unsigned>(c));
                out += buf;
            } else {
                out.push_back(static_cast<char>(c));
            }
        }
    }
    out.push_back('"');
}

void appendUtf8(std::string& out, uint32_t cp) {
    if (cp <= 0x7F) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

class Parser {
public:
    Parser(std::string_view text, std::string& err) : mS(text), mErr(err) {}

    bool run(Value& out) {
        skipWs();
        if (!parseValue(out, 0)) return false;
        skipWs();
        if (mI != mS.size()) {
            fail("unexpected trailing characters");
            return false;
        }
        return true;
    }

private:
    void fail(char const* msg) {
        if (mErr.empty()) {
            mErr = std::string(msg) + " at offset " + std::to_string(mI);
        }
    }

    void skipWs() {
        while (mI < mS.size()) {
            char c = mS[mI];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++mI;
            } else {
                break;
            }
        }
    }

    bool consume(char c) {
        if (mI < mS.size() && mS[mI] == c) {
            ++mI;
            return true;
        }
        return false;
    }

    bool parseValue(Value& out, int depth) {
        if (depth > 200) {
            fail("nesting too deep");
            return false;
        }
        if (mI >= mS.size()) {
            fail("unexpected end of input");
            return false;
        }
        char c = mS[mI];
        switch (c) {
        case '{': return parseObject(out, depth);
        case '[': return parseArray(out, depth);
        case '"': {
            std::string s;
            if (!parseString(s)) return false;
            out = Value(std::move(s));
            return true;
        }
        case 't':
            if (mS.compare(mI, 4, "true") == 0) { mI += 4; out = Value(true); return true; }
            fail("invalid literal"); return false;
        case 'f':
            if (mS.compare(mI, 5, "false") == 0) { mI += 5; out = Value(false); return true; }
            fail("invalid literal"); return false;
        case 'n':
            if (mS.compare(mI, 4, "null") == 0) { mI += 4; out = Value(); return true; }
            fail("invalid literal"); return false;
        default: return parseNumber(out);
        }
    }

    bool parseNumber(Value& out) {
        size_t start = mI;
        if (mI < mS.size() && (mS[mI] == '-' || mS[mI] == '+')) ++mI;
        bool digits = false, dot = false, exp = false;
        while (mI < mS.size()) {
            char c = mS[mI];
            if (c >= '0' && c <= '9') { digits = true; ++mI; }
            else if (c == '.' && !dot && !exp) { dot = true; ++mI; }
            else if ((c == 'e' || c == 'E') && !exp && digits) { exp = true; ++mI; }
            else if ((c == '+' || c == '-') && mI > start && (mS[mI - 1] == 'e' || mS[mI - 1] == 'E')) ++mI;
            else break;
        }
        if (!digits) {
            fail("invalid number");
            return false;
        }
        std::string tok(mS.substr(start, mI - start));
        if (!dot && !exp) {
            out = Value(static_cast<int64_t>(std::strtoll(tok.c_str(), nullptr, 10)));
        } else {
            out = Value(std::strtod(tok.c_str(), nullptr));
        }
        return true;
    }

    bool parseString(std::string& out) {
        if (!consume('"')) { fail("expected string"); return false; }
        out.clear();
        while (mI < mS.size()) {
            unsigned char c = static_cast<unsigned char>(mS[mI]);
            if (c == '"') {
                ++mI;
                return true;
            }
            if (c == '\\') {
                ++mI;
                if (mI >= mS.size()) break;
                char e = mS[mI++];
                switch (e) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    uint32_t cp = 0;
                    if (!readHex4(cp)) return false;
                    if (cp >= 0xD800 && cp <= 0xDBFF && mI + 6 <= mS.size() && mS[mI] == '\\' && mS[mI + 1] == 'u') {
                        size_t save = mI;
                        mI += 2;
                        uint32_t lo = 0;
                        if (!readHex4(lo)) return false;
                        if (lo >= 0xDC00 && lo <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        } else {
                            mI = save;
                        }
                    }
                    appendUtf8(out, cp);
                    break;
                }
                default: fail("invalid escape"); return false;
                }
                continue;
            }
            if (c < 0x20) { fail("control character in string"); return false; }
            out.push_back(static_cast<char>(c));
            ++mI;
        }
        fail("unterminated string");
        return false;
    }

    bool readHex4(uint32_t& cp) {
        if (mI + 4 > mS.size()) { fail("bad unicode escape"); return false; }
        cp = 0;
        for (int k = 0; k < 4; ++k) {
            char c = mS[mI++];
            cp <<= 4;
            if (c >= '0' && c <= '9') cp |= static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') cp |= static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') cp |= static_cast<uint32_t>(c - 'A' + 10);
            else { fail("bad unicode escape"); return false; }
        }
        return true;
    }

    bool parseArray(Value& out, int depth) {
        if (!consume('[')) return false;
        Array arr;
        skipWs();
        if (consume(']')) { out = Value(std::move(arr)); return true; }
        for (;;) {
            skipWs();
            Value v;
            if (!parseValue(v, depth + 1)) return false;
            arr.push_back(std::move(v));
            skipWs();
            if (consume(',')) continue;
            if (consume(']')) break;
            fail("expected ',' or ']'");
            return false;
        }
        out = Value(std::move(arr));
        return true;
    }

    bool parseObject(Value& out, int depth) {
        if (!consume('{')) return false;
        Object obj;
        skipWs();
        if (consume('}')) { out = Value(std::move(obj)); return true; }
        for (;;) {
            skipWs();
            std::string key;
            if (!parseString(key)) return false;
            skipWs();
            if (!consume(':')) { fail("expected ':'"); return false; }
            skipWs();
            Value v;
            if (!parseValue(v, depth + 1)) return false;
            obj.emplace_back(std::move(key), std::move(v));
            skipWs();
            if (consume(',')) continue;
            if (consume('}')) break;
            fail("expected ',' or '}'");
            return false;
        }
        out = Value(std::move(obj));
        return true;
    }

    std::string_view mS;
    std::string&     mErr;
    size_t           mI{0};
};

} // namespace

bool Value::asBool(bool def) const noexcept {
    if (mType == Type::Bool) return mBool;
    if (mType == Type::Number) return mNum != 0.0;
    return def;
}

double Value::asDouble(double def) const noexcept {
    if (mType == Type::Number) return mNum;
    if (mType == Type::Bool) return mBool ? 1.0 : 0.0;
    return def;
}

int64_t Value::asInt(int64_t def) const noexcept {
    if (mType != Type::Number) return def;
    return mIsInt ? mInt : static_cast<int64_t>(mNum);
}

std::string Value::asString(std::string def) const {
    if (mType == Type::String) return mStr;
    if (mType == Type::Number) {
        if (mIsInt) return std::to_string(mInt);
        char buf[40];
        std::snprintf(buf, sizeof buf, "%g", mNum);
        return buf;
    }
    if (mType == Type::Bool) return mBool ? "true" : "false";
    return def;
}

Value const* Value::find(std::string_view key) const {
    if (mType != Type::Object) return nullptr;
    for (auto const& kv : mObj) {
        if (kv.first == key) return &kv.second;
    }
    return nullptr;
}

Value* Value::find(std::string_view key) {
    if (mType != Type::Object) return nullptr;
    for (auto& kv : mObj) {
        if (kv.first == key) return &kv.second;
    }
    return nullptr;
}

Value const& Value::at(std::string_view key) const {
    Value const* v = find(key);
    return v ? *v : nullValue();
}

Value const& Value::at(size_t index) const {
    if (mType == Type::Array && index < mArr.size()) return mArr[index];
    return nullValue();
}

size_t Value::size() const noexcept {
    if (mType == Type::Array) return mArr.size();
    if (mType == Type::Object) return mObj.size();
    if (mType == Type::String) return mStr.size();
    return 0;
}

Array const& Value::asArray() const {
    static Array const empty;
    return mType == Type::Array ? mArr : empty;
}

Object const& Value::asObject() const {
    static Object const empty;
    return mType == Type::Object ? mObj : empty;
}

void Value::set(std::string key, Value v) {
    if (mType != Type::Object) {
        mType = Type::Object;
        mObj.clear();
    }
    for (auto& kv : mObj) {
        if (kv.first == key) {
            kv.second = std::move(v);
            return;
        }
    }
    mObj.emplace_back(std::move(key), std::move(v));
}

void Value::push(Value v) {
    if (mType != Type::Array) {
        mType = Type::Array;
        mArr.clear();
    }
    mArr.push_back(std::move(v));
}

std::string Value::dump(int indent) const {
    std::string out;
    dumpTo(out, indent, 0);
    return out;
}

void Value::dumpTo(std::string& out, int indent, int depth) const {
    auto newline = [&](int d) {
        if (indent <= 0) return;
        out.push_back('\n');
        out.append(static_cast<size_t>(d) * static_cast<size_t>(indent), ' ');
    };

    switch (mType) {
    case Type::Null: out += "null"; break;
    case Type::Bool: out += mBool ? "true" : "false"; break;
    case Type::Number:
        if (mIsInt) {
            out += std::to_string(mInt);
        } else {
            char buf[40];
            if (mNum == std::floor(mNum) && std::fabs(mNum) < 1e15) {
                std::snprintf(buf, sizeof buf, "%.0f", mNum);
            } else {
                std::snprintf(buf, sizeof buf, "%.17g", mNum);
            }
            out += buf;
        }
        break;
    case Type::String: appendEscaped(out, mStr); break;
    case Type::Array: {
        if (mArr.empty()) { out += "[]"; break; }
        out.push_back('[');
        for (size_t i = 0; i < mArr.size(); ++i) {
            if (i) out.push_back(',');
            newline(depth + 1);
            mArr[i].dumpTo(out, indent, depth + 1);
        }
        newline(depth);
        out.push_back(']');
        break;
    }
    case Type::Object: {
        if (mObj.empty()) { out += "{}"; break; }
        out.push_back('{');
        for (size_t i = 0; i < mObj.size(); ++i) {
            if (i) out.push_back(',');
            newline(depth + 1);
            appendEscaped(out, mObj[i].first);
            out.push_back(':');
            if (indent > 0) out.push_back(' ');
            mObj[i].second.dumpTo(out, indent, depth + 1);
        }
        newline(depth);
        out.push_back('}');
        break;
    }
    }
}

bool parse(std::string_view text, Value& out, std::string& error) {
    error.clear();
    // 去掉 UTF-8 BOM
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF) {
        text.remove_prefix(3);
    }
    Parser p(text, error);
    return p.run(out);
}

} // namespace mcpsync::json
