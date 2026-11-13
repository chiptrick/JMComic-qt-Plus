// 下载队列：章节级任务、断点续传、失败重试与状态持久化。
//
// 为什么单独一层（而不是塞进 DetailPage）：
//   * 下载是长任务：要能离开详情页、切到别的页面继续跑；
//   * 一个章节几十页，必须限并发（3）、单页失败换 CDN 重试，失败不能拖垮整章；
//   * 用户中途退出/被杀进程后要能看出"哪些下到一半"，并且能继续，而不是
//     留下一个永远转圈的假任务。
//
// 落盘布局（Root() 默认 <AppData>/download，可在设置页改）：
//   <root>/<作品名>/<章节名>/00001.jpg ...
//   <root>/<作品名>/<章节名>/.jm-download.json   ← 任务状态（续传依据）
//
// 与 PageLoader 的分工：PageLoader 是"阅读器在线看图"的缓存，随时可丢；
// 这里写的是用户主动下载、要长期保留的文件，所以不共用目录，也不共用状态。
#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

class JmApi;
class QNetworkAccessManager;
class NodeHttp;

struct DownloadTask {
    enum State {
        Queued = 0,   // 排队中
        Fetching,     // 正在取章节页表
        Running,      // 正在下页图
        Paused,       // 用户暂停
        Done,         // 全部页完成
        Failed,       // 失败（有 error 文案，可重试）
        Interrupted,  // 上次进程退出时没跑完
    };

    QString key;          // "<albumId>/<chapterId>"，队列内唯一
    QString albumId;
    QString albumTitle;
    QString chapterId;
    QString chapterName;
    QString coverUrl;
    QString dir;          // 落盘目录（绝对路径）
    int total = 0;        // 总页数（拿到 /chapter 后才有值）
    int done = 0;         // 已成功页数（含续传命中的）
    int failed = 0;       // 失败页数
    qint64 bytes = 0;     // 已落盘字节数
    State state = Queued;
    QString error;
    qint64 updatedAt = 0; // 秒

    bool Active() const { return state == Fetching || state == Running; }
    QString StateName() const;
    // 0~100；total 未定时返回 0
    int Percent() const { return total > 0 ? int(100.0 * done / total) : 0; }
};

class DownloadManager : public QObject
{
    Q_OBJECT
public:
    explicit DownloadManager(JmApi *api, QObject *parent = nullptr);

    // 下载根目录（空 = 默认）。SetRoot 会建目录并写 ui.ini。
    static QString Root();
    static bool SetRoot(const QString &dir);
    static QString DefaultRoot();

    // 启动时从磁盘恢复：没跑完的任务标成 Interrupted，已完成的保持 Done。
    void LoadPersisted();

    // 入队：一次可给多章（详情页"下载全部"）。重复入队会合并（已有任务不动）。
    void EnqueueChapters(const QString &albumId, const QString &albumTitle,
                         const QStringList &chapterIds, const QStringList &chapterNames,
                         const QString &coverUrl);

    // 每条任务的操作
    void Pause(const QString &key);
    void Resume(const QString &key);
    void Retry(const QString &key);
    void Remove(const QString &key, bool deleteFiles);
    void PauseAll();
    void ResumeAll();

    QVector<DownloadTask> Tasks() const { return tasks_; }
    int IndexOf(const QString &key) const;
    DownloadTask Task(const QString &key) const;
    bool Busy() const { return !activeKey_.isEmpty(); }
    QString ActiveKey() const { return activeKey_; }
    // 一行 ASCII 状态（--dl-report / 真机取证）
    QString Report() const;
    // 等所有任务跑完（CLI 自检用）；超时毫秒
    bool WaitIdle(int timeoutMs);

    // 退出前调用：把内存里的进度写回磁盘（正在跑的任务标 Interrupted）
    void Shutdown();

signals:
    // 列表/进度变化（高频，调用方自己做节流/合并）
    void Changed();
    void TaskFinished(const QString &key, bool ok, const QString &message);
    void Log(const QString &line);

private:
    void SaveTask(const DownloadTask &t);
    void Publish();
    void Pump();
    void StartTask(const QString &key);
    void FetchPageList(const DownloadTask &t);
    void OnChapterReady(const QString &chapterId);
    void QueuePages();
    void RequestPage(int index, int attempt);
    void HandlePage(int index, bool netError, int status, const QByteArray &data,
                    const QString &errText, int attempt);
    void FinishTask(bool ok, const QString &message);
    void StopActive(const QString &reason, DownloadTask::State state);
    QString OutPath(const DownloadTask &t, int index) const;
    static QString WithImageDomain(const QString &url, const QString &domain);

    JmApi *api_ = nullptr;
    QNetworkAccessManager *nam_ = nullptr;
    NodeHttp *nodeHttp_ = nullptr;

    QVector<DownloadTask> tasks_;
    QString activeKey_;
    int generation_ = 0;

    // 正在执行的这一章
    QString chapterId_;
    QString photoId_;
    QString scrambleId_;
    QStringList urls_;
    QStringList names_;
    QStringList results_;
    QVector<int> queue_;
    QVector<int> running_;
    int settled_ = 0;
    int doneThisRun_ = 0;
    QString firstError_;
    qint64 bytesThisRun_ = 0;
    bool pageListArrived_ = false;
};
