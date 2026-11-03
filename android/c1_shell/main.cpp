// C1-W4/W5：Qt 6.12 C++ 壳 —— 返回栈 + 标题栏 + 侧栏 + 本地阅读器 + 设置页。
//
// W1：可出包 + 16KB 门禁 0 未对齐 + 同 C0 的空壳级启动；
// W2：本地阅读器（打开本地目录 → 翻页 → 缩放）；
// W3：阅读器体验（双页/RTL/亮度/手势）；
// W4：返回栈 + 标题栏 + 侧栏（页面切换矩阵）；
// W5：设置页（QSettings INI，与 Python 版 config.ini 同格式同 section/key）。
// 保持 libsr_qnn.so 直链不退回 ctypes。
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QMainWindow>
#include <QPushButton>
#include <QStandardPaths>
#include <QStackedWidget>
#include <QTimer>
#include <QVector>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLoggingCategory>
#include <QSslSocket>

#include <android/log.h>
#include <dlfcn.h>
#include <link.h>

#include "reader.h"
#include "settings_page.h"
#include "net_client.h"
#include "jm_api.h"
#include "browse_page.h"
#include "frame_probe.h"

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "C1Shell", __VA_ARGS__)

namespace {

// C0-③ 判据（照搬）：进程 link map 里必须能找到直链的 libsr_qnn.so。
struct SrqProbe {
    bool found = false;
    QString path;
    void *base = nullptr;
};

int FindSrq(struct dl_phdr_info *info, size_t, void *data)
{
    auto *p = static_cast<SrqProbe *>(data);
    const char *name = info->dlpi_name ? info->dlpi_name : "";
    if (QString::fromLocal8Bit(name).endsWith(QLatin1String("libsr_qnn.so"))) {
        p->found = true;
        p->path = QString::fromLocal8Bit(name);
        p->base = reinterpret_cast<void *>(info->dlpi_addr);
        return 1;
    }
    return 0;
}

QString ProbeSrq()
{
    SrqProbe probe;
    dl_iterate_phdr(FindSrq, &probe);
    if (probe.found) {
        LOGI("sr_qnn in link map OK, base=%p path=%s", probe.base, qPrintable(probe.path));
        return QStringLiteral("sr_qnn direct-link: YES");
    }
    void *h = dlopen("libsr_qnn.so", RTLD_NOLOAD | RTLD_NOW);
    const QString line = QStringLiteral("sr_qnn direct-link: NO (%1)")
                             .arg(h ? QStringLiteral("handle-only") : QStringLiteral("absent"));
    LOGI("sr_qnn link map FAIL, dlopen=%p err=%s", h, h ? "-" : dlerror());
    if (h)
        dlclose(h);
    return line;
}

// W4：页面栈 + 历史返回栈。Push 记录来源页，Pop 回退；标题栏跟随当前页。
class ShellWindow : public QMainWindow
{
public:
    enum Page { Home = 0, Reader = 1, Settings = 2, Browse = 3 };

