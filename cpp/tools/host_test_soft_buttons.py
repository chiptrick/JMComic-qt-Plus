# coding:utf-8
"""近似直角按钮（3px）回归：纯源码守卫，不需要 Qt / 手机。

守三件事：
  1. 造型分级：UiStyle::Radius 仍是 0（其余控件保持硬直角），
     只有 UiStyle::RadiusSm = 3 给"登录 / 打卡 / 确定 / 取消 / 开始阅读 / 收藏"用。
  2. QSS 注入顺序：@radiusSm 必须先于 @radius 替换。@radius 是 @radiusSm 的前缀，
     顺序反了会拼出 "0pxSm"，Qt 静默丢弃整条声明 —— 圆角看起来"改了没生效"。
  3. 打标记的位置恰好这 7 个按钮（登录×2、打卡、开始阅读、收藏、确定、取消）；
     "下载本章 / 返回 / 代理与节点设置" 这类同位按钮不许跟着圆，否则全站又花了。
  4. QSS 规则只覆盖 border-radius，不许混进 padding / 颜色 / 状态 —— 尺寸、配色、
     hover/pressed 全部沿用 #Primary / #Ghost / #DangerBtn 原规则。

跑法：
  python android/tools/host_test_soft_buttons.py
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
UI = os.path.join(ROOT, "cpp", "shell", "ui")

Failures = []


def Check(ok, name, extra=""):
    print(("  ok  " if ok else "  FAIL ") + name + (("  [" + extra + "]") if extra and not ok else ""))
    if not ok:
        Failures.append(name)


def Read(name):
    with open(os.path.join(UI, name), encoding="utf-8") as f:
        return f.read()


def Count(hay, needle):
    return hay.count(needle)


def Main():
    theme_h = Read("theme.h")
    theme_cpp = Read("theme.cpp")
    kit = Read("ui_kit.cpp")
    win = Read("ui_main_window.cpp")
    browse = Read("ui_pages_browse.cpp")
    misc = Read("ui_pages_misc.cpp")

    # ---- 1. 造型来源 -------------------------------------------------------
    Check(re.search(r"constexpr\s+int\s+Radius\s*=\s*0\s*;", theme_h) is not None,
          "UiStyle::Radius 仍为 0（其余控件保持直角）")
    Check(re.search(r"constexpr\s+int\s+RadiusSm\s*=\s*3\s*;", theme_h) is not None,
          "UiStyle::RadiusSm = 3")
    Check("void MarkSoftButton(QWidget *w);" in theme_h, "theme.h 声明 MarkSoftButton")
    Check("void MarkSoftButton(QWidget *w)" in theme_cpp, "theme.cpp 定义 MarkSoftButton")
    Check('setProperty("btnShape", "soft")' in theme_cpp, "MarkSoftButton 打 btnShape=soft 标记")

    # ---- 2. token 注入顺序（@radius 是 @radiusSm 的前缀）-------------------
    i_sm = theme_cpp.find('replace(QLatin1String("@radiusSm")')
    i_zero = theme_cpp.find('replace(QLatin1String("@radius"),')
    Check(i_sm >= 0, "theme.cpp 注入 @radiusSm")
    Check(i_zero >= 0, "theme.cpp 仍注入 @radius")
    Check(0 <= i_sm < i_zero, "@radiusSm 先于 @radius 替换（否则拼出 0pxSm）",
          "%d vs %d" % (i_sm, i_zero))

    # ---- 3. QSS 规则：三个 id + 只改 border-radius -------------------------
    m = re.search(r'(QPushButton#Primary\[btnShape="soft"\][^{]*)\{([^}]*)\}', theme_cpp)
    Check(m is not None, "存在 [btnShape=\"soft\"] 的 border-radius 规则")
    if m:
        sel = m.group(1)
        body = re.sub(r"/\*.*?\*/", "", m.group(2), flags=re.S)
        decls = [d.strip() for d in body.split(";") if d.strip()]
        Check('QPushButton#Ghost[btnShape="soft"]' in sel and
              'QPushButton#DangerBtn[btnShape="soft"]' in sel,
              "规则覆盖 Primary / Ghost / DangerBtn")
        Check(decls == ["border-radius: @radiusSm"], "规则只声明 border-radius", str(decls))

    # ---- 4. 七个打标记的位置 ----------------------------------------------
    Check(Count(win, "UiStyle::MarkSoftButton(login)") == 1, "侧栏 登录 打标记")
    Check(Count(win, "UiStyle::MarkSoftButton(sign)") == 1, "侧栏 打卡 打标记")
    Check(Count(misc, "UiStyle::MarkSoftButton(login)") == 1, "登录页 登录 打标记")
    Check(Count(browse, "UiStyle::MarkSoftButton(read)") == 1, "详情页 开始阅读 打标记")
    Check(Count(browse, "UiStyle::MarkSoftButton(favBtn_)") == 1, "详情页 收藏 打标记")
    Check(Count(kit, "UiStyle::MarkSoftButton(ok)") == 1, "确认框 确定 打标记")
    Check(Count(kit, "UiStyle::MarkSoftButton(cancel)") == 1, "确认框 取消 打标记")

    total = sum(Count(t, "UiStyle::MarkSoftButton(") for t in (theme_cpp, kit, win, browse, misc))
    Check(total == 7, "全工程恰好 7 处打标记", str(total))

    # ---- 5. 反例守卫：同位非目标按钮不许圆 --------------------------------
    guards = [
        (browse, "下载本章"),
        (browse, "返回"),
        (browse, "跳转"),
        (misc, "代理与节点设置"),
        (misc, "恢复默认"),
    ]
    for text, label in guards:
        m = re.search(r'new QPushButton\(QStringLiteral\("%s"\)' % re.escape(label), text)
        Check(m is not None, "找到同位按钮 %s" % label)
        if m:
            # 只看这一个按钮的构造段落：到下一个 new QPushButton 为止（相邻按钮各有各的标记）
            nxt = text.find("new QPushButton", m.end())
            tail = text[m.start(): nxt if nxt > 0 else m.start() + 400]
            Check("MarkSoftButton" not in tail, "同位按钮 %s 保持直角" % label)

    print("")
    if Failures:
        print("FAILED: %d" % len(Failures))
        for f in Failures:
            print("  - " + f)
        return 1
    print("all soft-button guards passed")
    return 0


if __name__ == "__main__":
    sys.exit(Main())
