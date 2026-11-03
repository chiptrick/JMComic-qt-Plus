// C3 端到端（真网）：宿主端直接打禁漫移动端接口，验证
//   token/tokenparam 生成 -> HTTPS -> 响应 envelope -> AES 解密 -> JSON 字段解析
// 全链路。失败即说明"网络收编"还没通，不依赖模拟器/真机。
//
// 用法：host_test_jm_api [keyword]
// 退出码 0 = 拿到 >=1 条搜索结果且字段可读。
#include "jm_api.h"
#include "net_client.h"

#include <QCoreApplication>
#include <QTimer>

#include <cstdio>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    const QString keyword = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QStringLiteral("test");
    NetClient net;
    JmApi api(&net);
    QObject::connect(&api, &JmApi::Log, [](const QString &line) {
        std::printf("[api] %s\n", qPrintable(line));
    });

    int rc = 1;
    QObject::connect(&api, &JmApi::SearchReady,
                     [&](bool ok, const QVector<JmComicItem> &items, int total, int page,
                         const QString &error) {
                         if (!ok) {
                             std::printf("[FAIL] search: %s\n", qPrintable(error));
                             rc = 2;
                             app.quit();
                             return;
                         }
                         std::printf("[ok]   search '%s' page=%d hits=%d total=%d\n",
                                     qPrintable(keyword), page, int(items.size()), total);
                         for (int i = 0; i < items.size() && i < 3; ++i) {
                             std::printf("       #%s %s | %s | %s\n", qPrintable(items[i].id),
                                         qPrintable(items[i].name.left(48)),
                                         qPrintable(items[i].author.left(24)),
                                         qPrintable(items[i].category));
                         }
                         rc = items.isEmpty() ? 3 : 0;
                         app.quit();
                     });

    QTimer::singleShot(0, &api, [&] { api.FetchSetting(); api.Search(keyword, 1); });
    QTimer::singleShot(60000, &app, [&] {
        std::printf("[FAIL] timeout waiting for search\n");
        rc = 4;
        app.quit();
    });

    app.exec();
    std::printf(rc == 0 ? "\nALL PASS\n" : "\nFAILED (rc=%d)\n", rc);
    return rc;
}
