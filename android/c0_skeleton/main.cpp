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

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "C0Skeleton", __VA_ARGS__)

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    // --- C0-③ 验证：libsr_qnn 随主程序 DT_NEEDED 加载，这里只确认其常驻 ---
    QString srqLine;
    if (void *h = dlopen("libsr_qnn.so", RTLD_NOLOAD | RTLD_NOW)) {
        srqLine = QStringLiteral("libsr_qnn.so loaded: YES (handle %1)").arg(reinterpret_cast<qintptr>(h), 0, 16);
        LOGI("sr_qnn direct-link OK, handle=%p", h);
        dlclose(h);
    } else {
        const char *e = dlerror();
        srqLine = QStringLiteral("libsr_qnn.so loaded: NO (%1)").arg(QString::fromLocal8Bit(e ? e : "unknown"));
        LOGI("sr_qnn direct-link FAIL: %s", e ? e : "unknown");
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
