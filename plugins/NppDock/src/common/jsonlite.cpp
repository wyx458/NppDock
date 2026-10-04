// ============================================================================
// jsonlite.cpp —— 见 jsonlite.h 的说明
// ============================================================================
#include "jsonlite.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace jsonlite {

const Value* Value::Find(const char* key) const
{
    if (type != Type::Object || !key) return nullptr;
    for (const auto& kv : obj) {
        if (kv.first == key) return &kv.second;
    }
    return nullptr;
}

std::string Value::Str(const char* key, const std::string& def) const
{
    const Value* v = Find(key);
    return (v && v->IsStr()) ? v->str : def;
}

double Value::Num(const char* key, double def) const
{
    const Value* v = Find(key);
    return (v && v->IsNum()) ? v->num : def;
}

int Value::Int(const char* key, int def) const
{
    const Value* v = Find(key);
    return (v && v->IsNum()) ? (int)(v->num + (v->num >= 0 ? 0.5 : -0.5)) : def;
}

// ---------------------------------------------------------------------------
// 解析器（递归下降；深度有上限，防止手改出一个深不见底的括号把栈吃掉）
// ---------------------------------------------------------------------------
namespace {

const int kMaxDepth = 32;

struct Parser {
    const std::string& s;
    size_t             i    = 0;
    int                depth = 0;
    std::string        err;

    explicit Parser(const std::string& src) : s(src) {}

    bool Fail(const char* why)
    {
        char buf[160];
        // 行号按已扫过的换行数算 —— 报错要能直接定位到人改的那一行
        int line = 1;
        for (size_t k = 0; k < i && k < s.size(); ++k)
            if (s[k] == '\n') ++line;
        sprintf_s(buf, "第 %d 行：%s", line, why);
        err = buf;
        return false;
    }

    void SkipWs()
    {
        for (;;) {
            while (i < s.size() && (unsigned char)s[i] <= ' ') ++i;
            // 容忍注释（标准 JSON 没有，但手改配置时很需要）
            if (i + 1 < s.size() && s[i] == '/' && s[i + 1] == '/') {
                while (i < s.size() && s[i] != '\n') ++i;
                continue;
            }
            if (i + 1 < s.size() && s[i] == '/' && s[i + 1] == '*') {
                i += 2;
                while (i + 1 < s.size() && !(s[i] == '*' && s[i + 1] == '/')) ++i;
                i = (i + 1 < s.size()) ? i + 2 : s.size();
                continue;
            }
            break;
        }
    }

    bool Lit(const char* word)
    {
        const size_t n = strlen(word);
        if (s.compare(i, n, word) == 0) { i += n; return true; }
        return false;
    }

    bool ParseValue(Value& v)
    {
        if (++depth > kMaxDepth) { --depth; return Fail("嵌套太深（超过 32 层）"); }
        const bool ok = ParseValueInner(v);
        --depth;
        return ok;
    }

