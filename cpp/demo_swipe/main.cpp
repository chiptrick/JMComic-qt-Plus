// DEMO 入口：默认打开可交互窗口；--shot / --freeze / --selftest 供回归与截图使用。
//
// 常用：
//   jm_swipe_demo                                 手势 DEMO（拖左/拖右切分区，点左侧切模块）
//   jm_swipe_demo --shot a.png --freeze 0.45      冻结在分区 0→1 的 45% 处截图
//   jm_swipe_demo --shot b.png --level module --from 0 --to 2 --freeze 0.6
//   jm_swipe_demo --selftest                      打印阈值/边界/仲裁断言
//   jm_swipe_demo --auto                          自动走一遍全部转场
//   jm_swipe_demo --fade                          以"现状交叉淡入"启动做 A/B
#include "demo_widgets.h"
#include "demo_window.h"
#include "swipe_pager.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QFont>
#include <QMouseEvent>
#include <QTimer>

#include <cstdio>

namespace {

int gFails = 0;

void Check(const char *name, bool ok, const QString &detail)
{
    if (!ok)
        ++gFails;
    std::printf("%s %-40s %s\n", ok ? "PASS" : "FAIL", name, detail.toUtf8().constData());
}

QString Num(qreal v, int digits = 2)
{
    return QString::number(v, 'f', digits);
}

int RunSelfTest(DemoWindow &win)
{
    SlidePager *s = win.SectionPager();
    SlidePager *m = win.ModulePager();
    if (!s || !m) {
        std::printf("FAIL no pager\n");
        return 1;
    }
    // 时长 0：离手即落定，断言不需要等事件循环
    s->SetSlideDuration(0);
    m->SetSlideDuration(0);

    const int w = s->width();
    std::printf("-- 容器宽 %dpx，提交阈值 %.0fpx（28%%），轴锁 %dpx\n", w, w * 0.28,
                s->AxisLock());

    s->SetCurrent(0, false);
    Check("初始分区 = 0", s->Current() == 0, QStringLiteral("current=%1").arg(s->Current()));

    s->DemoSwipe(-60, 0);
    Check("位移 60px < 阈值 → 回弹", s->Current() == 0 && qAbs(s->Progress()) < 0.001,
          QStringLiteral("current=%1 p=%2").arg(s->Current()).arg(Num(s->Progress())));

    s->DemoSwipe(-140, 0);
    Check("位移 140px > 阈值 → 提交", s->Current() == 1,
          QStringLiteral("current=%1").arg(s->Current()));

    s->DemoSwipe(140, 0);
    Check("反向右滑 → 回到 0", s->Current() == 0, QStringLiteral("current=%1").arg(s->Current()));

    s->SetVelocityOverride(-1.2);
    s->DemoSwipe(-50, 0);
    s->SetVelocityOverride(0.0);
    Check("位移 50px 但甩速 1.2px/ms → 提交", s->Current() == 1,
          QStringLiteral("current=%1").arg(s->Current()));

    s->SetCurrent(0, false);
    s->DemoSwipe(-30, 110);
    Check("纵向手势不触发分区切换", s->Current() == 0,
          QStringLiteral("current=%1 p=%2").arg(s->Current()).arg(Num(s->Progress())));

    s->SetCurrent(s->Count() - 1, false);
    const int last = s->Current();
    s->DemoSwipe(-240, 0);
    Check("末页继续左滑 → 只回弹，不级联模块",
          s->Current() == last && qAbs(s->Progress()) < 0.001,
          QStringLiteral("current=%1 module=%2").arg(s->Current()).arg(m->Current()));

    s->SetCurrent(0, false);
    s->DemoSwipe(-4000, 0);
    Check("单次手势最多一页", s->Current() == 1, QStringLiteral("current=%1").arg(s->Current()));

    m->SetCurrent(0, false);
    s->SetCurrent(0, false);
    s->DemoSwipe(-220, 0);
    Check("嵌套仲裁：分区手势不带走模块", m->Current() == 0 && s->Current() == 1,
          QStringLiteral("module=%1 section=%2").arg(m->Current()).arg(s->Current()));

    m->SetCurrent(4, true);
    Check("非相邻模块跳转落点正确", m->Current() == 4,
          QStringLiteral("module=%1").arg(m->Current()));

    // ---- 子控件路径：按下发生在卡片上，验证"抢手势后不误触点击" ----
    // 只在当前可见的分区页里找，隐藏页从未布局（卡片高度为 0），命中测试没意义
    QWidget *page0 = s->PageAt(0);
    auto *card = page0 ? page0->findChild<DemoCard *>() : nullptr;
    if (!card || card->height() <= 0) {
        Check("首页卡片已布局", false,
              QStringLiteral("card=%1 h=%2").arg(card ? "ok" : "null").arg(card ? card->height() : -1));
    } else {
        int clicks = 0;
        QObject::connect(card, &DemoCard::Activated, card, [&clicks] { ++clicks; });

        s->SetCurrent(0, false);
        const QPoint center = card->rect().center();
        auto send = [card, center](QEvent::Type type, const QPoint &local, Qt::MouseButton button,
                                   Qt::MouseButtons buttons) {
            QMouseEvent ev(type, QPointF(local), QPointF(card->mapToGlobal(local)), button, buttons,
                           Qt::NoModifier);
            QCoreApplication::sendEvent(card, &ev);
        };
        send(QEvent::MouseButtonPress, center, Qt::LeftButton, Qt::LeftButton);
        for (int i = 1; i <= 5; ++i)
            send(QEvent::MouseMove, center + QPoint(-30 * i, 0), Qt::NoButton, Qt::LeftButton);
        send(QEvent::MouseButtonRelease, center + QPoint(-150, 0), Qt::LeftButton, Qt::NoButton);
        Check("拖拽卡片不误触点击", clicks == 0, QStringLiteral("clicks=%1").arg(clicks));
        Check("拖拽卡片可切分区", s->Current() == 1,
              QStringLiteral("current=%1").arg(s->Current()));

        s->SetCurrent(0, false);
        send(QEvent::MouseButtonPress, center, Qt::LeftButton, Qt::LeftButton);
        send(QEvent::MouseButtonRelease, center, Qt::LeftButton, Qt::NoButton);
        Check("纯点按卡片仍然触发", clicks == 1, QStringLiteral("clicks=%1").arg(clicks));
    }

    m->SetCurrent(0, false);
    s->SetCurrent(0, false);
    std::printf("%s\n", gFails == 0 ? "-- 全部通过" : "-- 存在失败项");
    return gFails == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("jm-swipe-demo"));

