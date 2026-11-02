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

#include <android/log.h>
#include <dlfcn.h>
#include <link.h>

#include "reader.h"
#include "settings_page.h"
#include "python_bridge.h"
#include "pyruntime.h"

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

// P4：桥脚本（bridge.py + net_client.py）随 APK 进 assets/，首次使用时复制到应用私有
// 目录。此前 build_wsl.sh 没有打包这两个文件，main.cpp 便退化成一段内联 echo 脚本，
// 于是"Android 端桥能跑"验的其实是桩代码——这里把退路去掉，缺文件就明确报错。
bool CopyBridgeAsset(const QString &name, const QString &dst)
{
    static const QStringList bases = {QStringLiteral("assets:/"),
                                      QStringLiteral("assets:/pylibs/")};
    for (const QString &base : bases) {
        const QString src = base + name;
        if (QFile::exists(src))
            return QFile::copy(src, dst);
    }
    return false;
}

QString StageBridgeScripts(QString *err)
{
    const QString dir = QStandardPaths::writableLocation(
                            QStandardPaths::AppLocalDataLocation)
                        + QStringLiteral("/bridge");
    if (!QDir().mkpath(dir)) {
        if (err)
            *err = QStringLiteral("mkdir failed: %1").arg(dir);
        return QString();
    }
    const QStringList names = {QStringLiteral("bridge.py"), QStringLiteral("net_client.py")};
    for (const QString &name : names) {
        const QString dst = dir + QLatin1Char('/') + name;
        if (QFile::exists(dst) && QFileInfo(dst).size() > 0)
            continue;  // 已解出且非空：复用，避免每次启动重写
        QFile::remove(dst);
        if (!CopyBridgeAsset(name, dst)) {
            if (err)
                *err = QStringLiteral("asset missing: %1").arg(name);
            return QString();
        }
        QFile::setPermissions(dst, QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                       | QFileDevice::ReadGroup | QFileDevice::ReadOther);
    }
    return dir + QStringLiteral("/bridge.py");
}

// HTTP 结果压缩成一行，给状态栏与 logcat 共用（P4 验收要看得到 http_status/耗时）
QString DescribeNetResult(const QJsonObject &obj)
{
    if (!obj.value(QStringLiteral("ok")).toBool()) {
        const QJsonObject e = obj.value(QStringLiteral("error")).toObject();
        return QStringLiteral("net ERR %1: %2 (attempts=%3, %4ms)")
            .arg(e.value(QStringLiteral("code")).toString(),
                 e.value(QStringLiteral("message")).toString())
            .arg(e.value(QStringLiteral("attempts")).toInt())
            .arg(e.value(QStringLiteral("elapsed_ms")).toInt());
    }
    const QJsonObject r = obj.value(QStringLiteral("result")).toObject();
    return QStringLiteral("net OK %1 %2B %3ms%4")
        .arg(r.value(QStringLiteral("http_status")).toInt())
        .arg(r.value(QStringLiteral("body_len")).toInt())
        .arg(r.value(QStringLiteral("elapsed_ms")).toInt())
        .arg(r.value(QStringLiteral("http_error")).toBool() ? QStringLiteral(" (http_error)")
                                                            : QString());
}

// W4：页面栈 + 历史返回栈。Push 记录来源页，Pop 回退；标题栏跟随当前页。
class ShellWindow : public QMainWindow{
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

        bridge_ = new PythonBridge(central);
        connect(bridge_, &PythonBridge::Response, this, [this](const QJsonObject &obj) {
            // 协议 v1 回带 method：早期只按 id 关联时这里永远拿到空串，pong 便永远认不出来
            const QString method = obj.value(QStringLiteral("method")).toString();
            const bool ok = obj.value(QStringLiteral("ok")).toBool();
            LOGI("bridge response id=%d method=%s ok=%s",
                 obj.value(QStringLiteral("id")).toInt(),
                 qPrintable(method), ok ? "true" : "false");
            if (method == QStringLiteral("ping")) {
                SetTitle(QStringLiteral("bridge pong"));
            } else if (method == QStringLiteral("net.request")) {
                const QString line = DescribeNetResult(obj);
                status_->setText(line);
                LOGI("%s", qPrintable(line));
            } else if (method == QStringLiteral("net.status")) {
                const QJsonObject r = obj.value(QStringLiteral("result")).toObject();
                const QString line = QStringLiteral("py %1 rss=%2KB up=%3ms")
                                         .arg(r.value(QStringLiteral("python")).toString())
                                         .arg(r.value(QStringLiteral("rss_kb")).toInt())
                                         .arg(r.value(QStringLiteral("uptime_ms")).toInt());
                status_->setText(line);
                LOGI("%s", qPrintable(line));
            }
        });
        connect(bridge_, &PythonBridge::Error, this, [this](const QString &text) {
            status_->setText(QStringLiteral("bridge: %1").arg(text));
            LOGI("bridge error: %s", qPrintable(text));
        });
        connect(bridge_, &PythonBridge::Crashed, this, [this] {
            status_->setText(QStringLiteral("bridge crashed; restarting"));
            LOGI("bridge crashed; restarting");
        });

