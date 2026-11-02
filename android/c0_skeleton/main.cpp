// C0-②' + C0-③：Qt 6.12 C++ 空壳，附带 libsr_qnn 直链验证。
// C0-③ 判据：c0skeleton 直接链接 libsr_qnn.so（去 ctypes），启动后能确认它已被
// Android 17(16KB) 的 linker 无缝加载（RTLD_NOLOAD 非空），与 Qt 库无符号冲突。
#include <QApplication>
#include <QLabel>
#include <QMainWindow>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

#include <android/log.h>
#include <dlfcn.h>
#include <link.h>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "C0Skeleton", __VA_ARGS__)

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    // --- C0-③ 验证 ---
    // 判据分两层，避免"同名库被别处装载"造成的假绿：
    //   ① linux 侧（更强）：构建脚本用 llvm-readelf 确认 c0skeleton 自己的 .so
    //      DT_NEEDED 里有 libsr_qnn.so —— 这才叫"直链"；
    //   ② 设备侧（本函数）：用 dl_iterate_phdr 在**进程 link map** 里找到 libsr_qnn.so
    //      并取出其基址；RTLD_NOLOAD 的 dlopen 只作补充（它不区分是谁加载的）。
    struct SrqProbe {
        bool found = false;
        QString path;
        void *base = nullptr;
    } probe;

    dl_iterate_phdr([](struct dl_phdr_info *info, size_t, void *data) -> int {
        auto *p = static_cast<SrqProbe *>(data);
        const char *name = info->dlpi_name ? info->dlpi_name : "";
        if (QString::fromLocal8Bit(name).endsWith(QLatin1String("libsr_qnn.so"))) {
            p->found = true;
            p->path = QString::fromLocal8Bit(name);
            p->base = reinterpret_cast<void *>(info->dlpi_addr);
            return 1; // 已找到，停止遍历
        }
        return 0;
    }, &probe);

    QString srqLine;
    if (probe.found) {
        srqLine = QStringLiteral("libsr_qnn.so in link map: YES (base %1)")
                      .arg(reinterpret_cast<quintptr>(probe.base), 0, 16);
        LOGI("sr_qnn in link map OK, base=%p path=%s", probe.base, qPrintable(probe.path));
    } else {
        // ② 层没命中时再看 ① 是否被破坏（例如被 strip 掉 NEEDED / 未真正链接）
        void *h = dlopen("libsr_qnn.so", RTLD_NOLOAD | RTLD_NOW);
        srqLine = QStringLiteral("libsr_qnn.so in link map: NO (dlopen: %1)")
                      .arg(h ? QStringLiteral("handle-only") : QStringLiteral("absent"));
        LOGI("sr_qnn link map FAIL, dlopen=%p err=%s", h, h ? "-" : dlerror());
        if (h)
            dlclose(h);
    }

    QMainWindow win;
    win.setWindowTitle(QStringLiteral("C0 skeleton"));

    auto *central = new QWidget;
    auto *lay = new QVBoxLayout(central);
    auto *label = new QLabel(QStringLiteral("C0 skeleton\nQt 6.12 C++ / 16KB gates\n%1").arg(srqLine), central);
    label->setAlignment(Qt::AlignCenter);
    auto *btn = new QPushButton(QStringLiteral("About"), central);
    QObject::connect(btn, &QPushButton::clicked, &win, [&win] {
        QMessageBox::information(&win, QStringLiteral("C0"),
                                 QStringLiteral("Qt 6.12.0 C++ on Android"));
    });
    lay->addWidget(label);
    lay->addWidget(btn);
    win.setCentralWidget(central);
    win.resize(540, 960);
    win.show();

    return app.exec();
}
