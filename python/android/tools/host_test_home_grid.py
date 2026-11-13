# coding:utf-8
"""宿主复现：首页"最新上传"与"分区"两页的每行封面数

三条时序都按生产代码的真实路径走(ComicListWidget.AddBookItem + 去抖重排)：
  T1  隐藏分区先填满 -> 切过去          (README §3.7 的场景)
  T2  切过去之后才填满                   (分区是"切页后才请求数据"，InitBack 之外的常规路径)
  T3  最新上传(一直可见)填满
期望三条都是"一行 2 个"。

用法：py -3.14 android/tools/host_test_home_grid.py
"""
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "src"))
os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
os.environ["JMCOMIC_ANDROID"] = "1"

from PySide6.QtCore import QPointF  # noqa: E402
from PySide6.QtWidgets import QApplication, QTabWidget, QVBoxLayout, QWidget  # noqa: E402

app = QApplication([])

from component.list.comic_list_widget import ComicListWidget  # noqa: E402
from tools import mobile_ui  # noqa: E402

FAILS = []
AVAIL = 364


def Check(name, ok, detail=""):
    print("[{}] {}{}".format("ok" if ok else "FAIL", name, ("  " + detail) if detail else ""))
    if not ok:
        FAILS.append(name)


def Pump(seconds=0.5):
    deadline = time.time() + seconds
    while time.time() < deadline:
        app.processEvents()
        time.sleep(0.01)


def StopPendingGrid():
    timer = mobile_ui._gridTimer
    if timer is not None:
        timer.stop()
    mobile_ui._gridPending = []


def Fill(view, n=6, tag="b"):
    for i in range(n):
        view.AddBookItem("{}{}".format(tag, i), "书 {}{}".format(tag, i), "", "")


def PerRow(view):
    if not view.count():
        return 0
    first = view.visualItemRect(view.item(0))
    row = 1
    for i in range(1, view.count()):
        if abs(view.visualItemRect(view.item(i)).top() - first.top()) <= 2:
            row += 1
        else:
            break
    return row


def Report(tag, view):
    vp = view.viewport()
    first = view.visualItemRect(view.item(0)) if view.count() else None
    print("     {}: 视口 {}px, items {}, 首项 {}px, 栅格 {}px, 每行 {} 个".format(
        tag, vp.width(), view.count(), first.width() if first else -1,
        view.gridSize().width(), PerRow(view)))
    return PerRow(view)


def NewHost(pages=4, width=None):
    host = QTabWidget()
    host.resize(width or AVAIL, 480)
    lists = []
    for i in range(pages):
        page = QWidget()
        layout = QVBoxLayout(page)
        layout.setContentsMargins(0, 0, 0, 0)
        lst = ComicListWidget(page)
        layout.addWidget(lst)
        host.addTab(page, "最新上传" if i == 0 else "分区%d" % i)
        lists.append(lst)
    host.show()
    Pump(1.0)
    mobile_ui.InstallTabReflowHook(host)
    return host, lists


# ---------------------------------------------------------------- T3 可见页
mobile_ui.ResetGridCover()
StopPendingGrid()
host, lists = NewHost()
Fill(lists[0], 6, "n")
Pump(0.6)
t3 = Report("T3 最新上传(可见)", lists[0])
Check("T3 最新上传页一行 2 个", t3 == 2, "实测 {}".format(t3))

# ---------------------------------------------------------------- T1 隐藏页先填
mobile_ui.ResetGridCover()
StopPendingGrid()
host1, lists1 = NewHost()
Fill(lists1[1], 6, "h")
Pump(0.4)
host1.setCurrentIndex(1)
Pump(1.0)
t1 = Report("T1 分区(隐藏时填满->切页)", lists1[1])
Check("T1 隐藏分区切页后一行 2 个", t1 == 2, "实测 {}".format(t1))

# ---------------------------------------------------------------- T2 切页后填
mobile_ui.ResetGridCover()
StopPendingGrid()
host2, lists2 = NewHost()
host2.setCurrentIndex(1)
Pump(0.5)
Fill(lists2[1], 6, "a")      # 真机：切到分区后才请求数据
Pump(0.8)
t2 = Report("T2 分区(切页后才填满)", lists2[1])
Check("T2 切页后填的分区一行 2 个", t2 == 2, "实测 {}".format(t2))

# ---------------------------------------------------------------- T5 主栈页面
# 首页之外的页面(下载/收藏/历史/本地…)挂在 mainView.subStackWidget 里，
# 由 InstallReflowHook 在换页后重排。它们和分区页走的是同一套网格代码，
# 这里单独验一遍"页面刚露出来"的路径。
print()
mobile_ui.ResetGridCover()
StopPendingGrid()
from PySide6.QtWidgets import QStackedWidget  # noqa: E402

stack = QStackedWidget()
stack.resize(AVAIL, 480)
pageA = QWidget()
layA = QVBoxLayout(pageA)
layA.setContentsMargins(0, 0, 0, 0)
listA = ComicListWidget(pageA)
layA.addWidget(listA)
pageB = QWidget()
layB = QVBoxLayout(pageB)
layB.setContentsMargins(0, 0, 0, 0)
listB = ComicListWidget(pageB)
layB.addWidget(listB)
stack.addWidget(pageA)
stack.addWidget(pageB)
stack.show()
Pump(0.8)
mobile_ui.InstallReflowHook(stack)
Fill(listA, 5, "p")
Pump(0.5)
stack.setCurrentIndex(1)          # 换到"下载/收藏"这类后建页面
Pump(0.5)
Fill(listB, 5, "q")
Pump(0.8)
t5a = PerRow(listA)
t5b = PerRow(listB)
Report("T5 主栈第一页", listA)
Report("T5 主栈第二页(换页后填)", listB)
Check("T5 主栈各页一行 2 个", t5a == 2 and t5b == 2, "实测 {} / {}".format(t5a, t5b))

# ---------------------------------------------------------------- T4 宽度扫描
# 真机/模拟器/分屏的逻辑宽度都在 320~480 之间。"cell 恰好等于视口/2"这类
# 边界只在特定宽度上翻车(346 翻车、342 不翻)，所以必须按宽度扫一遍：
# 每个宽度下三条时序都必须是 2 个。
print()
for width in (320, 330, 340, 346, 360, 384, 412, 480):
    mobile_ui.ResetGridCover()
    StopPendingGrid()
    host3, lists3 = NewHost(width=width)
    Fill(lists3[0], 4, "w")
    Pump(0.4)
    visible = PerRow(lists3[0])
    host3.setCurrentIndex(1)
    Pump(0.6)
    Fill(lists3[1], 4, "x")      # 切页后才填(真机分区路径)
    Pump(0.6)
    switched = PerRow(lists3[1])
    Check("宽 {}px: 最新上传/分区 都是 2 个".format(width),
          visible == 2 and switched == 2,
          "实测 {} / {} (视口 {}px)".format(visible, switched, lists3[0].viewport().width()))
    host3.close()
    Pump(0.1)

print()
print("RESULT: {}".format("PASS" if not FAILS else "FAIL " + ", ".join(FAILS)))
sys.exit(0 if not FAILS else 1)