    ShellWindow()
    {
        setWindowTitle(QStringLiteral("C1 shell"));
        resize(540, 960);

        auto *central = new QWidget(this);
        auto *root = new QVBoxLayout(central);

        // ---- 标题栏 + 侧栏（顶部导航行；真机宽屏时也可横向排列） ----
        auto *nav = new QHBoxLayout();
        back_ = new QPushButton(QStringLiteral("Back"), central);
        homeBtn_ = new QPushButton(QStringLiteral("Home"), central);
        readerBtn_ = new QPushButton(QStringLiteral("Reader"), central);
        browseBtn_ = new QPushButton(QStringLiteral("Browse"), central);
        settingsBtn_ = new QPushButton(QStringLiteral("Settings"), central);
        nav->addWidget(back_);
        nav->addWidget(homeBtn_);
        nav->addWidget(readerBtn_);
        nav->addWidget(browseBtn_);
        nav->addWidget(settingsBtn_);
        root->addLayout(nav);

        title_ = new QLabel(QStringLiteral("Home"), central);
        title_->setAlignment(Qt::AlignCenter);
        root->addWidget(title_);

        stack_ = new QStackedWidget(central);
        root->addWidget(stack_, 1);

        // ---- Home ----
        auto *home = new QWidget(central);
        auto *homeLay = new QVBoxLayout(home);
        auto *openReader = new QPushButton(QStringLiteral("Open local reader"), home);
        homeLay->addWidget(openReader);
        // C2/C3：在线搜索/详情/章节（不再经过 Python 子进程）
        auto *openBrowse = new QPushButton(QStringLiteral("Browse online (JM API)"), home);
        homeLay->addWidget(openBrowse);
        // P4-5：真机上一次真实网络请求的人工入口（自检钩子之外的第二条取证路径）
        auto *netBtn = new QPushButton(QStringLiteral("Net test (bridge)"), home);
        homeLay->addWidget(netBtn);
        homeLay->addStretch(1);
        stack_->addWidget(home);

        // ---- Reader ----
        reader_ = new ReaderView(central);
        stack_->addWidget(reader_);

        // ---- Settings ----
        settings_ = new SettingsPage(central);
        stack_->addWidget(settings_);

        // ---- Browse（在线） ----
        // net_ 必须先建：BrowsePage 的构造就把它交给 JmApi 用（JmApi 不持有它）。
        net_ = new NetClient(central);
        browse_ = new BrowsePage(net_, central);
        stack_->addWidget(browse_);

        // ---- Reader 工具条 ----
        auto *bar = new QHBoxLayout();
        auto *next = new QPushButton(QStringLiteral("Next page"), central);
        auto *twoPage = new QPushButton(QStringLiteral("2-up"), central);
        auto *dirBtn = new QPushButton(QStringLiteral("RTL"), central);
        auto *bright = new QPushButton(QStringLiteral("Dim"), central);
        bar->addWidget(next);
        bar->addWidget(twoPage);
        bar->addWidget(dirBtn);
        bar->addWidget(bright);
        root->addLayout(bar);

        status_ = new QLabel(ProbeSrq(), central);
        status_->setAlignment(Qt::AlignCenter);
        root->addWidget(status_);

        // net_ 已在 Browse 段创建；这里只挂全局状态显示，不能重建（重建会让
        // BrowsePage/JmApi 握着旧对象，请求发出去后永远等不到 Finished）。
        connect(net_, &NetClient::Finished, this, [this](const NetResult &r) {
            const QString line = r.Summary();
            status_->setText(line);
            LOGI("%s", qPrintable(line));
            if (r.ok)
                SetTitle(QStringLiteral("net %1").arg(r.httpStatus));
        });

        connect(openBrowse, &QPushButton::clicked, this, [this] { Push(Page::Browse); });
        connect(browseBtn_, &QPushButton::clicked, this, [this] { Push(Page::Browse); });
        connect(browse_, &BrowsePage::StatusMessage, this, [this](const QString &text) {
            status_->setText(text);
        });
        connect(browse_, &BrowsePage::OpenReaderRequested, this,
                [this](const QStringList &files, const QString &title) {
                    LOGI("open online reader: %s (%d pages)", qPrintable(title), int(files.size()));
                    reader_->LoadFiles(files);
                    Push(Page::Reader);
                });
        connect(browse_, &BrowsePage::SelfTestDone, this, [this](const QString &summary) {
            LOGI("browse selftest done: %s", qPrintable(summary));
            SetTitle(QStringLiteral("browse %1").arg(summary.left(40)));
        });

        connect(openReader, &QPushButton::clicked, this, [this] {
            Push(Page::Reader);
            reader_->LoadDefaultOrPick();
        });
        connect(netBtn, &QPushButton::clicked, this, [this] {
            RunNetProbe();
        });
        // 点完工具条要把焦点还给阅读器：否则焦点停在被点的按钮上，
        // 之后的左右键/加减号全部失效（用蓝牙键盘时会立刻遇到）。
        // 这是 W7 的 F4 用例抓到的真实缺陷，不是测试问题。
        auto keepReaderFocus = [this] { reader_->setFocus(Qt::OtherFocusReason); };
        connect(next, &QPushButton::clicked, this, [this, keepReaderFocus] {
            reader_->NextPage();
            keepReaderFocus();
        });
        connect(twoPage, &QPushButton::clicked, this, [this, keepReaderFocus] {
            reader_->ToggleTwoPage();
            keepReaderFocus();
        });
        connect(dirBtn, &QPushButton::clicked, this, [this, keepReaderFocus] {
            reader_->ToggleDirection();
            keepReaderFocus();
        });
        connect(bright, &QPushButton::clicked, this, [this, keepReaderFocus] {
            reader_->CycleBrightness();
            keepReaderFocus();
        });

        connect(homeBtn_, &QPushButton::clicked, this, [this] { Push(Page::Home); });
        connect(readerBtn_, &QPushButton::clicked, this, [this] {
            Push(Page::Reader);
            reader_->LoadDefaultOrPick();
        });
        connect(settingsBtn_, &QPushButton::clicked, this, [this] { Push(Page::Settings); });
        connect(back_, &QPushButton::clicked, this, [this] { Pop(); });

        connect(reader_, &ReaderView::PageChanged, this, [this](int cur, int total, const QString &path) {
            // 只在阅读器是当前页时才改标题，否则会把 Home/Settings 的标题覆盖掉
            if (stack_->currentIndex() == Page::Reader)
                SetReaderTitle();
            if (!path.isEmpty())
                LOGI("page %d/%d %s", cur, total, qPrintable(path));
        });
        connect(reader_, &ReaderView::StatusMessage, this, [this](const QString &text) {
            status_->setText(text);
            LOGI("%s", qPrintable(text));
        });
        connect(reader_, &ReaderView::SelfTestDone, this, [this] {
            SetTitle(QStringLiteral("selftest done"));
        });
        connect(settings_, &SettingsPage::StatusMessage, this, [this](const QString &text) {
            status_->setText(text);
            LOGI("%s", qPrintable(text));
        });

        UpdateNavEnabled();
        setCentralWidget(central);
    }

