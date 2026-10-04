// 单元测试壳：验证 A2 —— 输出到上限后**丢最旧的整行、保住尾巴**。
// 老实现是"超了就直接 return"，于是后面的统计行、"（已终止）"全被无声吃掉。
// 这条没法从界面驱动（要造 40 万字符的输出），所以直接把 nettest.cpp include
// 进来、喂它一堆合成行。
#include "../../src/apps/nettest/nettest.cpp"

#include <cstdio>

static int g_fail = 0;
static void check(bool ok, const char* what)
{
    printf("%s %s\n", ok ? "  OK  " : "[FAIL]", what);
    if (!ok) ++g_fail;
}

int main()
{
    const size_t cap0 = kMaxOutChars;

    for (int i = 0; i < 40000; ++i) {
        wchar_t b[96];
        swprintf_s(b, L"[%05d] 一行普普通通的输出，用来把上限撑满 padding", i);
        AppendLine(b);
    }

    printf("--- 缓冲区 %zu 字符，上限 %zu，已省略 %d 行 ---\n",
           g_outBuf.size(), cap0, g_omittedLines);

    check(g_outBuf.size() <= cap0, "缓冲区不超过上限（不会无限吃内存）");
    check(g_omittedLines > 0, "确实发生了省略，而且计数不是 0（以前那句提示永远不会出现）");
    check(g_outBuf.find(L"[39999]") != std::wstring::npos, "最新一行还在（尾巴保住了）");
    check(g_outBuf.find(L"[00000]") == std::wstring::npos, "最旧的行已经被丢掉（丢的是头部）");

    // 关键：到上限之后**再追加**的那几行必须留下（统计行、"（已终止）"就是这一类）
    AppendLine(L"");
    AppendLine(L"===== 最终统计 =====");
    AppendLine(L"（已终止）");
    check(g_outBuf.find(L"===== 最终统计 =====") != std::wstring::npos,
          "到上限之后再追加的统计行仍然保留");
    check(g_outBuf.find(L"（已终止）") != std::wstring::npos,
          "到上限之后再追加的『（已终止）』仍然保留");
    check(g_outBuf.size() <= cap0, "追加之后仍然不超上限");

    // 只丢掉整行 —— 不能出现"半行"（第一行应当是完整的 [xxxxx] 开头）
    {
        const size_t firstLineEnd = g_outBuf.find(L"\r\n");
        const std::wstring first = g_outBuf.substr(0, firstLineEnd);
        check(first.rfind(L"[", 0) == 0 && first.size() > 8,
              "缓冲区第一行是**完整的整行**（不是被切了一半的残句）");
    }

    // 单行就超上限的极端情况
    {
        g_outBuf.clear();
        g_omittedLines = 0;
        AppendLine(std::wstring(cap0 + 1000, L'x'));
        check(g_outBuf.size() <= cap0, "单行超上限时也不会把内存撑爆");
    }

    printf("\n%s  失败 %d\n", g_fail ? "有问题：" : "全部通过.", g_fail);
    return g_fail ? 1 : 0;
}
