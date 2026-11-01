#include <QApplication>
#include <QLabel>
#include <QMainWindow>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    QMainWindow win;
    win.setWindowTitle(QStringLiteral("C0 skeleton"));

    auto *central = new QWidget;
    auto *lay = new QVBoxLayout(central);
    auto *label = new QLabel(QStringLiteral("C0 skeleton\nQt 6.12 C++ / 16KB gates"), central);
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
