#include "frame_probe.h"

#include <QElapsedTimer>
#include <QFile>
#include <QStandardPaths>
#include <QTimer>
#include <QVector>

#include <algorithm>

#if defined(__ANDROID__)
#include <android/log.h>
#define C1_FPS_LOG(...) __android_log_print(ANDROID_LOG_INFO, "C1Fps", __VA_ARGS__)
#else
#include <QDebug>
#define C1_FPS_LOG(...) qInfo(__VA_ARGS__)
#endif

namespace {

QString MarkerPath()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)
           + QStringLiteral("/fps_log");
}

double Percentile(QVector<double> values, double p)
{
    if (values.isEmpty())
        return 0.0;
    std::sort(values.begin(), values.end());
    const int idx = qBound(0, int(qRound((values.size() - 1) * p)), values.size() - 1);
    return values.at(idx);
}

// 汇总器：定时落盘，而不是"等下一次翻页顺带检查"。
// 后者在用户翻完就停的场景下会让最后一批样本永远打不出来（实测踩过）。
struct Aggregator
{
    QVector<double> samples;
    qint64 total = 0;
    QTimer *timer = nullptr;

    void Flush()
    {
        if (samples.isEmpty())
            return;
        const double p50 = Percentile(samples, 0.50);
        const double p95 = Percentile(samples, 0.95);
        const double worst = *std::max_element(samples.begin(), samples.end());
        C1_FPS_LOG("page-turns n=%d total=%lld p50=%.1fms p95=%.1fms max=%.1fms",
                   int(samples.size()), static_cast<long long>(total), p50, p95, worst);
        samples.clear();
    }
};

Aggregator &Agg()
{
    static Aggregator agg;
    return agg;
}

constexpr int kFlushMs = 3000;
constexpr int kFlushCount = 60;

}  // namespace

namespace FrameProbe {

bool Enabled()
{
    static const bool enabled = [] {
        if (qEnvironmentVariableIntValue("JM_C1_FPS") == 1)
            return true;
        return QFile::exists(MarkerPath());
    }();
    return enabled;
}

void LogPageTurn(double ms)
{
    if (!Enabled() || ms < 0.0)
        return;

    Aggregator &agg = Agg();
    if (!agg.timer) {
        agg.timer = new QTimer();
        agg.timer->setSingleShot(true);
        agg.timer->setInterval(kFlushMs);
        QObject::connect(agg.timer, &QTimer::timeout, [] { Agg().Flush(); });
    }

    agg.samples.append(ms);
    ++agg.total;

    if (agg.samples.size() >= kFlushCount) {
        agg.timer->stop();
        agg.Flush();
    } else if (!agg.timer->isActive()) {
        agg.timer->start();  // 首样本后 3 秒必落盘，不依赖"还有下一步操作"
    }
}

}  // namespace FrameProbe