        connect(openReader, &QPushButton::clicked, this, [this] {
            reader_->LoadDefaultOrPick();
            Push(Page::Reader);
        });
        connect(netBtn, &QPushButton::clicked, this, [this] {
            QString err;
            if (!StartBridge(&err)) {
                status_->setText(QStringLiteral("bridge: %1").arg(err));
                LOGI("net test: bridge start failed: %s", qPrintable(err));
                return;
            }
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
        MaybeRunPythonSelfTest();
        MaybeRunNetSelfTest();
    }

    // 起桥：Python 解释器 + 真实 bridge.py（含 net_client.py）。失败必须说出来，
    // 不允许退化成内联 echo 桩——否则"Android 端桥能跑"验的是桩，不是真代码。
    bool StartBridge(QString *err)
    {
        QString python;
#ifdef C1_HAVE_PY_RUNTIME
        if (!PyRuntime::EnsureReady(&python, err))
            return false;
#else
        python = qEnvironmentVariable("JM_C1_PYTHON");
        if (python.isEmpty())
            python = QStandardPaths::findExecutable(QStringLiteral("python3"));
        if (python.isEmpty()) {
            if (err)
                *err = QStringLiteral("no python runtime");
            return false;
        }
#endif
        QString script = qEnvironmentVariable("JM_C1_BRIDGE");
        if (script.isEmpty())
            script = StageBridgeScripts(err);
        if (script.isEmpty())
            return false;
        if (!bridge_->Start(python, {script})) {
            if (err)
                *err = bridge_->GetLastError();
            return false;
        }
        LOGI("bridge started: %s %s", qPrintable(python), qPrintable(script));
        return true;
    }

    // P4：发一次真实请求，URL 可由 JM_C1_NET_URL 覆盖（默认取一个稳定的小页面）
    void RunNetProbe()
    {
        const QString url = qEnvironmentVariable("JM_C1_NET_URL",
                                                 QStringLiteral("https://www.baidu.com/"));
        LOGI("net probe: GET %s", qPrintable(url));
        QTimer::singleShot(350, this, [this, url] {
            if (!bridge_->NetRequest(QStringLiteral("GET"), url, {}, 15000)) {
                status_->setText(QStringLiteral("net: %1").arg(bridge_->GetLastError()));
                LOGI("net probe send failed: %s", qPrintable(bridge_->GetLastError()));
            }
        });
    }

    // W6 冒烟：run-as 建 `python_selftest` 标记即可触发；设备上没有 Python 时会明确失败
    void MaybeRunPythonSelfTest()
    {
        const QString marker = QStandardPaths::writableLocation(
                                   QStandardPaths::AppLocalDataLocation)
                               + QStringLiteral("/python_selftest");
        if (!QFile::exists(marker))
            return;
        QFile::remove(marker);
        LOGI("python selftest marker found");
        QString err;
        if (!StartBridge(&err)) {
            LOGI("python selftest: bridge start failed: %s", qPrintable(err));
            status_->setText(QStringLiteral("bridge: %1").arg(err));
            return;
        }
        // 若同时存在 net_selftest 标记，它的 net.request 已经证明桥可用；再发 ping 只会撞上
        // 单通道保护（日志里多一条 "bridge busy"，让取证者误以为是缺陷）。
        const QString netMarker = QStandardPaths::writableLocation(
                                      QStandardPaths::AppLocalDataLocation)
                                  + QStringLiteral("/net_selftest");
        if (QFile::exists(netMarker)) {
            LOGI("python selftest: net_selftest 同时存在，跳过 ping（由 net.request 覆盖）");
            return;
        }
        QTimer::singleShot(600, this, [this] { bridge_->SendRequest(QStringLiteral("ping"), {}); });
    }

    // P4 冒烟：run-as 建 `net_selftest` 标记即可触发一次真实网络请求 + net.status
    void MaybeRunNetSelfTest()
    {
        const QString marker = QStandardPaths::writableLocation(
                                   QStandardPaths::AppLocalDataLocation)
                               + QStringLiteral("/net_selftest");
        if (!QFile::exists(marker))
            return;
        QFile::remove(marker);
        LOGI("net selftest marker found");
        QString err;
        if (!StartBridge(&err)) {
            LOGI("net selftest: bridge start failed: %s", qPrintable(err));
            status_->setText(QStringLiteral("bridge: %1").arg(err));
            return;
        }
        RunNetProbe();
        QTimer::singleShot(16000, this, [this] { bridge_->NetStatus(); });
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
    PythonBridge *bridge_ = nullptr;
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
    QApplication app(argc, argv);
    ShellWindow win;
    win.show();
    win.MaybeRunSelfTest();
    return app.exec();
}