    // 冒烟入口：run-as 建一个 selftest 标记文件即可触发（只在标记存在时跑一次）
    void MaybeRunSelfTest()
    {
        const QString marker = QStandardPaths::writableLocation(
                                   QStandardPaths::AppLocalDataLocation)
                               + QStringLiteral("/selftest");
        if (!QFile::exists(marker)) {
            LOGI("selftest marker absent: %s", qPrintable(marker));
        } else {
            QFile::remove(marker);
            LOGI("selftest marker found: %s", qPrintable(marker));
            Push(Page::Reader);
            QTimer::singleShot(200, this, [this] { reader_->RunSelfTest(); });
            QTimer::singleShot(3200, this, [this] { settings_->RunSelfTest(); });
        }
        MaybeRunNetSelfTest();
        MaybeRunBrowseSelfTest();
    }

    // C2/C3 自检：run-as 建 `browse_selftest` 标记 -> 搜索 -> 详情 -> 章节 -> 下前 N 页。
    // 关键字与页数可用 JM_C1_BROWSE_Q / JM_C1_BROWSE_PAGES 覆盖（默认 test / 3）。
    void MaybeRunBrowseSelfTest()
    {
        const QString marker = QStandardPaths::writableLocation(
                                   QStandardPaths::AppLocalDataLocation)
                               + QStringLiteral("/browse_selftest");
        if (!QFile::exists(marker))
            return;
        QFile::remove(marker);
        const QString query = qEnvironmentVariable("JM_C1_BROWSE_Q", QStringLiteral("test"));
        const int pages = qEnvironmentVariableIntValue("JM_C1_BROWSE_PAGES");
        LOGI("browse selftest marker found: q=%s pages=%d", qPrintable(query), pages);
        Push(Page::Browse);
        browse_->RunSelfTest(query, pages > 0 ? pages : 3);
    }

    // 一次真实请求，URL 可由 JM_C1_NET_URL 覆盖（默认取一个稳定的小页面）。
    // 与旧 Python 桥相比没有子进程：失败只可能是网络层错误，不会再有
    // "进程起不来 / 被 SELinux 拒绝 exec" 这类环境问题。
    void RunNetProbe()
    {
        const QString url = qEnvironmentVariable("JM_C1_NET_URL",
                                                 QStringLiteral("https://www.baidu.com/"));
        LOGI("net probe: GET %s", qPrintable(url));
        NetRequest req;
        req.url = QUrl(url);
        req.timeoutMs = 15000;
        req.maxBytes = 64 * 1024;
        if (!net_->Send(req))
            LOGI("net probe rejected: %s", qPrintable(net_->GetLastError()));
    }

