// C2/C3：在线浏览（搜索 → 详情 → 章节 → 阅读）。
//
// 这一页是"Python 网络子进程可以删掉"的证据链：整条链路（搜索、详情、章节、图片）
// 全部由 C++ 进程内完成，没有 Python、没有子进程、没有 JSON 桥。
//
// 图片处理三个必踩的点（都已在 Python 版踩过）：
//   1. 图片是 webp（不是 jpg），必须用 Qt 的 imageformats 插件解码；
//   2. 每张图按 scramble_id 被切成 N 条横带上下颠倒，必须先还原再显示；
//   3. 还原要按"章节 id + 文件名"算 N，不是按相册 id —— 算错就是"图能开但错位"。
#pragma once

#include <QStringList>
#include <QVector>
#include <QWidget>

#include "jm_api.h"   // JmAlbum/JmChapterDetail 是成员（值语义），必须完整类型

class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QStackedWidget;
class NetClient;

class BrowsePage : public QWidget
{
    Q_OBJECT
public:
    explicit BrowsePage(NetClient *net, QWidget *parent = nullptr);

    // 冒烟：搜索关键字 → 打开第一个结果 → 打开第一章 → 下载前 pages 页
    void RunSelfTest(const QString &keyword = QStringLiteral("test"), int pages = 3);
    // 直接按 id 打开（纯数字输入时走这条）
    void OpenAlbum(const QString &albumId);

    QString LastSummary() const { return lastSummary_; }

signals:
    void StatusMessage(const QString &text);
    void OpenReaderRequested(const QStringList &files, const QString &title);
    void SelfTestDone(const QString &summary);

private:
    void BuildUi();
    void OnSearchClicked();
    void ShowResults(const QVector<JmComicItem> &items, int total);
    void ShowAlbum(const JmAlbum &album);
    void ShowChapter(const JmChapterDetail &chapter);
    void DownloadNext();
    void Fail(const QString &stage, const QString &error);

    NetClient *net_ = nullptr;
    JmApi *api_ = nullptr;

    QStackedWidget *inner_ = nullptr;
    QLineEdit *keyword_ = nullptr;
    QPushButton *goBtn_ = nullptr;
    QListWidget *results_ = nullptr;
    QLabel *detailTitle_ = nullptr;
    QLabel *detailMeta_ = nullptr;
    QListWidget *chapters_ = nullptr;
    QPushButton *readBtn_ = nullptr;
    QPushButton *backBtn_ = nullptr;
    QLabel *status_ = nullptr;

    JmAlbum album_;
    JmChapterDetail chapter_;
    QStringList pendingUrls_;
    QStringList savedFiles_;
    QString cacheDir_;
    int downloadIndex_ = 0;
    int pageLimit_ = 0;
    bool imageRetried_ = false;   // 每个章节只换一次 CDN 域名，避免来回打转
    bool selftest_ = false;
    QString lastSummary_;
};
