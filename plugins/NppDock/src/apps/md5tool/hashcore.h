// ============================================================================
// hashcore.h —— 常用摘要算法（自包含实现）
// ----------------------------------------------------------------------------
// 【为什么不用 Windows 自带的加密 API（bcrypt / CNG）】
//   1) 依赖清单要保持干净：本 exe 的 dumpbin /dependents 只应有
//      KERNEL32 / GDI32 / COMDLG32 / SHELL32 / USER32。调 CNG 会多出 bcrypt.dll，
//      也就多一个"目标机器上有没有 / 版本够不够"的变量。
//   2) 算法本身可读：这是一份能直接看懂的参考实现，日后要改动/移植都方便。
//   3) 覆盖够用：这里只需要"算一个摘要"，不需要流加密、密钥派生、证书那些东西。
//
// 【正确性怎么保证】
//   摘要算法是"算错也不会报错"的典型 —— 出错时照样吐一串十六进制，
//   肉眼完全分辨不出来。所以每一种算法都必须与权威实现对照，
//   见 tools/check_md5.py（对照 Python hashlib + zlib.crc32，可选 certutil 二次交叉）。
//   ⚠️ 常量表（SHA 的 K 表、初始 IV）只要抄错一个字符，结果就全错但不会崩 ——
//      所以**不要**凭记忆改这些表，改完必须跑一遍那个脚本。
//
// 【用法】
//     hashcore::Hasher h(hashcore::Algo::Sha256);
//     while (...) h.Update(buf, n);
//     std::string hex = h.HexDigest();      // 小写十六进制，长度见 AlgoHexLen()
//
// 本文件与实现同样适用于其他应用，放在 md5tool 目录下只是"就近"，
// 需要时整个挪走即可（不依赖本目录以外任何东西）。
// ============================================================================
#pragma once

#include <stddef.h>
#include <string>

namespace hashcore {

// ---------------------------------------------------------------------------
// 支持的算法
// ---------------------------------------------------------------------------
// 刻意只收"日常真的会遇到"的几种：
//   校验下载文件、核对镜像、贴给运维的口令清单 —— 这六种覆盖了绝大多数场合。
//   刻意**不收**的：MD4/ RIPEMD / SHA-224 / CRC16 / CRC64 —— 要么已淘汰，
//   要么只在极窄的领域出现，放进下拉框只会让人犹豫。
enum class Algo {
    Md5 = 0,
    Sha1,
    Sha256,
    Sha384,
    Sha512,
    Crc32,
};

int            AlgoCount();                 // 下拉框用：条目数
Algo           AlgoAt(int index);           // 下拉框用：第 index 项是哪个算法
const wchar_t* AlgoName(Algo a);            // L"MD5" / L"SHA-1" / … / L"CRC32"
size_t         AlgoHexLen(Algo a);          // 结果十六进制字符数：8/32/40/64/96/128

// 从命令行文本认算法（大小写不敏感，同时接受 sha256 与 sha-256 两种写法）
bool AlgoFromName(const wchar_t* name, Algo* out);

// ---------------------------------------------------------------------------
// 流式摘要器
// ---------------------------------------------------------------------------
// 为什么要流式：校验的文件可能有几个 GB，不能整个读进内存。
// 一次 Update 喂一块，最后 HexDigest() 收口。
class Hasher {
public:
    explicit Hasher(Algo a);
    ~Hasher();

    void        Update(const void* data, size_t len);
    std::string HexDigest();     // 可重复调用，结果一致

private:
    Hasher(const Hasher&);
    Hasher& operator=(const Hasher&);   // 禁止拷贝（内部持有上下文）

    Algo        _algo;
    void*       _impl;
    std::string _hex;
};

} // namespace hashcore
