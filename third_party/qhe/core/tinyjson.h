#pragma once
// Minimal single-header fork of nlohmann/json v3.11 for HSTF parsing.
// Only what's needed: string, int, float, bool, null, object, array.
// Size-optimized for embedded use in the HSR renderer.

#include <cstdint>
#include <string>
#include <vector>
#include <map>
#include <stdexcept>
#include <sstream>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace tinyjson {

enum class Type { Null, Bool, Int, Float, String, Array, Object };

class Value {
public:
    Type type = Type::Null;
    bool boolVal = false;
    int64_t intVal = 0;
    double floatVal = 0.0;
    std::string strVal;
    std::vector<Value> arrVal;
    std::map<std::string, Value> objVal;

    Value() = default;
    Value(Type t) : type(t) {}

    bool isNull()    const { return type == Type::Null; }
    bool isBool()    const { return type == Type::Bool; }
    bool isInt()     const { return type == Type::Int; }
    bool isFloat()   const { return type == Type::Float; }
    bool isString()  const { return type == Type::String; }
    bool isArray()   const { return type == Type::Array; }
    bool isObject()  const { return type == Type::Object; }
    bool isNumber()  const { return type == Type::Int || type == Type::Float; }

    std::string asString() const { return strVal; }
    int64_t asInt() const {
        if (type == Type::Int) return intVal;
        if (type == Type::Float)   // (NaN / out of range: undefined as a cast; 0 or the nearest end instead)
            return floatVal >= 9.2e18 ? INT64_MAX : floatVal <= -9.2e18 ? INT64_MIN : floatVal == floatVal ? (int64_t)floatVal : 0;
        // Asset IDs may be quoted ("targetId": "2719744159" in community shellconfig/HSTF) —
        // parse unsigned so u32 targetIds above INT_MAX survive the (u32) cast at call sites.
        if (type == Type::String) return (int64_t)std::strtoull(strVal.c_str(), nullptr, 10);
        return 0;
    }
    double asFloat() const {
        if (type == Type::Float) return floatVal;
        if (type == Type::Int) return (double)intVal;
        return 0.0;
    }
    bool asBool() const { return boolVal; }

    Value& operator[](const std::string& k) { return objVal[k]; }
    // Out-of-range / non-array index reads as Null (file data drives the indices). The mutable overload hands
    // back a per-thread scratch Null so a stray write can't corrupt anything.
    Value& operator[](size_t i) {
        if (i < arrVal.size()) return arrVal[i];
        thread_local Value scratch; scratch = Value(); return scratch;
    }
    const Value& operator[](const std::string& k) const {
        static Value empty;
        auto it = objVal.find(k);
        return it != objVal.end() ? it->second : empty;
    }
    const Value& operator[](size_t i) const {
        static const Value empty;
        return i < arrVal.size() ? arrVal[i] : empty;
    }
    // Element count of an array (0 otherwise), so size()/operator[](size_t) loops can't index an object.
    size_t size() const { return isArray() ? arrVal.size() : 0; }

    bool has(const std::string& k) const { return objVal.find(k) != objVal.end(); }
};

// Fast JSON parser — no extra allocations for simple values
class Parser {
    const char* p;
    const char* end;
    // Nesting cap: recursion depth (and the recursive Value destructor) stays bounded on hostile input.
    static constexpr int kMaxDepth = 256;
    int depth = 0;
public:
    Parser(const std::string& s) : p(s.data()), end(s.data() + s.size()) {}

    Value parse() {
        skipWS();
        return parseValue();
    }

private:
    void skipWS() { while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p; }

    char peek() { return p < end ? *p : 0; }
    char next() { return p < end ? *p++ : 0; }

    Value parseValue() {
        skipWS();
        switch (peek()) {
            case '"': return parseString();
            case '{': return parseObject();
            case '[': return parseArray();
            case 't': case 'f': return parseBool();
            case 'n': return parseNull();
            default: return parseNumber();
        }
    }