    QFont font = app.font();
    font.setFamily(QStringLiteral("Microsoft YaHei UI"));
    font.setPixelSize(13);
    app.setFont(font);

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("首页横向滑动 / 整页转场 DEMO"));
    parser.addHelpOption();
    QCommandLineOption shotOpt(QStringLiteral("shot"), QStringLiteral("截图到文件"),
                               QStringLiteral("file"));
    QCommandLineOption freezeOpt(QStringLiteral("freeze"), QStringLiteral("冻结进度 0..1"),
                                 QStringLiteral("p"));
    QCommandLineOption levelOpt(QStringLiteral("level"), QStringLiteral("section|module"),
                                QStringLiteral("level"), QStringLiteral("section"));
    QCommandLineOption fromOpt(QStringLiteral("from"), QStringLiteral("起点索引"), QStringLiteral("n"));
    QCommandLineOption toOpt(QStringLiteral("to"), QStringLiteral("目标索引"), QStringLiteral("n"));
    QCommandLineOption fadeOpt(QStringLiteral("fade"), QStringLiteral("以现状交叉淡入启动"));
    QCommandLineOption autoOpt(QStringLiteral("auto"), QStringLiteral("自动演示一遍"));
    QCommandLineOption selfOpt(QStringLiteral("selftest"), QStringLiteral("跑断言后退出"));
    QCommandLineOption rulesOpt(QStringLiteral("rules"), QStringLiteral("打开边界说明浮层"));
    QCommandLineOption delayOpt(QStringLiteral("delay"), QStringLiteral("截图延时"),
                                QStringLiteral("ms"), QStringLiteral("320"));
    QCommandLineOption wOpt(QStringLiteral("width"), QStringLiteral("窗口宽"), QStringLiteral("px"));
    QCommandLineOption hOpt(QStringLiteral("height"), QStringLiteral("窗口高"), QStringLiteral("px"));
    parser.addOption(shotOpt);
    parser.addOption(freezeOpt);
    parser.addOption(levelOpt);
    parser.addOption(fromOpt);
    parser.addOption(toOpt);
    parser.addOption(fadeOpt);
    parser.addOption(autoOpt);
    parser.addOption(selfOpt);
    parser.addOption(rulesOpt);
    parser.addOption(delayOpt);
    parser.addOption(wOpt);
    parser.addOption(hOpt);
    parser.process(app);

    DemoWindow win;
    if (parser.isSet(wOpt) || parser.isSet(hOpt)) {
        const int ww = parser.isSet(wOpt) ? parser.value(wOpt).toInt() : win.width();
        const int hh = parser.isSet(hOpt) ? parser.value(hOpt).toInt() : win.height();
        win.resize(ww, hh);
    }
    if (parser.isSet(fadeOpt))
        win.SetCompareFade(true);

    win.show();
    QApplication::processEvents();

    if (parser.isSet(selfOpt))
        return RunSelfTest(win);

    if (parser.isSet(freezeOpt)) {
        SlidePager *pager = parser.value(levelOpt) == QLatin1String("module") ? win.ModulePager()
                                                                            : win.SectionPager();
        const int from = parser.isSet(fromOpt) ? parser.value(fromOpt).toInt() : 0;
        int to = parser.isSet(toOpt) ? parser.value(toOpt).toInt() : -1;
        if (to < 0)
            to = from + 1;
        pager->FreezeAt(from, to, parser.value(freezeOpt).toDouble());
        std::printf("freeze %s %d->%d p=%s\n", qPrintable(parser.value(levelOpt)), from, to,
                    qPrintable(parser.value(freezeOpt)));
        std::printf("  geo pager=%dx%d cur.x=%d incoming.x=%d\n", pager->width(), pager->height(),
                    pager->PageAt(from) ? pager->PageAt(from)->x() : -1,
                    pager->PageAt(to) ? pager->PageAt(to)->x() : -1);
    }

    if (parser.isSet(rulesOpt))
        win.ShowRules(true);

    if (parser.isSet(autoOpt))
        win.StartAutoSequence();

    if (parser.isSet(shotOpt)) {
        const QString path = parser.value(shotOpt);
        const int delay = qMax(0, parser.value(delayOpt).toInt());
        QTimer::singleShot(delay, &win, [&win, path] {
            const QPixmap pm = win.grab();
            const bool ok = pm.save(path);
            std::printf("shot %s %dx%d ok=%d\n", qPrintable(path), pm.width(), pm.height(),
                        ok ? 1 : 0);
            QCoreApplication::quit();
        });
    }

    return app.exec();
}
