// ============================================================================
// hashcore.cpp —— 摘要算法实现（MD5 / SHA-1 / SHA-256 / SHA-384 / SHA-512 / CRC32）
// ----------------------------------------------------------------------------
// 全部自包含，只用到 <string.h>。规范出处：
//   MD5     RFC 1321
//   SHA-1   RFC 3174
//   SHA-2   FIPS 180-4（SHA-256 / SHA-384 / SHA-512 共用一套消息扩展与轮函数）
//   CRC32   IEEE 802.3（反射多项式 0xEDB88320，等价于 zlib.crc32）
//
// 【实现上统一的三个写法，避免各算法各写一套容易错的东西】
//   1) 先攒满一个整块再压缩：Update() 只管往 _buf 里填，填满 64/128 字节就压缩一次。
//   2) 收尾（补位 + 长度）在 Digest() 里做，并且**用状态的副本**算 ——
//      这样 HexDigest() 重复调用不会把状态算坏。
//   3) 长度一律用 64 位字节计数器累加，收尾时再换算成比特；
//      MD5 的长度字段是**小端**，SHA 系列是**大端**，这一点很容易写反。
// ============================================================================

#include "hashcore.h"

#include <string.h>

namespace hashcore {
namespace {

// ---------------------------------------------------------------------------
// 位运算与字节序
// ---------------------------------------------------------------------------
inline unsigned int Rotl32(unsigned int x, int n) { return (x << n) | (x >> (32 - n)); }
inline unsigned int Rotr32(unsigned int x, int n) { return (x >> n) | (x << (32 - n)); }
inline unsigned long long Rotr64(unsigned long long x, int n) {
    return (x >> n) | (x << (64 - n));
}

inline unsigned int Be32(const unsigned char* p) {
    return ((unsigned int)p[0] << 24) | ((unsigned int)p[1] << 16)
         | ((unsigned int)p[2] << 8)  | (unsigned int)p[3];
}
inline unsigned long long Be64(const unsigned char* p) {
    unsigned long long v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | (unsigned long long)p[i];
    return v;
}

inline std::string HexOf(const unsigned char* d, size_t n)
{
    static const char kHex[] = "0123456789abcdef";
    std::string s(n * 2, '0');
    for (size_t i = 0; i < n; ++i) {
        s[i * 2]     = kHex[(d[i] >> 4) & 0x0F];
        s[i * 2 + 1] = kHex[d[i] & 0x0F];
    }
    return s;
}

// ---------------------------------------------------------------------------
// 流式接口：各算法实现它，Hasher 只做转发
// ---------------------------------------------------------------------------
class Impl {
public:
    virtual ~Impl() {}
    virtual void        Update(const unsigned char* data, size_t len) = 0;
    virtual std::string Digest() = 0;
};

// ===========================================================================
// MD5（RFC 1321）
// ===========================================================================
void Md5Transform(unsigned int state[4], const unsigned char block[64])
{
    unsigned int a = state[0], b = state[1], c = state[2], d = state[3];
    unsigned int x[16];

    for (int i = 0, j = 0; i < 16; ++i, j += 4) {
        x[i] = ((unsigned int)block[j])
             | ((unsigned int)block[j + 1] << 8)
             | ((unsigned int)block[j + 2] << 16)
             | ((unsigned int)block[j + 3] << 24);
    }

    auto F = [](unsigned int u, unsigned int v, unsigned int w) { return (u & v) | (~u & w); };
    auto G = [](unsigned int u, unsigned int v, unsigned int w) { return (u & w) | (v & ~w); };
    auto H = [](unsigned int u, unsigned int v, unsigned int w) { return u ^ v ^ w; };
    auto I = [](unsigned int u, unsigned int v, unsigned int w) { return v ^ (u | ~w); };

    // 展开成一个通用宏，避免 64 行手抄出错
    #define STEP(fn, a, b, c, d, k, s, t) \
        (a) = (b) + Rotl32((a) + fn((b), (c), (d)) + x[(k)] + (unsigned int)(t), (s))

    STEP(F, a, b, c, d,  0,  7, 0xd76aa478);
    STEP(F, d, a, b, c,  1, 12, 0xe8c7b756);
    STEP(F, c, d, a, b,  2, 17, 0x242070db);
    STEP(F, b, c, d, a,  3, 22, 0xc1bdceee);
    STEP(F, a, b, c, d,  4,  7, 0xf57c0faf);
    STEP(F, d, a, b, c,  5, 12, 0x4787c62a);
    STEP(F, c, d, a, b,  6, 17, 0xa8304613);
    STEP(F, b, c, d, a,  7, 22, 0xfd469501);
    STEP(F, a, b, c, d,  8,  7, 0x698098d8);
    STEP(F, d, a, b, c,  9, 12, 0x8b44f7af);
    STEP(F, c, d, a, b, 10, 17, 0xffff5bb1);
    STEP(F, b, c, d, a, 11, 22, 0x895cd7be);
    STEP(F, a, b, c, d, 12,  7, 0x6b901122);
    STEP(F, d, a, b, c, 13, 12, 0xfd987193);
    STEP(F, c, d, a, b, 14, 17, 0xa679438e);
    STEP(F, b, c, d, a, 15, 22, 0x49b40821);

    STEP(G, a, b, c, d,  1,  5, 0xf61e2562);
    STEP(G, d, a, b, c,  6,  9, 0xc040b340);
    STEP(G, c, d, a, b, 11, 14, 0x265e5a51);
    STEP(G, b, c, d, a,  0, 20, 0xe9b6c7aa);
    STEP(G, a, b, c, d,  5,  5, 0xd62f105d);
    STEP(G, d, a, b, c, 10,  9, 0x02441453);
    STEP(G, c, d, a, b, 15, 14, 0xd8a1e681);
    STEP(G, b, c, d, a,  4, 20, 0xe7d3fbc8);
    STEP(G, a, b, c, d,  9,  5, 0x21e1cde6);
    STEP(G, d, a, b, c, 14,  9, 0xc33707d6);
    STEP(G, c, d, a, b,  3, 14, 0xf4d50d87);
    STEP(G, b, c, d, a,  8, 20, 0x455a14ed);
    STEP(G, a, b, c, d, 13,  5, 0xa9e3e905);
    STEP(G, d, a, b, c,  2,  9, 0xfcefa3f8);
    STEP(G, c, d, a, b,  7, 14, 0x676f02d9);
    STEP(G, b, c, d, a, 12, 20, 0x8d2a4c8a);

    STEP(H, a, b, c, d,  5,  4, 0xfffa3942);
    STEP(H, d, a, b, c,  8, 11, 0x8771f681);
    STEP(H, c, d, a, b, 11, 16, 0x6d9d6122);
    STEP(H, b, c, d, a, 14, 23, 0xfde5380c);
    STEP(H, a, b, c, d,  1,  4, 0xa4beea44);
    STEP(H, d, a, b, c,  4, 11, 0x4bdecfa9);
    STEP(H, c, d, a, b,  7, 16, 0xf6bb4b60);
    STEP(H, b, c, d, a, 10, 23, 0xbebfbc70);
    STEP(H, a, b, c, d, 13,  4, 0x289b7ec6);
    STEP(H, d, a, b, c,  0, 11, 0xeaa127fa);
    STEP(H, c, d, a, b,  3, 16, 0xd4ef3085);
    STEP(H, b, c, d, a,  6, 23, 0x04881d05);
    STEP(H, a, b, c, d,  9,  4, 0xd9d4d039);
    STEP(H, d, a, b, c, 12, 11, 0xe6db99e5);
    STEP(H, c, d, a, b, 15, 16, 0x1fa27cf8);
    STEP(H, b, c, d, a,  2, 23, 0xc4ac5665);

    STEP(I, a, b, c, d,  0,  6, 0xf4292244);
    STEP(I, d, a, b, c,  7, 10, 0x432aff97);
    STEP(I, c, d, a, b, 14, 15, 0xab9423a7);
    STEP(I, b, c, d, a,  5, 21, 0xfc93a039);
    STEP(I, a, b, c, d, 12,  6, 0x655b59c3);
    STEP(I, d, a, b, c,  3, 10, 0x8f0ccc92);
    STEP(I, c, d, a, b, 10, 15, 0xffeff47d);
    STEP(I, b, c, d, a,  1, 21, 0x85845dd1);
    STEP(I, a, b, c, d,  8,  6, 0x6fa87e4f);
    STEP(I, d, a, b, c, 15, 10, 0xfe2ce6e0);
    STEP(I, c, d, a, b,  6, 15, 0xa3014314);
    STEP(I, b, c, d, a, 13, 21, 0x4e0811a1);
    STEP(I, a, b, c, d,  4,  6, 0xf7537e82);
    STEP(I, d, a, b, c, 11, 10, 0xbd3af235);
    STEP(I, c, d, a, b,  2, 15, 0x2ad7d2bb);
    STEP(I, b, c, d, a,  9, 21, 0xeb86d391);

    #undef STEP

    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
}

class Md5Impl : public Impl {
public:
    Md5Impl() : _buflen(0), _total(0) {
        _state[0] = 0x67452301; _state[1] = 0xefcdab89;
        _state[2] = 0x98badcfe; _state[3] = 0x10325476;
    }

