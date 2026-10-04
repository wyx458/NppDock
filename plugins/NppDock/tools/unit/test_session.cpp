// 单元测试壳：验证 C6-a 的**帧分割**。
// Session 把三条命令合到一次 ssh 里，靠随机哨兵分段；分割一旦错位就是
// "刷不出来"，而且从现象极难反推原因 —— 所以这里喂合成样本把它钉死。
#include "../../src/apps/backpack/packcore.cpp"

#include <cstdio>

static int g_fail = 0;
static void check(bool ok, const char* what)
{
    printf("%s %s\n", ok ? "  OK  " : "[FAIL]", what);
    if (!ok) ++g_fail;
}

static const std::string MK = "NPPDOCKABCD1234";

int main()
{
    // ---------- 1) 正常一帧 ----------
    {
        std::string t;
        t += MK + " OK\n";
        t += MK + " NOTE\n";
        t += "第一行\n第二行\r\n";            // 便笺：混着 LF / CRLF
        t += "\n";                            // 我们额外 echo 的那一行
        t += MK + " NOTEEND\n";
        t += MK + " LIST\n";
        t += "-rw-r--r-- 1 root root 1234 1700000000 a.txt\n";
        t += "drwxr-xr-x 2 root root 4096 1700000000 sub\n";
        t += MK + " LISTEND\n";

        packcore::SessionOut o;
        ParseSessionFrames(t, MK, o);
        check(o.probeOk, "正常帧：probe 段认出来了");
        check(o.noteOk, "正常帧：便笺段拿到了");
        check(o.note == std::string("第一行\n第二行\r\n"),
              "正常帧：便笺内容逐字节保留（含 CRLF，不去 \\r）");
        check(o.listOk, "正常帧：目录段拿到了");
        check(o.files.size() == 2, "正常帧：解析出两个条目");
    }

    // ---------- 2) 便笺为空（文件是空的）----------
    {
        std::string t = MK + " OK\n" + MK + " NOTE\n" + "\n" +
                        MK + " NOTEEND\n" + MK + " LIST\n" + MK + " LISTEND\n";
        packcore::SessionOut o;
        ParseSessionFrames(t, MK, o);
        check(o.noteOk && o.note.empty(), "空便笺：noteOk=true 且内容为空串");
        check(o.listOk && o.files.empty(), "空目录：listOk=true 且没有条目");
    }

    // ---------- 3) 便笺末行没有换行 ----------
    {
        std::string t = MK + " OK\n" + MK + " NOTE\n" + "没有换行的结尾" + "\n" +
                        MK + " NOTEEND\n" + MK + " LIST\n" + MK + " LISTEND\n";
        packcore::SessionOut o;
        ParseSessionFrames(t, MK, o);
        check(o.noteOk && o.note == std::string("没有换行的结尾"),
              "便笺末行无换行：内容照样完整");
    }

    // ---------- 4) 便笺里出现 "NOTE" 这样的字样也不能被误切 ----------
    {
        std::string t = MK + " OK\n" + MK + " NOTE\n" +
                        "NOTE 这个词很普通\nNOTEEND 也是\n" + "\n" +
                        MK + " NOTEEND\n" + MK + " LIST\n" + MK + " LISTEND\n";
        packcore::SessionOut o;
        ParseSessionFrames(t, MK, o);
        check(o.noteOk && o.note == std::string("NOTE 这个词很普通\nNOTEEND 也是\n"),
              "便笺里含 NOTE / NOTEEND 字样：按**整行+哨兵**判定，不误切");
    }

    // ---------- 5) 少一段（连接中途断掉）----------
    {
        std::string t = MK + " OK\n" + MK + " NOTE\n" + "abc\n" + "\n" +
                        MK + " NOTEEND\n";          // LIST 段没来
        packcore::SessionOut o;
        ParseSessionFrames(t, MK, o);
        check(o.probeOk, "缺 LIST 段：probe 仍然认出来");
        check(!o.noteOk && !o.listOk, "缺 LIST 段：两段都判为『没拿到』（宁可走老路）");
    }

    // ---------- 6) 只有哨兵前缀相同的干扰行 ----------
    {
        std::string t = "NPPDOCKABCD1234 NOTEX 干扰行\n" + MK + " OK\n" +
                        MK + " NOTE\n" + "x\n" + "\n" + MK + " NOTEEND\n" +
                        MK + " LIST\n" + MK + " LISTEND\n";
        packcore::SessionOut o;
        ParseSessionFrames(t, MK, o);
        check(o.probeOk && o.noteOk && o.note == std::string("x\n"),
              "前缀相近的干扰行不影响分割");
    }

    // ---------- 7) 连不上：没有任何哨兵 ----------
    {
        std::string t = "ssh: connect to host 192.0.2.1 port 22: Connection timed out\n";
        packcore::SessionOut o;
        ParseSessionFrames(t, MK, o);
        check(!o.probeOk && !o.noteOk && !o.listOk, "无哨兵：三段全判为没拿到");
    }

    printf("\n%s  失败 %d\n", g_fail ? "有问题：" : "全部通过.", g_fail);
    return g_fail ? 1 : 0;
}
