// 单元测试壳：直接调 packcore 的本地分支，验证
//   · A4 本地写改成"临时文件 + 原子替换"（内容对、无残留）
//   · A6 本地 Move 的行为基线（远端对齐它）
// 做法：把 packcore.cpp include 进来，用它自己的公开接口。
#include "../../src/apps/backpack/packcore.cpp"

#include <cstdio>

static int g_fail = 0;
static void check(bool ok, const char* what)
{
    printf("%s %s\n", ok ? "  OK  " : "[FAIL]", what);
    if (!ok) ++g_fail;
}

static bool ReadAll(const std::wstring& p, std::string& s)
{
    s.clear();
    HANDLE h = ::CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    char buf[4096];
    DWORD got = 0;
    while (::ReadFile(h, buf, sizeof(buf), &got, nullptr) && got > 0) s.append(buf, got);
    ::CloseHandle(h);
    return true;
}

static bool Exists(const std::wstring& p)
{
    return ::GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// 目录里还有没有我们的临时文件残留？（A4 的关键验收点）
static int CountTempLeftovers(const std::wstring& dir)
{
    WIN32_FIND_DATAW fd{};
    HANDLE h = ::FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int n = 0;
    do {
        if (packcore::IsTempName(fd.cFileName)) ++n;
    } while (::FindNextFileW(h, &fd));
    ::FindClose(h);
    return n;
}

int main()
{
    wchar_t tmp[MAX_PATH]{};
    ::GetTempPathW(MAX_PATH, tmp);
    const std::wstring root = std::wstring(tmp) + L"nppdpack_test_" +
                              std::to_wstring((unsigned long)::GetCurrentProcessId());
    ::CreateDirectoryW(root.c_str(), nullptr);

    packcore::Target t;
    t.local = true;
    t.localRoot = root;

    // ---- A4：便笺写入（本地）----
    const std::string note = "第一行\n第二行 with 空格\n";
    packcore::Res w = packcore::Write(t, L".nppbackpack-note.txt",
                                      std::string(note.c_str(), note.size()));
    check(Ok(w), "Write：写便笺返回成功");

    std::string back;
    check(ReadAll(root + L"\\.nppbackpack-note.txt", back), "Write：目标文件存在");
    check(back == note, "Write：内容逐字节一致（换行/空格都没变）");
    check(CountTempLeftovers(root) == 0, "Write：成功路径不留临时文件");

    // 覆盖写：旧内容必须被整体替换，而不是被追加
    packcore::Res w2 = packcore::Write(t, L".nppbackpack-note.txt", std::string("短"));
    std::string back2;
    ReadAll(root + L"\\.nppbackpack-note.txt", back2);
    check(Ok(w2) && back2 == std::string("短"), "Write：覆盖写是整体替换（没被追加）");
    check(CountTempLeftovers(root) == 0, "Write：覆盖写也不留临时文件");

    // ---- A6：Move（本地）----
    packcore::Write(t, L"a.txt", std::string("AAA"));
    packcore::Write(t, L"b.txt", std::string("BBB"));

    packcore::Res m1 = packcore::Move(t, L"b.txt", L"a.txt");
    std::string aContent;
    ReadAll(root + L"\\a.txt", aContent);
    check(!Ok(m1), "Move：目标已存在 → 报错（不覆盖）");
    check(aContent == std::string("AAA"), "Move：目标已存在时，a.txt 内容没被改");
    check(Exists(root + L"\\b.txt"), "Move：失败后源文件还在");

    packcore::Res m2 = packcore::Move(t, L"b.txt", L"c.txt");
    std::string cContent;
    ReadAll(root + L"\\c.txt", cContent);
    check(Ok(m2) && cContent == std::string("BBB"), "Move：目标不存在 → 正常改名");
    check(!Exists(root + L"\\b.txt"), "Move：改名后源文件没了");

    packcore::Res m3 = packcore::Move(t, L"c.txt", L"c.txt");
    check(Ok(m3), "Move：源与目标同名 → 视为无操作成功（不报『已存在』）");

    // ---- A4 的反面：失败路径不留垃圾（用一个非法目标名制造失败）----
    packcore::Res w3 = packcore::Write(t, L"sub\\nope.txt", std::string("x"));
    check(!Ok(w3), "Write：目标目录不存在 → 报错");
    check(CountTempLeftovers(root) == 0, "Write：失败路径也不留临时文件");

    // ---- Probe 会顺手清掉残留 ----
    // 两种形态都造一个：普通名 + 以点开头的名（便笺就是后者）
    for (const wchar_t* n : { L"x.nppbackpack-tmp-999",
                              L".nppbackpack-note.txt.nppbackpack-tmp-999" }) {
        HANDLE h = ::CreateFileW((root + L"\\" + n).c_str(), GENERIC_WRITE, 0,
                                 nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) ::CloseHandle(h);   // ⚠️ 不关句柄就删不掉（共享冲突）
    }
    check(CountTempLeftovers(root) == 2, "造两个残留的临时文件（普通名 + 点开头）");
    packcore::Res pr = packcore::Probe(t);
    check(Ok(pr), "Probe（本地）返回成功");
    check(CountTempLeftovers(root) == 0, "Probe：两种形态的残留都被清掉了");

    printf("\n%s  失败 %d\n", g_fail ? "有问题：" : "全部通过.", g_fail);
    return g_fail ? 1 : 0;
}