    bool ParseString(std::string& out)
    {
        if (i >= s.size() || s[i] != '"') return Fail("这里应该是字符串（缺少引号）");
        ++i;
        out.clear();
        while (i < s.size()) {
            const char c = s[i++];
            if (c == '"') return true;
            if (c != '\\') { out.push_back(c); continue; }
            if (i >= s.size()) break;
            const char e = s[i++];
            switch (e) {
            case '"':  out.push_back('"');  break;
            case '\\': out.push_back('\\'); break;
            case '/':  out.push_back('/');  break;
            case 'b':  out.push_back('\b'); break;
            case 'f':  out.push_back('\f'); break;
            case 'n':  out.push_back('\n'); break;
            case 'r':  out.push_back('\r'); break;
            case 't':  out.push_back('\t'); break;
            case 'u': {
                // \uXXXX -> UTF-8。够用版：只处理 BMP（不做代理对合并），
                // 配置里最多出现中文标点，够用了。
                if (i + 4 > s.size()) return Fail("\\u 后面不足 4 位十六进制");
                unsigned cp = 0;
                for (int k = 0; k < 4; ++k) {
                    const char h = s[i + k];
                    cp <<= 4;
                    if (h >= '0' && h <= '9')      cp |= (unsigned)(h - '0');
                    else if (h >= 'a' && h <= 'f') cp |= (unsigned)(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') cp |= (unsigned)(h - 'A' + 10);
                    else return Fail("\\u 后面不是十六进制");
                }
                i += 4;
                if (cp < 0x80) {
                    out.push_back((char)cp);
                } else if (cp < 0x800) {
                    out.push_back((char)(0xC0 | (cp >> 6)));
                    out.push_back((char)(0x80 | (cp & 0x3F)));
                } else {
                    out.push_back((char)(0xE0 | (cp >> 12)));
                    out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
                    out.push_back((char)(0x80 | (cp & 0x3F)));
                }
                break;
            }
            default: return Fail("无法识别的转义字符");
            }
        }
        return Fail("字符串没有收尾的引号");
    }

    bool ParseValueInner(Value& v)
    {
        SkipWs();
        if (i >= s.size()) return Fail("文件在这里就结束了（缺少值）");
        const char c = s[i];

        if (c == '{') {
            ++i;
            v.type = Value::Type::Object;
            SkipWs();
            if (i < s.size() && s[i] == '}') { ++i; return true; }
            for (;;) {
                SkipWs();
                std::string key;
                if (!ParseString(key)) return false;
                SkipWs();
                if (i >= s.size() || s[i] != ':') return Fail("键后面应该是冒号");
                ++i;
                Value child;
                if (!ParseValue(child)) return false;
                v.obj.emplace_back(std::move(key), std::move(child));
                SkipWs();
                if (i < s.size() && s[i] == ',') { ++i; continue; }
                if (i < s.size() && s[i] == '}') { ++i; return true; }
                return Fail("对象里缺少逗号或右花括号");
            }
        }

        if (c == '[') {
            ++i;
            v.type = Value::Type::Array;
            SkipWs();
            if (i < s.size() && s[i] == ']') { ++i; return true; }
            for (;;) {
                Value child;
                if (!ParseValue(child)) return false;
                v.arr.push_back(std::move(child));
                SkipWs();
                if (i < s.size() && s[i] == ',') { ++i; continue; }
                if (i < s.size() && s[i] == ']') { ++i; return true; }
                return Fail("数组里缺少逗号或右方括号");
            }
        }

        if (c == '"') {
            v.type = Value::Type::String;
            return ParseString(v.str);
        }

        if (Lit("true"))  { v.type = Value::Type::Bool; v.b = true;  return true; }
        if (Lit("false")) { v.type = Value::Type::Bool; v.b = false; return true; }
        if (Lit("null"))  { v.type = Value::Type::Null; return true; }

        // 数字
        {
            const size_t start = i;
            if (i < s.size() && (s[i] == '-' || s[i] == '+')) ++i;
            bool any = false;
            while (i < s.size() && ((s[i] >= '0' && s[i] <= '9') || s[i] == '.' ||
                                    s[i] == 'e' || s[i] == 'E' ||
                                    s[i] == '+' || s[i] == '-')) {
                ++i;
                any = true;
            }
            if (!any) return Fail("既不是对象/数组/字符串，也不是数字");
            v.type = Value::Type::Number;
            v.num  = std::atof(s.substr(start, i - start).c_str());
            return true;
        }
    }
};

} // namespace

bool Parse(const std::string& text, Value& out, std::string& err)
{
    out = Value{};
    Parser p(text);
    if (!p.ParseValue(out)) { err = p.err; return false; }
    p.SkipWs();
    if (p.i < text.size()) {
        err = "解析完一个值之后还有多余内容（是不是漏了逗号，或者多写了）";
        return false;
    }
    return true;
}

std::string Quote(const std::string& s)
{
    std::string out = "\"";
    for (unsigned char c : s) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:
            if (c < 0x20) {
                char buf[8];
                sprintf_s(buf, "\\u%04X", (unsigned)c);
                out += buf;
            } else {
                out += (char)c;   // UTF-8 原样写（文件本身就是 UTF-8）
            }
        }
    }
    out += "\"";
    return out;
}

} // namespace jsonlite
