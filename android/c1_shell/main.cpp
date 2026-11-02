// C1-W1：Qt 6.12 C++ 壳工程骨架 —— 壳 + 导航占位。
//
// W1 的验收是"可出包 + 16KB 门禁 0 未对齐 + 同 C0 的空壳级启动"，
// 这里只放最小的导航结构（页面栈 + 返回栈桩），不做阅读器/设置（W2~W5）。
// 保留 C0-③ 的 libsr_qnn.so 直链验证：壳以后要直连超分后端，这条链路不能退回 ctypes。
#include <QApplication>
#include <QLabel>
#include <QMainWindow>
#include <QPushButton>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>

#include <android/log.h>
#include <dlfcn.h>
#include <link.h>

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

// 最小导航壳：W4 会替换成完整的返回栈/标题栏/侧栏；这里只保证"页面栈 + 返回"可用。
class ShellWindow : public QMainWindow
{
public:
    ShellWindow()
    {
        setWindowTitle(QStringLiteral("C1 shell"));
        resize(540, 960);

        auto *central = new QWidget(this);
        auto *root = new QVBoxLayout(central);

        auto *title = new QLabel(QStringLiteral("C1 shell / W1"), central);
        title->setAlignment(Qt::AlignCenter);
        root->addWidget(title);

        stack_ = new QStackedWidget(central);
        root->addWidget(stack_, 1);

        // 两个占位页：后续分别长成本地阅读器(W2~W3)与设置(W5)
        auto *home = new QLabel(QStringLiteral("Home page placeholder"), central);
        home->setAlignment(Qt::AlignCenter);
        stack_->addWidget(home);

        auto *reader = new QLabel(QStringLiteral("Local reader placeholder"), central);
        reader->setAlignment(Qt::AlignCenter);
        stack_->addWidget(reader);

        auto *bar = new QHBoxLayout();
        auto *back = new QPushButton(QStringLiteral("Back"), central);
        auto *next = new QPushButton(QStringLiteral("Open reader"), central);
        bar->addWidget(back);
        bar->addWidget(next);
        root->addLayout(bar);

        auto *status = new QLabel(ProbeSrq(), central);
        status->setAlignment(Qt::AlignCenter);
        root->addWidget(status);

        connect(next, &QPushButton::clicked, this, [this] { Push(1); });
        connect(back, &QPushButton::clicked, this, [this] { Pop(); });
        setCentralWidget(central);
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
};

}  // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    ShellWindow win;
    win.show();
    return app.exec();
}
