#include "sr_bench.h"

#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QMutex>
#include <QMutexLocker>
#include <QStringList>
#include <QtConcurrent>

#include <algorithm>
#include <atomic>

#include "ui_prefs.h"

namespace {

struct Store {
    QMutex mutex;
    QHash<QString, QHash<QString, SrBench::Sample>> results;
    SrBench::Progress prog;
    std::atomic_bool cancel{false};
    bool loaded = false;
    bool hardwareLoaded = false;
    QVector<SrEngine::BenchHw> hardware;
};

Store &S()
{
    static Store s;
    return s;
}

QString SignatureOf(const QVector<SrEngine::BenchHw> &hw)
{
    QStringList parts;
    for (const SrEngine::BenchHw &h : hw)
        parts << (h.id + QLatin1Char(':') + h.label + (h.available ? QString() : QStringLiteral("(x)")));
    return parts.join(QLatin1Char('|'));
}

QJsonObject ToJson(const QHash<QString, QHash<QString, SrBench::Sample>> &results)
{
    QJsonObject out;
    for (auto it = results.constBegin(); it != results.constEnd(); ++it) {
        QJsonObject hwObj;
        for (auto hit = it.value().constBegin(); hit != it.value().constEnd(); ++hit) {
            QJsonObject s;
            s.insert(QStringLiteral("ok"), hit.value().ok);
            s.insert(QStringLiteral("ms"), hit.value().ms);
            if (!hit.value().note.isEmpty())
                s.insert(QStringLiteral("note"), hit.value().note);
            hwObj.insert(hit.key(), s);
        }
        out.insert(it.key(), hwObj);
    }
    return out;
}

QHash<QString, QHash<QString, SrBench::Sample>> FromJson(const QJsonObject &root)
{
    QHash<QString, QHash<QString, SrBench::Sample>> out;
    for (auto it = root.constBegin(); it != root.constEnd(); ++it) {
        if (!it.value().isObject())
            continue;
        QHash<QString, SrBench::Sample> hw;
        const QJsonObject obj = it.value().toObject();
        for (auto hit = obj.constBegin(); hit != obj.constEnd(); ++hit) {
            const QJsonObject s = hit.value().toObject();
            SrBench::Sample sample;
            sample.ok = s.value(QStringLiteral("ok")).toBool();
            sample.ms = s.value(QStringLiteral("ms")).toDouble();
            sample.note = s.value(QStringLiteral("note")).toString();
            hw.insert(hit.key(), sample);
        }
        if (!hw.isEmpty())
            out.insert(it.key(), hw);
    }
    return out;
}

const char *kPrefKey = "srBenchData";

}  // namespace

