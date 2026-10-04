// ============================================================================
// jsonlite.h —— 够用就好的 JSON 读/写（自研，零依赖）
// ----------------------------------------------------------------------------
// 为什么自己写：
//   「立规矩」要求嵌进 dock 的软件全部自研；而且配置文件是人手改的，
//   我们要的是"能读懂 + 能写回"，不需要通用库的完整语义。
//
// 支持的范围（刻意做小）：
//   读：对象 / 数组 / 字符串（含 \" \\ \n \t \uXXXX）/ 数字 / true / false / null
//   **额外容忍 `//` 与 `/* */` 注释** —— 手改配置时留一行说明是很自然的事，
//   而标准 JSON 不许注释。我们读得进、写出去仍是**干净的标准 JSON**
//   （注释只存在用户的编辑里，不往返）。
//   写：只提供"拼字符串"要用的转义与格式化辅助，不提供 DOM 序列化 ——
//   本项目的配置结构是固定的，直接手拼比套一层 DOM 更好读也更好调。
//
// ⚠️ 本文件刻意不做的事（别拿它当通用 JSON 库用）：
//   · 不做 UTF-8 与 \u 的完整往返（\u 只在读的时候解；写的时候中文直接原样写）
//   · 不保留键顺序（我们是按名字取值，不遍历）
//   · 不做超长/超深的防护之外的东西（有最大深度限制，防手改出错栈溢出）
// ============================================================================
#pragma once

#include <string>
#include <vector>
#include <map>

namespace jsonlite {

// ---------------------------- 读 ----------------------------
// 解析一棵树。失败时 ok=false 且 err 里是"人能看懂"的原因（含位置）。
struct Value;
using Object = std::vector<std::pair<std::string, Value>>;   // 保序，便于报错定位
using Array  = std::vector<Value>;

struct Value {
    enum class Type { Null, Bool, Number, String, Array, Object } type = Type::Null;
    bool        b   = false;
    double      num = 0;
    std::string str;
    Array       arr;
    Object      obj;

    bool IsNull()   const { return type == Type::Null; }
    bool IsObj()    const { return type == Type::Object; }
    bool IsArr()    const { return type == Type::Array; }
    bool IsStr()    const { return type == Type::String; }
    bool IsNum()    const { return type == Type::Number; }

    // 取子项。找不到/类型不对就返回默认值 —— 配置文件是"人改的东西"，
    // 少一个键、类型写错都不该让程序崩，够用即可（调用方按需再校验）。
    const Value* Find(const char* key) const;
    std::string  Str(const char* key, const std::string& def = "") const;
    double       Num(const char* key, double def = 0) const;
    int          Int(const char* key, int def = 0) const;
};

bool Parse(const std::string& text, Value& out, std::string& err);

// ---------------------------- 写 ----------------------------
// 把字符串转义成 JSON 字面量（含两端引号）。中文原样输出（文件按 UTF-8 存）。
std::string Quote(const std::string& s);

// 缩进辅助：整份文件按 2 空格缩进拼——手改起来舒服
inline std::string Indent(int level) { return std::string((size_t)level * 2, ' '); }

} // namespace jsonlite
