// C3 端到端（真网）：宿主端直接打禁漫移动端接口，验证
//   token/tokenparam 生成 -> HTTPS -> 响应 envelope -> AES 解密 -> JSON 字段解析
// 全链路。失败即说明"网络收编"还没通，不依赖模拟器/真机。
//
// 用法：host_test_jm_api [keyword]
//       host_test_jm_api --parse-login     # 离线：/login 明文字段映射（不联网）
// 退出码 0 = 拿到 >=1 条搜索结果且字段可读 / 解析断言全过。
#include "jm_api.h"
#include "net_client.h"

#include <QCoreApplication>
#include <QTimer>

#include <cstdio>

namespace {

// 返回 0=通过 / 1=失败，调用方直接累加即可。
int CheckInt(const char *what, int got, int want)
{
    const bool ok = got == want;
    std::printf("%s %s: got=%d want=%d\n", ok ? "[ok]  " : "[FAIL]", what, got, want);
    return ok ? 0 : 1;
}

// 登录响应（解密后的 data 段）→ JmUserInfo 的字段映射。
// 之前 C++ 只从里面取 username/uid，把 coin/level/album_favorites 丢掉，
// 界面就只能硬编码 0——这组断言就是钉住"一个字段都不能丢"。
int RunParseLoginTest()
{
    int fails = 0;
    {
        const QByteArray full = R"({"uid":18240,"username":"misaka18240",
            "level_name":"Lv.3","level":3,"coin":120,"album_favorites":7,
            "album_favorites_max":500,"exp":30,"nextLevelExp":100,"s":"avs-token"})";
        const JmUserInfo u = JmApi::ParseLoginPayload(full);
        std::printf("payload#1 name=%s title=%s\n", qPrintable(u.name),
                    qPrintable(u.levelName));
        fails += CheckInt("uid", int(u.uid), 18240);
        fails += CheckInt("level", u.level, 3);
        fails += CheckInt("coin", u.coins, 120);
        fails += CheckInt("album_favorites", u.favorites, 7);
        fails += CheckInt("album_favorites_max", u.favoritesMax, 500);
        fails += CheckInt("exp", u.exp, 30);
        fails += CheckInt("nextLevelExp", u.nextExp, 100);
        fails += CheckInt("hasProfile", int(u.hasProfile), 1);
        const bool nameOk = u.name == QStringLiteral("misaka18240")
                            && u.levelName == QStringLiteral("Lv.3");
        std::printf("%s name/level_name\n", nameOk ? "[ok]  " : "[FAIL]");
        fails += nameOk ? 0 : 1;
    }
    {
        // 不同账号类型的数字是字符串：不能解成 0
        const QByteArray str = R"({"uid":"777","username":"strUser","level":"2",
            "coin":"45","album_favorites":"9","album_favorites_max":"300",
            "exp":"1","nextLevelExp":"80"})";
        const JmUserInfo u = JmApi::ParseLoginPayload(str);
        fails += CheckInt("str uid", int(u.uid), 777);
        fails += CheckInt("str level", u.level, 2);
        fails += CheckInt("str coin", u.coins, 45);
        fails += CheckInt("str fav", u.favorites, 9);
        fails += CheckInt("str favMax", u.favoritesMax, 300);
    }
    {
        // 只有会话票据：hasProfile=false，UI 必须保留缓存值而不是写 0
        const JmUserInfo u = JmApi::ParseLoginPayload(R"({"s":"only-token"})");
        fails += CheckInt("minimal hasProfile", int(u.hasProfile), 0);
    }
    std::printf(fails == 0 ? "\nALL PASS\n" : "\nFAILED (%d)\n", fails);
    return fails == 0 ? 0 : 5;
}

