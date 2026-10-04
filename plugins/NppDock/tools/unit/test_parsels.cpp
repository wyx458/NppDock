// 单元测试壳：把 packcore.cpp 整份 include 进来，于是静态的 ParseLs 在同一个
// 编译单元里可见 —— 用合成样本验证 B5（文件名解析健壮化）。
// 不属于产品代码，跑完即删（放在 _t/ 下）。
#include "../../src/apps/backpack/packcore.cpp"

#include <cstdio>

static int g_fail = 0;

static void check(bool ok, const char* what)
{
    printf("%s %s\n", ok ? "  OK  " : "[FAIL]", what);
    if (!ok) ++g_fail;
}

static const packcore::FileItem* find(const std::vector<packcore::FileItem>& v,
                                      const std::wstring& n)
{
    for (const auto& f : v) if (f.name == n) return &f;
    return nullptr;
}

int main()
{
    // 注意：每行都是 `权限 链接数 属主 属组 大小 时间戳 名字`
    std::string ls =
        "total 12\n"
        "-rw-r--r-- 1 root root 1234 1700000000 a  b.txt\n"          // 连续空格
        "-rw-r--r-- 1 root root 1234 1700000000 tail.txt \n"          // 名字以空格结尾
        "drwxr-xr-x 2 root root 4096 1700000000 sub\n"
        "lrwxrwxrwx 1 root root    7 1700000000 link -> target\n"
        "lrwxrwxrwx 1 root root    5 1700000000 a -> b -> c\n"        // 名字里也有 " -> "
        "prw-r--r-- 1 root root    0 1700000000 fifo\n"
        "srwxr-xr-x 1 root root    0 1700000000 sock\n"
        "crw-rw-rw- 1 root root    1, 3 1700000000 null\n"           // 设备号里带逗号
        "-rw-r--r-- 1 root root   12 1700000000 中文名 有空 格.txt\n"
        "--w------- 1 root root    0 1700000000 onlywrite\n"
        "brw-rw---- 1 root root    7, 0 1700000000 sda\n";

    std::vector<packcore::FileItem> v;
    ParseLs(ls, v);

    printf("--- 解析出 %d 项 ---\n", (int)v.size());
    for (const auto& f : v) {
        printf("  [%s%s%s] %ls%s\n",
               f.isDir ? "D" : "-", f.isLink ? "L" : "-", f.isOther ? "O" : "-",
               f.name.c_str(),
               f.linkTo.empty() ? "" : "");
        if (!f.linkTo.empty()) printf("        -> %ls\n", f.linkTo.c_str());
    }

    check(find(v, L"a  b.txt") != nullptr, "连续空格：名字里两个空格原样保留");
    check(find(v, L"a b.txt") == nullptr, "连续空格：没有被塌成一个空格");
    check(find(v, L"tail.txt ") != nullptr, "结尾空格：名字末尾那个空格没被吃掉");
    check(find(v, L"tail.txt") == nullptr, "结尾空格：没有被 trim 掉");
    check(find(v, L"中文名 有空 格.txt") != nullptr, "中文+空格：原样");

    const packcore::FileItem* sub = find(v, L"sub");
    check(sub && sub->isDir, "目录 d 被识别为目录");

    const packcore::FileItem* lk = find(v, L"link");
    check(lk && lk->isLink, "软链 l 被识别为符号链接（不再是普通文件）");
    check(lk && lk->linkTo == L"target", "软链目标被拆出来");

    const packcore::FileItem* ab = find(v, L"a -> b");
    check(ab && ab->isLink && ab->linkTo == L"c",
          "名字里也含 \" -> \" 时，按最后一次出现切分");

    check(find(v, L"fifo") != nullptr, "fifo 不再整行消失");
    check(find(v, L"sock") != nullptr, "socket 不再整行消失");
    check(find(v, L"null") != nullptr, "字符设备不再整行消失");
    check(find(v, L"sda") != nullptr, "块设备不再整行消失");
    check(find(v, L"fifo") && find(v, L"fifo")->isOther, "fifo 被标成『其它』");

    check(find(v, L"onlywrite") != nullptr, "权限以 --w 开头的普通文件也没被丢掉");
    check(v.size() >= 8, "总计至少 8 项（total 行被跳过）");

    printf("\n%s  失败 %d\n", g_fail ? "有问题：" : "全部通过.", g_fail);
    return g_fail ? 1 : 0;
}
