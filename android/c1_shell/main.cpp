// C1-W2：Qt 6.12 C++ 壳 —— 导航 + 本地阅读器骨架。
//
// W1：可出包 + 16KB 门禁 0 未对齐 + 同 C0 的空壳级启动；
// W2：本地阅读器（打开本地目录 → 翻页 → 缩放），保持 libsr_qnn.so 直链不退回 ctypes。
#include <QApplication>
#include <QFile>
#include <QLabel>
#include <QMainWindow>
#include <QPushButton>
#include <QStandardPaths>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QHBoxLayout>

#include <android/log.h>
#include <dlfcn.h>
#include <link.h>

#include "reader.h"

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

// W4 会替换成完整的返回栈/标题栏/侧栏；这里只保证"页面栈 + 返回"可用。
class ShellWindow : public QMainWindow
{
public:
    ShellWindow()
    {
        setWindowTitle(QStringLiteral("C1 shell"));
        resize(540, 960);

        auto *central = new QWidget(this);
        auto *root = new QVBoxLayout(central);

        title_ = new QLabel(QStringLiteral("C1 shell / W2"), central);
        title_->setAlignment(Qt::AlignCenter);
        root->addWidget(title_);

        stack_ = new QStackedWidget(central);
        root->addWidget(stack_, 1);

        // 首页：入口占位
        auto *home = new QWidget(central);
        auto *homeLay = new QVBoxLayout(home);
        auto *openReader = new QPushButton(QStringLiteral("Open local reader"), home);
        homeLay->addWidget(openReader);
        homeLay->addStretch(1);
        stack_->addWidget(home);

        // 阅读页：W2 骨架（目录 → 图片列表 → 翻页/缩放）
        reader_ = new ReaderView(central);
        stack_->addWidget(reader_);

        auto *bar = new QHBoxLayout();
        auto *back = new QPushButton(QStringLiteral("Back"), central);
        auto *next = new QPushButton(QStringLiteral("Next page"), central);
        auto *twoPage = new QPushButton(QStringLiteral("2-up"), central);
        auto *dirBtn = new QPushButton(QStringLiteral("RTL"), central);
        auto *bright = new QPushButton(QStringLiteral("Dim"), central);
        bar->addWidget(back);
        bar->addWidget(next);
        bar->addWidget(twoPage);
        bar->addWidget(dirBtn);
        bar->addWidget(bright);
        root->addLayout(bar);

        status_ = new QLabel(ProbeSrq(), central);
        status_->setAlignment(Qt::AlignCenter);
        root->addWidget(status_);

        connect(openReader, &QPushButton::clicked, this, [this] {
            reader_->LoadDefaultOrPick();
            Push(1);
        });
        connect(next, &QPushButton::clicked, this, [this] { reader_->NextPage(); });
        connect(twoPage, &QPushButton::clicked, this, [this] { reader_->ToggleTwoPage(); });
        connect(dirBtn, &QPushButton::clicked, this, [this] { reader_->ToggleDirection(); });
        connect(bright, &QPushButton::clicked, this, [this] { reader_->CycleBrightness(); });
        connect(back, &QPushButton::clicked, this, [this] { Pop(); });
        connect(reader_, &ReaderView::PageChanged, this, [this](int cur, int total, const QString &path) {
            if (total > 0)
                title_->setText(QStringLiteral("Reader %1/%2").arg(cur).arg(total));
            else
                title_->setText(QStringLiteral("Reader (no pages)"));
            if (!path.isEmpty())
                LOGI("page %d/%d %s", cur, total, qPrintable(path));
        });
        connect(reader_, &ReaderView::StatusMessage, this, [this](const QString &text) {
            status_->setText(text);
            LOGI("%s", qPrintable(text));
        });
        connect(reader_, &ReaderView::SelfTestDone, this, [this] {
            title_->setText(QStringLiteral("selftest done"));
        });
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
            return;
        }
        QFile::remove(marker);
        LOGI("selftest marker found: %s", qPrintable(marker));
        Push(1);
        QTimer::singleShot(200, this, [this] { reader_->RunSelfTest(); });
    }

private:
    void Push(int index)
    {
        if (index < 0 || index >= stack_->count())
            return;
        stack_->setCurrentIndex(index);
    }

    void Pop()
    {
        if (stack_->currentIndex() > 0)
            stack_->setCurrentIndex(0);
    }

    QStackedWidget *stack_ = nullptr;
    ReaderView *reader_ = nullptr;
    QLabel *title_ = nullptr;
    QLabel *status_ = nullptr;
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