// 详情/章节的离线解析断言：钉住"单章节作品兜底"与"页图按真实文件名拼"。
// 这两个点都出过线上可见的问题：
//   * /album 的 series 为空（整本一张 photo）时不能返回 0 章，否则详情页只剩"暂无章节"；
//   * /chapter 的 images 编号有洞（00027 缺号、末项 00028）时不能按序号生成 URL。
int RunParseAlbumTest()
{
    int fails = 0;
    {
        // 实测 1471982：series=[]、total_photos=13、series_id="0"
        const QByteArray single = R"({"id":"1471982","name":"摸鱼图","author":["kuzlin"],
            "description":"d","tags":["a","b"],"series":[],"images":["00001.webp"],
            "total_photos":13,"series_id":"0"})";
        const JmAlbum a = JmApi::ParseAlbumPayload(single, QStringLiteral("1471982"), QStringLiteral("cdn.example"));
        std::printf("single: id=%s chapters=%d first=%s photos=%d\n", qPrintable(a.id),
                    int(a.chapters.size()),
                    a.chapters.isEmpty() ? "-" : qPrintable(a.chapters.first().id), a.totalPhotos);
        fails += CheckInt("single chapters", int(a.chapters.size()), 1);
        fails += CheckInt("single totalPhotos", a.totalPhotos, 13);
        const bool idOk = !a.chapters.isEmpty()
                          && a.chapters.first().id == QStringLiteral("1471982")
                          && a.chapters.first().name.isEmpty();
        std::printf("%s single chapter id == album id\n", idOk ? "[ok]  " : "[FAIL]");
        fails += idOk ? 0 : 1;
    }
    {
        // 多章节：series 原样映射，顺序与 sort 都不改写
        const QByteArray multi = R"({"id":"313833","name":"n","author":"x",
            "series":[{"id":"313833","name":"","sort":"1"},
                      {"id":"900001","name":"2","sort":"2"}],
            "total_photos":0})";
        const JmAlbum a = JmApi::ParseAlbumPayload(multi, QStringLiteral("313833"), QStringLiteral("cdn.example"));
        fails += CheckInt("multi chapters", int(a.chapters.size()), 2);
        const bool orderOk = a.chapters.size() == 2
                             && a.chapters.at(0).id == QStringLiteral("313833")
                             && a.chapters.at(1).id == QStringLiteral("900001")
                             && a.chapters.at(1).name == QStringLiteral("2");
        std::printf("%s multi chapters order/id\n", orderOk ? "[ok]  " : "[FAIL]");
        fails += orderOk ? 0 : 1;
    }
    {
        // 响应缺 id：用请求参数回填，保证详情页的"切作品"判定与单章兜底仍然成立
        const JmAlbum a = JmApi::ParseAlbumPayload(R"({"name":"n","series":[]})",
                                                  QStringLiteral("555"), QStringLiteral("cdn.example"));
        const bool ok = a.id == QStringLiteral("555") && a.chapters.size() == 1
                        && a.chapters.first().id == QStringLiteral("555");
        std::printf("%s album id fallback\n", ok ? "[ok]  " : "[FAIL]");
        fails += ok ? 0 : 1;
    }
    {
        // 页图：编号有洞时按真实文件名拼，不做 00001..N 的假设
        const QByteArray chap = R"({"id":"313833","name":"n",
            "series":[{"id":"313833","name":"","sort":"1"}],
            "images":["00001.webp","00026.webp","00028.webp"]})";
        const JmChapterDetail c = JmApi::ParseChapterPayload(
            chap, QStringLiteral("313833"), QStringLiteral("cdn.example"));
        std::printf("chapter: pages=%d last=%s\n", int(c.images.size()),
                    c.images.isEmpty() ? "-" : qPrintable(c.images.last()));
        fails += CheckInt("chapter pages", int(c.images.size()), 3);
        const bool urlOk = c.images.size() == 3
                           && c.images.at(0).endsWith(QStringLiteral("/media/photos/313833/00001.webp"))
                           && c.images.at(2).endsWith(QStringLiteral("/media/photos/313833/00028.webp"));
        std::printf("%s chapter urls use real filenames\n", urlOk ? "[ok]  " : "[FAIL]");
        fails += urlOk ? 0 : 1;
        fails += CheckInt("chapter siblings", int(c.siblings.size()), 1);
    }
    {
        // 新版 images 是对象数组：取 image 字段，并剥掉可能的路径前缀
        const JmChapterDetail c = JmApi::ParseChapterPayload(
            R"({"id":"7","images":[{"page":1,"image":"/media/photos/7/00001.jpg"}],
                "series":[]})",
            QStringLiteral("7"), QStringLiteral("cdn.example"));
        const bool ok = c.images.size() == 1
                        && c.images.first().endsWith(QStringLiteral("/media/photos/7/00001.jpg"));
        std::printf("%s chapter object images\n", ok ? "[ok]  " : "[FAIL]");
        fails += ok ? 0 : 1;
    }
    {
        // 缺 id 的章节响应：用请求参数回填，PageLoader 的迟到判定才有依据
        const JmChapterDetail c = JmApi::ParseChapterPayload(
            R"({"images":[],"series":[]})", QStringLiteral("42"), QStringLiteral("cdn.example"));
        const bool ok = c.id == QStringLiteral("42") && c.images.isEmpty();
        std::printf("%s chapter id fallback\n", ok ? "[ok]  " : "[FAIL]");
        fails += ok ? 0 : 1;
    }
    std::printf(fails == 0 ? "\nALL PASS\n" : "\nFAILED (%d)\n", fails);
    return fails == 0 ? 0 : 6;
}

}  // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    if (argc > 1 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--parse-login"))
        return RunParseLoginTest();
    if (argc > 1 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--parse-album"))
        return RunParseAlbumTest();

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
