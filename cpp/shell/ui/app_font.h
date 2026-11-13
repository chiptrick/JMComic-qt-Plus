// 应用字体：跟随系统 / 自定义（含可变字体轴），并监听运行期的系统字体变更。
//
// 为什么需要单独一层：
//   * 主题 qss 里原本写死了 `font-family: Roboto`（QtMaterial 的遗产）。Windows 没装
//     Roboto，Qt 不报错，只会静默换成 Tahoma —— 于是"设置应用字体"这件事根本不生效。
//     现在 qss 里的 font-family 被剥掉，字体统一由这里产出（见 theme.cpp）。
//   * 系统字体是**会变的**：用户在 Windows 设置里改了"文本大小/默认字体"，Qt 不会
//     自动重发应用字体事件。必须自己监听 WM_SETTINGCHANGE(SPI_SETNONCLIENTMETRICS)
//     并重新取值。
//
// 平台差异：
//   Windows  → SystemParametersInfoW(SPI_GETNONCLIENTMETRICS).lfMessageFont，
//              与资源管理器/系统对话框同源；变更由 WM_SETTINGCHANGE / WM_FONTCHANGE 通知。
//   Android  → QFontDatabase::systemFont(GeneralFont)（Roboto / Noto），
//              运行期不改，靠 fontDatabaseChanged + 轮询兜底。
//   macOS    → .AppleSystemUIFont；监听 QEvent::ApplicationFontChange 无效，
//              同样靠轮询兜底（本工程未在 macOS 验证，逻辑与 Linux 同路）。
//
// 回退策略（三级）：
//   1. 自定义字体不存在 → 退回系统字体，并在设置页显示原因；
//   2. 自定义字体缺中文字形 → QFont::setFamilies({自定义, 系统}) 让 Qt 逐字回退，
//      不会出现豆腐块；
//   3. 系统字体也取不到（极端情况）→ 用 QFontDatabase 的默认族 + 9pt。
#pragma once

#include <QFont>
#include <QObject>
#include <QString>
#include <QStringList>

class QTimer;

class AppFont : public QObject
{
    Q_OBJECT
public:
    enum class Source { System, Custom };

    struct Config {
        Source source = Source::System;
        QString family;        // 仅 Custom 有效
        int weight = 400;      // 100..900（Thin..Black）
        int widthPct = 100;    // 50..200（映射到 wdth 轴 / QFont::setStretch）
        int pointSize = 9;     // 8..12（超出后定高容器会裁字，见 app_font.cpp 的 kMaxPoint）
    };

    static AppFont &I();

    // 启动时调用一次：读配置 → 解析 → 应用 → 装监听
    void Init();

    const Config &Cfg() const { return cfg_; }

    // 当前生效的字体（含回退家族列表与可变轴）
    QFont Build() const;
    // QFontInfo 实际解析到的家族（可能与配置不同，用于诊断/设置页展示）
    QString ResolvedFamily() const;
    QString SystemFamily() const;
    double EffectivePointSize() const;
    bool UsingCustom() const { return cfg_.source == Source::Custom; }
    // 配置的家族是否真的在字体库里（否则已回退）
    bool CustomFamilyAvailable() const;
    QString StatusText() const;

    void SetSource(Source s);
    void SetFamily(const QString &family);
    void SetWeight(int w);
    void SetWidth(int pct);
    void SetPointSize(int pt);
    void ResetToSystem();

    // 系统字体清单（缓存，fontDatabaseChanged 时失效）
    static QStringList Families();

    // 系统字体可能变了：重读并应用（由监听器调用，也可手动触发）
    void RefreshFromSystem();

    // 测试/演示用：替换"系统字体"的来源，并立刻触发一次与真实变更相同的检查流程。
    // 只伪造"操作系统给出的答案"，其余（防抖、比对、重设、刷新）都走真实路径。
    void SetSystemFontProviderForTest(const QFont &font);
    void ClearSystemFontProviderForTest();

signals:
    // 字体已变化（UiTheme 收到后决定是否需要重建 qss：只有字号变化才需要）
    void Changed();

private:
    AppFont() = default;
    void Load();
    void Persist() const;
    void ReadSystemFont();
    void ScheduleApply();
    void ApplyNow();

    Config cfg_;
    QFont sysFont_;        // 最近一次读到的系统字体
    QFont applied_;        // 最近一次已应用的字体（去重用）
    // 去重签名：QFont::key() 在部分 Qt 版本里不含可变轴，只比 key 会让
    // "只改字重/字宽"的请求被当成重复而丢弃（手机上表现为设置不生效）
    QString appliedKey_;
    bool sysFromTest_ = false;
    bool hasApplied_ = false;
    bool applying_ = false;
    QTimer *applyTimer_ = nullptr;   // 40ms 合并：拖动滑块时不会每步都重刷整个界面
};
