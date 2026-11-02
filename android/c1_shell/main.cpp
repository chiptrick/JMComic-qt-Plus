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
    enum Page { Home = 0, Reader = 1, Settings = 2 };

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
        settingsBtn_ = new QPushButton(QStringLiteral("Settings"), central);
        nav->addWidget(back_);
        nav->addWidget(homeBtn_);
        nav->addWidget(readerBtn_);
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

        net_ = new NetClient(central);
        connect(net_, &NetClient::Finished, this, [this](const NetResult &r) {
            const QString line = r.Summary();
            status_->setText(line);
            LOGI("%s", qPrintable(line));
            if (r.ok)
                SetTitle(QStringLiteral("net %1").arg(r.httpStatus));
        });

        connect(openReader, &QPushButton::clicked, this, [this] {
            reader_->LoadDefaultOrPick();
            Push(Page::Reader);
        });
        connect(netBtn, &QPushButton::clicked, this, [this] {
            RunNetProbe();
        });
        connect(next, &QPushButton::clicked, this, [this] { reader_->NextPage(); });
        connect(twoPage, &QPushButton::clicked, this, [this] { reader_->ToggleTwoPage(); });
        connect(dirBtn, &QPushButton::clicked, this, [this] { reader_->ToggleDirection(); });
        connect(bright, &QPushButton::clicked, this, [this] { reader_->CycleBrightness(); });

        connect(homeBtn_, &QPushButton::clicked, this, [this] { Push(Page::Home); });
        connect(readerBtn_, &QPushButton::clicked, this, [this] {
            reader_->LoadDefaultOrPick();
            Push(Page::Reader);
        });
        connect(settingsBtn_, &QPushButton::clicked, this, [this] { Push(Page::Settings); });
        connect(back_, &QPushButton::clicked, this, [this] { Pop(); });

        connect(reader_, &ReaderView::PageChanged, this, [this](int cur, int total, const QString &path) {
            if (total > 0)
                SetTitle(QStringLiteral("Reader %1/%2").arg(cur).arg(total));
            else
                SetTitle(QStringLiteral("Reader (no pages)"));
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
            SetTitle(QStringLiteral("Reader"));
            break;
        case Page::Settings:
            SetTitle(QStringLiteral("Settings"));
            break;
        default:
            SetTitle(QStringLiteral("Home"));
        }
    }

    void SetTitle(const QString &text)
    {
        title_->setText(text);
    }

    void UpdateNavEnabled()
    {
        back_->setEnabled(!history_.isEmpty());
    }

    QStackedWidget *stack_ = nullptr;
    ReaderView *reader_ = nullptr;
    SettingsPage *settings_ = nullptr;
    NetClient *net_ = nullptr;
    QLabel *title_ = nullptr;
    QLabel *status_ = nullptr;
    QPushButton *back_ = nullptr;
    QPushButton *homeBtn_ = nullptr;
    QPushButton *readerBtn_ = nullptr;
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
    win.MaybeRunSelfTest();
    return app.exec();
}
