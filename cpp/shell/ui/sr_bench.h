// 模型列表测速的状态层（界面无关）。
//
// 为什么单独一层：
//   * 测速要跑几十秒到几分钟，必须放后台任务；浮层随时可能被关掉，
//     任务不能持有界面对象的指针，所以进度/结果统一放在这里的进程级存储里，
//     界面只做轮询渲染。
//   * 结果要求"始终保留"：切筛选、关浮层、切页都不丢，并且写进 ui.ini
//     （带硬件指纹；换显卡/换设备后旧数据自动作废）。
#pragma once

#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>

#include "../sr_engine.h"

namespace SrBench {

struct Sample {
    bool ok = false;
    double ms = 0.0;
    QString note;   // 失败原因
};

// 硬件清单（进程内缓存；ReloadHardware 可强制重枚举）
const QVector<SrEngine::BenchHw> &Hardware();
void ReloadHardware();
QString HardwareShort(const QString &id);
QString HardwareLabel(const QString &id);
bool HardwareAvailable(const QString &id);

// 测试结果（model -> hwId -> sample）
bool Has(const QString &model);
QHash<QString, Sample> For(const QString &model);
// 全量结果拷贝（界面按版本号拉取一次，避免每行都去抢锁）
QHash<QString, QHash<QString, Sample>> All();

struct Progress {
    bool running = false;
    bool cancelling = false;
    int done = 0;
    int total = 0;
    QString activeModel;
    QString activeHw;
    QStringList pending;   // 取消后未完成的模型（供"继续测试"）
    quint64 version = 0;   // 结果每变一次 +1，界面据此决定是否重绘
    QString lastSummary;   // 上一次跑完的摘要（"完成 46 个模型" / "已取消 · 完成 12/46"）
    QString signature;     // 硬件指纹
};
Progress Snapshot();

// 启动后台测速（只跑 available 的硬件）。models 为空或已有任务时返回 false。
bool Start(const QStringList &models);
void Cancel();
// 继续上次未完成的测试（pending 非空时）。
bool ResumePending();
void DismissPending();

void ClearResults();

// 首次使用时从 ui.ini 读取；签名不符则丢弃。
void EnsureLoaded();

}  // namespace SrBench
