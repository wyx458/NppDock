// ===========================================================================
// packprops.h —— 「文件属性」小窗
//
// 王的原话要求（v1.6）：
//   · 弹窗要**丰富一些**（不是一行 MessageBox）；
//   · **Win32 风格**；
//   · **不要有声音** —— 所以不用 MessageBox（它带系统提示音），自己建窗；
//   · **每一行都能单独复制**；
//   · 最下面放「复制全部」和「关闭」。
//
// 版面（一行 = 标签 + 可选中/可复制的值 + 一个「复制」）：
//   ┌ 文件属性 ─────────────────────────────────────────────┐
//   │  名称     [便笺.txt                              ] [复制] │
//   │  类型     [文件                                  ] [复制] │
//   │  大小     [1.2 KB（1234 字节）                    ] [复制] │
//   │  修改时间 [10-04 17:38                             ] [复制] │
//   │  位置     [/home/ubuntu/npp-backpack/便笺.txt      ] [复制] │
//   │                                          [复制全部][关闭] │
//   └────────────────────────────────────────────────────────┘
// ===========================================================================
#pragma once

#include <windows.h>
#include <string>
#include <vector>

namespace packprops {

struct Row {
    std::wstring label;
    std::wstring value;
};

// 弹出属性窗（**模态**：返回前会禁掉 owner，挡住底下的操作）。
// 必须在 owner 所在线程调用（内部跑一个嵌套消息循环）。
void Show(HWND owner, const std::wstring& title, const std::vector<Row>& rows);

// v1.7：单行文本输入小窗（重命名用）。
//
// 同样**自己建窗**、不用 MessageBox / DialogBox —— 那两个都带系统提示音，
// 王明确说过不要声音（属性窗当初就是为这个改的自建窗）。
// 返回 true = 用户按了「确定」；value 是去掉首尾空白后的结果（空串会被拒绝）。
bool Prompt(HWND owner, const std::wstring& title, const std::wstring& label,
            const std::wstring& initial, std::wstring& value);

// 把一行拼成 "标签：值"（「复制全部」和单行复制都用它，保证两边文字一致）
std::wstring Format(const Row& r);

} // namespace packprops