    void Update(const unsigned char* data, size_t len) override {
        _total += len;
        while (len > 0) {
            size_t take = 64 - _buflen;
            if (take > len) take = len;
            memcpy(_buf + _buflen, data, take);
            _buflen += take; data += take; len -= take;
            if (_buflen == 64) { Md5Transform(_state, _buf); _buflen = 0; }
        }
    }

    std::string Digest() override {
        unsigned int st[4];
        memcpy(st, _state, sizeof(st));

        // 补位：0x80 + 若干 0，留最后 8 字节放长度。
        // 当前块剩余不足 56 字节时只需补一个块，否则要补两个。
        unsigned char block[128] = { 0 };
        memcpy(block, _buf, _buflen);
        block[_buflen] = 0x80;
        const size_t blocks = (_buflen < 56) ? 1 : 2;

        // ⚠️ MD5 的长度字段是**小端**（SHA 系列是大端，别抄错）
        unsigned long long bits = _total * 8;
        size_t off = blocks * 64 - 8;
        for (int i = 0; i < 8; ++i)
            block[off + i] = (unsigned char)((bits >> (8 * i)) & 0xFF);

        Md5Transform(st, block);
        if (blocks == 2) Md5Transform(st, block + 64);

        unsigned char d[16];
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
                d[i * 4 + j] = (unsigned char)((st[i] >> (8 * j)) & 0xFF);
        return HexOf(d, 16);
    }

private:
    unsigned int       _state[4];
    unsigned long long _total;
    unsigned char      _buf[64];
    size_t             _buflen;
};

// ===========================================================================
// SHA-1（RFC 3174）
// ===========================================================================
void Sha1Block(unsigned int h[5], const unsigned char* p)
{
    unsigned int w[80];
    for (int i = 0; i < 16; ++i) w[i] = Be32(p + i * 4);
    for (int i = 16; i < 80; ++i)
        w[i] = Rotl32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

    unsigned int a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; ++i) {
        unsigned int f, k;
        if      (i < 20) { f = (b & c) | (~b & d);          k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d;                   k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
        else             { f = b ^ c ^ d;                   k = 0xCA62C1D6; }
        unsigned int t = Rotl32(a, 5) + f + e + k + w[i];
        e = d; d = c; c = Rotl32(b, 30); b = a; a = t;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}

class Sha1Impl : public Impl {
public:
    Sha1Impl() : _buflen(0), _total(0) {
        _h[0] = 0x67452301; _h[1] = 0xEFCDAB89; _h[2] = 0x98BADCFE;
        _h[3] = 0x10325476; _h[4] = 0xC3D2E1F0;
    }

    void Update(const unsigned char* data, size_t len) override {
        _total += len;
        while (len > 0) {
            size_t take = 64 - _buflen;
            if (take > len) take = len;
            memcpy(_buf + _buflen, data, take);
            _buflen += take; data += take; len -= take;
            if (_buflen == 64) { Sha1Block(_h, _buf); _buflen = 0; }
        }
    }

    std::string Digest() override {
        unsigned int h[5];
        memcpy(h, _h, sizeof(h));

        unsigned char block[128] = { 0 };
        memcpy(block, _buf, _buflen);
        block[_buflen] = 0x80;
        const size_t blocks = (_buflen < 56) ? 1 : 2;

        // SHA 系列长度字段**大端**
        unsigned long long bits = _total * 8;
        size_t off = blocks * 64 - 8;
        for (int i = 0; i < 8; ++i)
            block[off + i] = (unsigned char)((bits >> (8 * (7 - i))) & 0xFF);

        Sha1Block(h, block);
        if (blocks == 2) Sha1Block(h, block + 64);

        unsigned char d[20];
        for (int i = 0; i < 5; ++i) {
            d[i * 4 + 0] = (unsigned char)(h[i] >> 24);
            d[i * 4 + 1] = (unsigned char)(h[i] >> 16);
            d[i * 4 + 2] = (unsigned char)(h[i] >> 8);
            d[i * 4 + 3] = (unsigned char)(h[i]);
        }
        return HexOf(d, 20);
    }

private:
    unsigned int       _h[5];
    unsigned long long _total;
    unsigned char      _buf[64];
    size_t             _buflen;
};

// ===========================================================================
// SHA-256 / SHA-224（FIPS 180-4）
// ===========================================================================
// K 表：前 64 个素数的立方根小数部分前 32 位。
// ⚠️ 这张表**不要凭记忆改**，抄错一个字符结果就全错（而且不会崩）。
const unsigned int kSha256K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

void Sha256Block(unsigned int h[8], const unsigned char* p)
{
    unsigned int w[64];
    for (int i = 0; i < 16; ++i) w[i] = Be32(p + i * 4);
    for (int i = 16; i < 64; ++i) {
        unsigned int s0 = Rotr32(w[i - 15], 7) ^ Rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        unsigned int s1 = Rotr32(w[i - 2], 17) ^ Rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    unsigned int a = h[0], b = h[1], c = h[2], d = h[3];
    unsigned int e = h[4], f = h[5], g = h[6], hh = h[7];

    for (int i = 0; i < 64; ++i) {
        unsigned int S1 = Rotr32(e, 6) ^ Rotr32(e, 11) ^ Rotr32(e, 25);
        unsigned int ch = (e & f) ^ (~e & g);
        unsigned int t1 = hh + S1 + ch + kSha256K[i] + w[i];
        unsigned int S0 = Rotr32(a, 2) ^ Rotr32(a, 13) ^ Rotr32(a, 22);
        unsigned int mj = (a & b) ^ (a & c) ^ (b & c);
        unsigned int t2 = S0 + mj;
        hh = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    h[0] += a; h[1] += b; h[2] += c; h[3] += d;
    h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

class Sha256Impl : public Impl {
public:
    Sha256Impl() : _buflen(0), _total(0) {
        static const unsigned int kIv[8] = {
            0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
            0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
        };
        memcpy(_h, kIv, sizeof(_h));
    }

    void Update(const unsigned char* data, size_t len) override {
        _total += len;
        while (len > 0) {
            size_t take = 64 - _buflen;
            if (take > len) take = len;
            memcpy(_buf + _buflen, data, take);
            _buflen += take; data += take; len -= take;
            if (_buflen == 64) { Sha256Block(_h, _buf); _buflen = 0; }
        }
    }

    std::string Digest() override {
        unsigned int h[8];
        memcpy(h, _h, sizeof(h));

        unsigned char block[128] = { 0 };
        memcpy(block, _buf, _buflen);
        block[_buflen] = 0x80;
        const size_t blocks = (_buflen < 56) ? 1 : 2;

        unsigned long long bits = _total * 8;
        size_t off = blocks * 64 - 8;
        for (int i = 0; i < 8; ++i)
            block[off + i] = (unsigned char)((bits >> (8 * (7 - i))) & 0xFF);

        Sha256Block(h, block);
        if (blocks == 2) Sha256Block(h, block + 64);

        unsigned char d[32];
        for (int i = 0; i < 8; ++i) {
            d[i * 4 + 0] = (unsigned char)(h[i] >> 24);
            d[i * 4 + 1] = (unsigned char)(h[i] >> 16);
            d[i * 4 + 2] = (unsigned char)(h[i] >> 8);
            d[i * 4 + 3] = (unsigned char)(h[i]);
        }
        return HexOf(d, 32);
    }

private:
    unsigned int       _h[8];
    unsigned long long _total;
    unsigned char      _buf[64];
    size_t             _buflen;
};

// ===========================================================================
// SHA-512 / SHA-384（FIPS 180-4）
// ---------------------------------------------------------------------------
// 与 SHA-256 是同一套结构，只是：字长 64 位、块 128 字节、80 轮、长度字段 128 位。
// SHA-384 = SHA-512 换一套初始值 + 只取前 6 个字（48 字节）。
// ===========================================================================
const unsigned long long kSha512K[80] = {
    0x428a2f98d728ae22ull, 0x7137449123ef65cdull, 0xb5c0fbcfec4d3b2full,
    0xe9b5dba58189dbbcull, 0x3956c25bf348b538ull, 0x59f111f1b605d019ull,
    0x923f82a4af194f9bull, 0xab1c5ed5da6d8118ull, 0xd807aa98a3030242ull,
    0x12835b0145706fbeull, 0x243185be4ee4b28cull, 0x550c7dc3d5ffb4e2ull,
    0x72be5d74f27b896full, 0x80deb1fe3b1696b1ull, 0x9bdc06a725c71235ull,
    0xc19bf174cf692694ull, 0xe49b69c19ef14ad2ull, 0xefbe4786384f25e3ull,
    0x0fc19dc68b8cd5b5ull, 0x240ca1cc77ac9c65ull, 0x2de92c6f592b0275ull,
    0x4a7484aa6ea6e483ull, 0x5cb0a9dcbd41fbd4ull, 0x76f988da831153b5ull,
    0x983e5152ee66dfabull, 0xa831c66d2db43210ull, 0xb00327c898fb213full,
    0xbf597fc7beef0ee4ull, 0xc6e00bf33da88fc2ull, 0xd5a79147930aa725ull,
    0x06ca6351e003826full, 0x142929670a0e6e70ull, 0x27b70a8546d22ffcull,
    0x2e1b21385c26c926ull, 0x4d2c6dfc5ac42aedull, 0x53380d139d95b3dfull,
    0x650a73548baf63deull, 0x766a0abb3c77b2a8ull, 0x81c2c92e47edaee6ull,
    0x92722c851482353bull, 0xa2bfe8a14cf10364ull, 0xa81a664bbc423001ull,
    0xc24b8b70d0f89791ull, 0xc76c51a30654be30ull, 0xd192e819d6ef5218ull,
    0xd69906245565a910ull, 0xf40e35855771202aull, 0x106aa07032bbd1b8ull,
    0x19a4c116b8d2d0c8ull, 0x1e376c085141ab53ull, 0x2748774cdf8eeb99ull,
    0x34b0bcb5e19b48a8ull, 0x391c0cb3c5c95a63ull, 0x4ed8aa4ae3418acbull,
    0x5b9cca4f7763e373ull, 0x682e6ff3d6b2b8a3ull, 0x748f82ee5defb2fcull,
    0x78a5636f43172f60ull, 0x84c87814a1f0ab72ull, 0x8cc702081a6439ecull,
    0x90befffa23631e28ull, 0xa4506cebde82bde9ull, 0xbef9a3f7b2c67915ull,
    0xc67178f2e372532bull, 0xca273eceea26619cull, 0xd186b8c721c0c207ull,
    0xeada7dd6cde0eb1eull, 0xf57d4f7fee6ed178ull, 0x06f067aa72176fbaull,
    0x0a637dc5a2c898a6ull, 0x113f9804bef90daeull, 0x1b710b35131c471bull,
    0x28db77f523047d84ull, 0x32caab7b40c72493ull, 0x3c9ebe0a15c9bebcull,
    0x431d67c49c100d4cull, 0x4cc5d4becb3e42b6ull, 0x597f299cfc657e2aull,
    0x5fcb6fab3ad6faecull, 0x6c44198c4a475817ull,
};

void Sha512Block(unsigned long long h[8], const unsigned char* p)
{
    unsigned long long w[80];
    for (int i = 0; i < 16; ++i) w[i] = Be64(p + i * 8);
    for (int i = 16; i < 80; ++i) {
        unsigned long long s0 = Rotr64(w[i - 15], 1) ^ Rotr64(w[i - 15], 8) ^ (w[i - 15] >> 7);
        unsigned long long s1 = Rotr64(w[i - 2], 19) ^ Rotr64(w[i - 2], 61) ^ (w[i - 2] >> 6);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    unsigned long long a = h[0], b = h[1], c = h[2], d = h[3];
    unsigned long long e = h[4], f = h[5], g = h[6], hh = h[7];

    for (int i = 0; i < 80; ++i) {
        unsigned long long S1 = Rotr64(e, 14) ^ Rotr64(e, 18) ^ Rotr64(e, 41);
        unsigned long long ch = (e & f) ^ (~e & g);
        unsigned long long t1 = hh + S1 + ch + kSha512K[i] + w[i];
        unsigned long long S0 = Rotr64(a, 28) ^ Rotr64(a, 34) ^ Rotr64(a, 39);
        unsigned long long mj = (a & b) ^ (a & c) ^ (b & c);
        unsigned long long t2 = S0 + mj;
        hh = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    h[0] += a; h[1] += b; h[2] += c; h[3] += d;
    h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

class Sha512Impl : public Impl {
public:
    explicit Sha512Impl(bool is384) : _is384(is384), _buflen(0), _total(0) {
        static const unsigned long long kIv512[8] = {
            0x6a09e667f3bcc908ull, 0xbb67ae8584caa73bull, 0x3c6ef372fe94f82bull,
            0xa54ff53a5f1d36f1ull, 0x510e527fade682d1ull, 0x9b05688c2b3e6c1full,
            0x1f83d9abfb41bd6bull, 0x5be0cd19137e2179ull,
        };
        static const unsigned long long kIv384[8] = {
            0xcbbb9d5dc1059ed8ull, 0x629a292a367cd507ull, 0x9159015a3070dd17ull,
            0x152fecd8f70e5939ull, 0x67332667ffc00b31ull, 0x8eb44a8768581511ull,
            0xdb0c2e0d64f98fa7ull, 0x47b5481dbefa4fa4ull,
        };
        memcpy(_h, is384 ? kIv384 : kIv512, sizeof(_h));
    }

    void Update(const unsigned char* data, size_t len) override {
        _total += len;
        while (len > 0) {
            size_t take = 128 - _buflen;
            if (take > len) take = len;
            memcpy(_buf + _buflen, data, take);
            _buflen += take; data += take; len -= take;
            if (_buflen == 128) { Sha512Block(_h, _buf); _buflen = 0; }
        }
    }

    std::string Digest() override {
        unsigned long long h[8];
        memcpy(h, _h, sizeof(h));

        // 补位门槛是 112（128 - 16），长度字段占 16 字节
        unsigned char block[256] = { 0 };
        memcpy(block, _buf, _buflen);
        block[_buflen] = 0x80;
        const size_t blocks = (_buflen < 112) ? 1 : 2;

        // 128 位长度字段：高 64 位在绝大多数场合恒为 0（文件不可能有 2^61 字节），
        // 但按规范照样写出来，避免"只在超大文件上出错"这种隐患。
        unsigned long long hi = _total >> 61;
        unsigned long long lo = _total << 3;
        size_t off = blocks * 128 - 16;
        for (int i = 0; i < 8; ++i)
            block[off + i] = (unsigned char)((hi >> (8 * (7 - i))) & 0xFF);
        for (int i = 0; i < 8; ++i)
            block[off + 8 + i] = (unsigned char)((lo >> (8 * (7 - i))) & 0xFF);

        Sha512Block(h, block);
        if (blocks == 2) Sha512Block(h, block + 128);

        const int nWords = _is384 ? 6 : 8;      // SHA-384 截断到前 6 个字
        unsigned char d[64];
        for (int i = 0; i < nWords; ++i) {
            d[i * 8 + 0] = (unsigned char)(h[i] >> 56);
            d[i * 8 + 1] = (unsigned char)(h[i] >> 48);
            d[i * 8 + 2] = (unsigned char)(h[i] >> 40);
            d[i * 8 + 3] = (unsigned char)(h[i] >> 32);
            d[i * 8 + 4] = (unsigned char)(h[i] >> 24);
            d[i * 8 + 5] = (unsigned char)(h[i] >> 16);
            d[i * 8 + 6] = (unsigned char)(h[i] >> 8);
            d[i * 8 + 7] = (unsigned char)(h[i]);
        }
        return HexOf(d, (size_t)nWords * 8);
    }

private:
    bool               _is384;
    unsigned long long _h[8];
    unsigned long long _total;
    unsigned char      _buf[128];
    size_t             _buflen;
};

// ===========================================================================
// CRC32（IEEE 802.3，反射多项式 0xEDB88320）
// ---------------------------------------------------------------------------
// 与 zlib.crc32 等价：初值全 1，逐字节反射查表，末了再取反。
//
// 表**运行期生成**（256 项）而不是写死 256 个常量：
//   写死 256 行是纯手抄，抄错一个照样"能跑但结果错"，而且没人会去逐行核对。
//   生成只需要 256×8 次移位，一次性的开销可以忽略。
// ===========================================================================
unsigned int* Crc32Table()
{
    static unsigned int table[256];
    static bool built = false;
    if (!built) {
        for (unsigned int i = 0; i < 256; ++i) {
            unsigned int c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        built = true;
    }
    return table;
}

class Crc32Impl : public Impl {
public:
    Crc32Impl() : _crc(0xFFFFFFFFu), _total(0) {}

    void Update(const unsigned char* data, size_t len) override {
        const unsigned int* t = Crc32Table();
        unsigned int c = _crc;
        for (size_t i = 0; i < len; ++i)
            c = t[(c ^ data[i]) & 0xFF] ^ (c >> 8);
        _crc = c;
        _total += len;
    }

    std::string Digest() override {
        unsigned int v = _crc ^ 0xFFFFFFFFu;
        unsigned char d[4];
        d[0] = (unsigned char)(v >> 24);
        d[1] = (unsigned char)(v >> 16);
        d[2] = (unsigned char)(v >> 8);
        d[3] = (unsigned char)(v);
        return HexOf(d, 4);
    }

private:
    unsigned int       _crc;
    unsigned long long _total;   // 保留（将来若加 CRC64 会用到），当前不参与输出
};

} // namespace

// ===========================================================================
// 对外接口
// ===========================================================================
int AlgoCount() { return 6; }

Algo AlgoAt(int index)
{
    // 下拉框显示顺序 = 日常使用频率从高到低
    static const Algo kOrder[] = {
        Algo::Md5, Algo::Sha1, Algo::Sha256,
        Algo::Sha384, Algo::Sha512, Algo::Crc32,
    };
    if (index < 0 || index >= (int)(sizeof(kOrder) / sizeof(kOrder[0])))
        return Algo::Md5;
    return kOrder[index];
}

const wchar_t* AlgoName(Algo a)
{
    switch (a) {
    case Algo::Md5:    return L"MD5";
    case Algo::Sha1:   return L"SHA-1";
    case Algo::Sha256: return L"SHA-256";
    case Algo::Sha384: return L"SHA-384";
    case Algo::Sha512: return L"SHA-512";
    case Algo::Crc32:  return L"CRC32";
    }
    return L"MD5";
}

size_t AlgoHexLen(Algo a)
{
    switch (a) {
    case Algo::Md5:    return 32;
    case Algo::Sha1:   return 40;
    case Algo::Sha256: return 64;
    case Algo::Sha384: return 96;
    case Algo::Sha512: return 128;
    case Algo::Crc32:  return 8;
    }
    return 32;
}

bool AlgoFromName(const wchar_t* name, Algo* out)
{
    if (!name || !*name) return false;

    struct Pair { const wchar_t* a; const wchar_t* b; Algo algo; };
    static const Pair kMap[] = {
        { L"md5",    L"md5",     Algo::Md5    },
        { L"sha1",   L"sha-1",   Algo::Sha1   },
        { L"sha256", L"sha-256", Algo::Sha256 },
        { L"sha384", L"sha-384", Algo::Sha384 },
        { L"sha512", L"sha-512", Algo::Sha512 },
        { L"crc32",  L"crc32",   Algo::Crc32  },
    };
    for (int i = 0; i < (int)(sizeof(kMap) / sizeof(kMap[0])); ++i) {
        if (_wcsicmp(name, kMap[i].a) == 0 || _wcsicmp(name, kMap[i].b) == 0) {
            if (out) *out = kMap[i].algo;
            return true;
        }
    }
    return false;
}

Hasher::Hasher(Algo a) : _algo(a), _impl(nullptr)
{
    Impl* impl = nullptr;
    switch (a) {
    case Algo::Md5:    impl = new Md5Impl();          break;
    case Algo::Sha1:   impl = new Sha1Impl();         break;
    case Algo::Sha256: impl = new Sha256Impl();       break;
    case Algo::Sha384: impl = new Sha512Impl(true);   break;
    case Algo::Sha512: impl = new Sha512Impl(false);  break;
    case Algo::Crc32:  impl = new Crc32Impl();        break;
    }
    if (!impl) impl = new Md5Impl();
    _impl = impl;
}

Hasher::~Hasher()
{
    delete static_cast<Impl*>(_impl);
    _impl = nullptr;
}

void Hasher::Update(const void* data, size_t len)
{
    if (_impl && data && len)
        static_cast<Impl*>(_impl)->Update(static_cast<const unsigned char*>(data), len);
}

std::string Hasher::HexDigest()
{
    if (_hex.empty() && _impl)
        _hex = static_cast<Impl*>(_impl)->Digest();
    return _hex;
}

} // namespace hashcore
