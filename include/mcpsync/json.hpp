// mcpack-sync : 极简 JSON（保留键顺序，便于配置/状态文件的稳定 diff）
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mcpsync::json {

class Value;
using Array  = std::vector<Value>;
using Object = std::vector<std::pair<std::string, Value>>; // 保持插入顺序

enum class Type { Null, Bool, Number, String, Array, Object };

class Value {
public:
    Value() : mType(Type::Null) {}
    Value(std::nullptr_t) : mType(Type::Null) {}
    Value(bool b) : mType(Type::Bool), mBool(b) {}
    Value(int v) : mType(Type::Number), mNum(static_cast<double>(v)), mInt(v), mIsInt(true) {}
    Value(int64_t v) : mType(Type::Number), mNum(static_cast<double>(v)), mInt(v), mIsInt(true) {}
    Value(uint64_t v) : mType(Type::Number), mNum(static_cast<double>(v)), mInt(static_cast<int64_t>(v)), mIsInt(true) {}
    Value(double v) : mType(Type::Number), mNum(v), mInt(0), mIsInt(false) {}
    Value(const char* s) : mType(Type::String), mStr(s) {}
    Value(std::string s) : mType(Type::String), mStr(std::move(s)) {}
    Value(std::string_view s) : mType(Type::String), mStr(s) {}
    Value(Array a) : mType(Type::Array), mArr(std::move(a)) {}
    Value(Object o) : mType(Type::Object), mObj(std::move(o)) {}

    static Value array() { return Value(Array{}); }
    static Value object() { return Value(Object{}); }

    Type type() const noexcept { return mType; }
    bool isNull() const noexcept { return mType == Type::Null; }
    bool isObject() const noexcept { return mType == Type::Object; }
    bool isArray() const noexcept { return mType == Type::Array; }
    bool isString() const noexcept { return mType == Type::String; }
    bool isNumber() const noexcept { return mType == Type::Number; }
    bool isBool() const noexcept { return mType == Type::Bool; }

    // 宽松取值：类型不符时返回默认值
    bool        asBool(bool def = false) const noexcept;
    double      asDouble(double def = 0.0) const noexcept;
    int64_t     asInt(int64_t def = 0) const noexcept;
    std::string asString(std::string def = {}) const;

    // 对象/数组访问
    Value const* find(std::string_view key) const;
    Value*       find(std::string_view key);
    bool         has(std::string_view key) const { return find(key) != nullptr; }

    Value const& at(std::string_view key) const;      // 缺失返回静态 null
    Value const& at(size_t index) const;              // 越界返回静态 null

    size_t size() const noexcept;
    Array const&  asArray() const;
    Object const& asObject() const;

    // 写入（对象自动建键）
    void set(std::string key, Value v);
    void push(Value v);

    std::string dump(int indent = 2) const;

private:
    void dumpTo(std::string& out, int indent, int depth) const;

    Type        mType{Type::Null};
    bool        mBool{false};
    double      mNum{0.0};
    int64_t     mInt{0};
    bool        mIsInt{false};
    std::string mStr;
    Array       mArr;
    Object      mObj;
};

// 解析；失败返回 false 并填充 error
bool parse(std::string_view text, Value& out, std::string& error);

} // namespace mcpsync::json