    // 自检钩子：run-as 建 `net_selftest` 标记即可触发（W7 的取证入口）
    void MaybeRunNetSelfTest()
    {
        const QString marker = QStandardPaths::writableLocation(
                                   QStandardPaths::AppLocalDataLocation)
                               + QStringLiteral("/net_selftest");
        if (!QFile::exists(marker))
            return;
        QFile::remove(marker);
        LOGI("net selftest marker found");
        RunNetProbe();
    }

private:
    void Push(int index)
    {
        if (index < 0 || index >= stack_->count())
            return;
        if (stack_->currentIndex() != index) {
            history_.append(stack_->currentIndex());
            stack_->setCurrentIndex(index);
        }
        // 键盘焦点跟着页面走：否则焦点会留在顶栏按钮上（实测是 Home 按钮），
        // 方向键/加减号全部被按钮链吞掉，阅读器的 keyPressEvent 永远收不到。
        if (index == Page::Reader)
            reader_->setFocus(Qt::OtherFocusReason);
        UpdateTitle();
        UpdateNavEnabled();
    }

    void Pop()
    {
        if (history_.isEmpty()) {
            if (stack_->currentIndex() != Page::Home)
                stack_->setCurrentIndex(Page::Home);
        } else {
            stack_->setCurrentIndex(history_.takeLast());
        }
        UpdateTitle();
        UpdateNavEnabled();
    }

    void UpdateTitle()
    {
        switch (stack_->currentIndex()) {
        case Page::Reader:
            // 必须带上页码：Push/Pop 会调用本函数，若这里写死 "Reader"，
            // 就会把 PageChanged 刚设好的 "Reader 3/12" 顶掉 —— 表现为
            // "进阅读器看不到第几页，翻一次页才出现"（W7 的 F2 判据抓到的真缺陷）。
            SetReaderTitle();
            break;
        case Page::Settings:
            SetTitle(QStringLiteral("Settings"));
            break;
        case Page::Browse:
            SetTitle(QStringLiteral("Browse"));
            break;
        default:
            SetTitle(QStringLiteral("Home"));
        }
    }

    void SetTitle(const QString &text)
    {
        title_->setText(text);
    }

    // 阅读器标题的唯一来源：总页数与当前页都从 ReaderView 取，不依赖信号时序。
    void SetReaderTitle()
    {
        const int total = reader_ ? reader_->PageCount() : 0;
        const int cur = reader_ ? reader_->CurrentPage() : 0;
        SetTitle(total > 0 ? QStringLiteral("Reader %1/%2").arg(cur).arg(total)
                           : QStringLiteral("Reader (no pages)"));
    }

    void UpdateNavEnabled()
    {
        back_->setEnabled(!history_.isEmpty());
    }

    QStackedWidget *stack_ = nullptr;
    ReaderView *reader_ = nullptr;
    SettingsPage *settings_ = nullptr;
    BrowsePage *browse_ = nullptr;
    NetClient *net_ = nullptr;
    QLabel *title_ = nullptr;
    QLabel *status_ = nullptr;
    QPushButton *back_ = nullptr;
    QPushButton *homeBtn_ = nullptr;
    QPushButton *readerBtn_ = nullptr;
    QPushButton *browseBtn_ = nullptr;
    QPushButton *settingsBtn_ = nullptr;
    QVector<int> history_;
};

}  // namespace

int main(int argc, char *argv[])
{
    // TLS 后端问题只会在第一次用 QSslSocket 时才暴露，且默认日志级别看不到原因；
    // 打开 qt.network.ssl 的 debug 规则，把"为什么不可用"打到 logcat。
    QLoggingCategory::setFilterRules(QStringLiteral("qt.network.ssl.debug=true"));
    QApplication app(argc, argv);
    // TLS 后端与证书就位情况各打一行：Android 上"没有后端/没有 CA"都只会表现为
    // SSLErr，不留痕迹的话现场无法定位。
    LOGI("tls backends: [%s] supportsSsl=%d",
         qPrintable(QSslSocket::availableBackends().join(QLatin1Char(','))),
         int(QSslSocket::supportsSsl()));
    const int caCount = NetClient::EnsureCaCertificates();
    LOGI("CA certificates: %d", caCount);
    ShellWindow win;
    win.show();

    // 性能探针：存在 fps_log 标记时，阅读器会把每次翻页耗时汇总成 C1Fps 行。
    // 不测 fps 的原因见 frame_probe.h 头注释（无连续动画，且 gfxinfo/SF 都取不到数）。
    if (FrameProbe::Enabled())
        LOGI("perf probe enabled: page-turn timing");

    win.MaybeRunSelfTest();
    return app.exec();
}