    Value parseNull() {
        if (p + 4 <= end && memcmp(p, "null", 4) == 0) { p += 4; return Value(Type::Null); }
        throw std::runtime_error("Expected null");
    }

    Value parseBool() {
        if (p + 4 <= end && memcmp(p, "true", 4) == 0) {
            p += 4; Value v(Type::Bool); v.boolVal = true; return v;
        }
        if (p + 5 <= end && memcmp(p, "false", 5) == 0) {
            p += 5; Value v(Type::Bool); v.boolVal = false; return v;
        }
        throw std::runtime_error("Expected bool");
    }

    Value parseNumber() {
        bool neg = false;
        if (peek() == '-') { neg = true; next(); }

        // Try to parse as integer first
        bool isFloat = false;
        int64_t intVal = 0;
        double floatVal = 0;
        const char* start = p;

        uint64_t acc = 0;   // unsigned: overlong digit runs wrap instead of signed-overflow UB
        while (p < end && *p >= '0' && *p <= '9') {
            acc = acc * 10 + (uint64_t)(*p - '0');
            ++p;
        }
        intVal = (int64_t)acc;
        if (peek() == '.') {
            isFloat = true;
            next();
            double frac = 0, scale = 0.1;
            while (p < end && *p >= '0' && *p <= '9') {
                frac += (*p - '0') * scale;
                scale *= 0.1;
                ++p;
            }
            floatVal = (double)intVal + frac;
        }
        if (peek() == 'e' || peek() == 'E') {
            if (!isFloat) floatVal = (double)intVal;   // "1e0": no '.', so the value so far is the integer part
            isFloat = true;
            next();
            bool eNeg = (peek() == '-');
            if (eNeg || peek() == '+') next();
            double exp = 0;
            while (p < end && *p >= '0' && *p <= '9') { exp = exp * 10 + (*p - '0'); ++p; }
            floatVal = floatVal * std::pow(10.0, eNeg ? -exp : exp);
        }
        if (p == start) throw std::runtime_error("Expected number");

        if (neg) { intVal = (int64_t)(0 - (uint64_t)intVal); floatVal = -floatVal; }
        Value v(isFloat ? Type::Float : Type::Int);
        v.intVal = intVal;
        v.floatVal = floatVal;
        return v;
    }

    Value parseString() {
        next(); // skip opening "
        Value v(Type::String);
        while (p < end && peek() != '"') {
            if (peek() == '\\') {
                next();
                switch (next()) {
                    case '"': v.strVal += '"'; break;
                    case '\\': v.strVal += '\\'; break;
                    case '/': v.strVal += '/'; break;
                    case 'n': v.strVal += '\n'; break;
                    case 't': v.strVal += '\t'; break;
                    case 'r': v.strVal += '\r'; break;
                    default: v.strVal += '\\'; break;
                }
            } else {
                v.strVal += next();
            }
        }
        if (peek() == '"') next();
        return v;
    }

    void enter() { if (++depth > kMaxDepth) throw std::runtime_error("JSON nested too deep"); }

    Value parseArray() {
        enter();
        next(); // skip '['
        Value v(Type::Array);
        skipWS();
        while (peek() != ']' && p < end) {
            v.arrVal.push_back(parseValue());
            skipWS();
            if (peek() == ',') { next(); skipWS(); }
        }
        if (peek() == ']') next();
        --depth;
        return v;
    }

    Value parseObject() {
        enter();
        next(); // skip '{'
        Value v(Type::Object);
        skipWS();
        while (peek() != '}' && p < end) {
            Value key = parseValue();
            skipWS();
            if (peek() == ':') next();
            Value val = parseValue();
            v.objVal[key.asString()] = std::move(val);
            skipWS();
            if (peek() == ',') { next(); skipWS(); }
        }
        if (peek() == '}') next();
        --depth;
        return v;
    }
};

inline Value parse(const std::string& s) { return Parser(s).parse(); }

} // namespace tinyjson