namespace SrBench {

const QVector<SrEngine::BenchHw> &Hardware()
{
    Store &s = S();
    QMutexLocker lock(&s.mutex);
    if (!s.hardwareLoaded) {
        s.hardwareLoaded = true;
        s.hardware = SrEngine::BenchHardware();
        s.prog.signature = SignatureOf(s.hardware);
    }
    return s.hardware;
}

void ReloadHardware()
{
    Store &s = S();
    QMutexLocker lock(&s.mutex);
    s.hardwareLoaded = true;
    s.hardware = SrEngine::BenchHardware();
    s.prog.signature = SignatureOf(s.hardware);
}

QString HardwareShort(const QString &id)
{
    for (const SrEngine::BenchHw &h : Hardware()) {
        if (h.id == id)
            return h.shortLabel;
    }
    return id;
}

QString HardwareLabel(const QString &id)
{
    for (const SrEngine::BenchHw &h : Hardware()) {
        if (h.id == id)
            return h.label;
    }
    return id;
}

bool HardwareAvailable(const QString &id)
{
    for (const SrEngine::BenchHw &h : Hardware()) {
        if (h.id == id)
            return h.available;
    }
    return false;
}

bool Has(const QString &model)
{
    Store &s = S();
    QMutexLocker lock(&s.mutex);
    return s.results.contains(model) && !s.results.value(model).isEmpty();
}

QHash<QString, Sample> For(const QString &model)
{
    Store &s = S();
    QMutexLocker lock(&s.mutex);
    return s.results.value(model);
}

QHash<QString, QHash<QString, Sample>> All()
{
    Store &s = S();
    QMutexLocker lock(&s.mutex);
    return s.results;
}

Progress Snapshot()
{
    Store &s = S();
    QMutexLocker lock(&s.mutex);
    return s.prog;
}

void EnsureLoaded()
{
    Store &s = S();
    {
        QMutexLocker lock(&s.mutex);
        if (s.loaded)
            return;
        s.loaded = true;
    }
    // 硬件清单要在锁外取（首次会建 Vulkan 实例/探测 QNN）
    const QString sig = SignatureOf(Hardware());
    const QString blob = UiPrefs::GetString(QString::fromLatin1(kPrefKey));
    if (blob.isEmpty())
        return;
    const QJsonObject root = QJsonDocument::fromJson(QByteArray::fromBase64(blob.toUtf8())).object();
    if (root.value(QStringLiteral("sig")).toString() != sig) {
        // 硬件变了（换显卡/换驱动/换设备）：旧耗时没有可比性，直接丢弃
        return;
    }
    const auto parsed = FromJson(root.value(QStringLiteral("results")).toObject());
    QMutexLocker lock(&s.mutex);
    s.results = parsed;
    ++s.prog.version;
}

void ClearResults()
{
    Store &s = S();
    {
        QMutexLocker lock(&s.mutex);
        s.results.clear();
        s.prog.pending.clear();
        ++s.prog.version;
    }
    // 清空也要落盘，否则重启后旧数据又回来
    UiPrefs::Set(QString::fromLatin1(kPrefKey), QString());
}

namespace {

void SaveLocked(Store &s)
{
    QJsonObject root;
    root.insert(QStringLiteral("sig"), s.prog.signature);
    root.insert(QStringLiteral("results"), ToJson(s.results));
    const QByteArray blob = QJsonDocument(root).toJson(QJsonDocument::Compact).toBase64();
    UiPrefs::Set(QString::fromLatin1(kPrefKey), QString::fromUtf8(blob));
}

void SetProgress(const QString &model, const QString &hw)
{
    Store &s = S();
    QMutexLocker lock(&s.mutex);
    s.prog.activeModel = model;
    s.prog.activeHw = hw;
    ++s.prog.version;
}

void StoreSamples(const QString &model, const QVector<SrEngine::BenchSample> &rows)
{
    Store &s = S();
    QMutexLocker lock(&s.mutex);
    QHash<QString, Sample> &dst = s.results[model];
    for (const SrEngine::BenchSample &r : rows) {
        Sample sample;
        sample.ok = r.ok;
        sample.ms = r.ms;
        sample.note = r.note;
        dst.insert(r.hwId, sample);
    }
    ++s.prog.version;
}

}  // namespace

bool Start(const QStringList &models)
{
    if (models.isEmpty())
        return false;
    QVector<QString> hwIds;
    for (const SrEngine::BenchHw &h : Hardware()) {
        if (h.available)
            hwIds << h.id;
    }
    if (hwIds.isEmpty())
        return false;

    Store &s = S();
    {
        QMutexLocker lock(&s.mutex);
        if (s.prog.running)
            return false;
        s.prog.running = true;
        s.prog.cancelling = false;
        s.prog.done = 0;
        s.prog.total = models.size();
        s.prog.activeModel.clear();
        s.prog.activeHw.clear();
        s.prog.pending.clear();
        s.cancel.store(false);
        ++s.prog.version;
    }

    const QString original = SrEngine::CurrentModel();
    const bool hadGpu = std::any_of(hwIds.cbegin(), hwIds.cend(),
                                    [](const QString &id) { return id.startsWith(QLatin1String("gpu")); });

    // 后台任务：只碰 Store 与 SrEngine，不引用任何界面对象
    QtConcurrent::run([models, hwIds, original, hadGpu] {
        QElapsedTimer timer;
        timer.start();
        for (int i = 0; i < models.size(); ++i) {
            Store &st = S();
            if (st.cancel.load()) {
                QMutexLocker lock(&st.mutex);
                st.prog.pending = models.mid(i);
                break;
            }
            const QString model = models.at(i);
            SetProgress(model, QString());
            const QVector<SrEngine::BenchSample> rows = SrEngine::BenchModel(
                model, hwIds, 192, 3,
                [model](const QString &hw) { SetProgress(model, hw); },
                &st.cancel);
            StoreSamples(models.at(i), rows);
            QMutexLocker lock(&st.mutex);
            st.prog.done = i + 1;
            st.prog.activeHw.clear();
            ++st.prog.version;
        }

        // 收尾：恢复原模型 + 默认 GPU；结果落盘
        SrEngine::BenchFinish(original, hadGpu);
        Store &st = S();
        {
            QMutexLocker lock(&st.mutex);
            st.prog.running = false;
            st.prog.cancelling = false;
            st.prog.activeModel.clear();
            st.prog.activeHw.clear();
            const bool cancelled = st.cancel.load();
            if (!cancelled)
                st.prog.pending.clear();
            st.prog.lastSummary =
                cancelled
                    ? QStringLiteral("已取消 · 完成 %1/%2").arg(st.prog.done).arg(st.prog.total)
                    : QStringLiteral("完成 %1 个模型 · %2s")
                          .arg(st.prog.done)
                          .arg(timer.elapsed() / 1000.0, 0, 'f', 1);
            ++st.prog.version;
            SaveLocked(st);
        }
    });
    return true;
}

void Cancel()
{
    Store &s = S();
    s.cancel.store(true);
    QMutexLocker lock(&s.mutex);
    if (s.prog.running)
        s.prog.cancelling = true;
    ++s.prog.version;
}

bool ResumePending()
{
    QStringList pending;
    {
        Store &s = S();
        QMutexLocker lock(&s.mutex);
        pending = s.prog.pending;
    }
    if (pending.isEmpty())
        return false;
    {
        Store &s = S();
        QMutexLocker lock(&s.mutex);
        s.prog.pending.clear();
    }
    return Start(pending);
}

void DismissPending()
{
    Store &s = S();
    QMutexLocker lock(&s.mutex);
    s.prog.pending.clear();
    ++s.prog.version;
}

}  // namespace SrBench
