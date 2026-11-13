// DEMO 主窗口：把"首页分区"和"左侧导航模块"两套切换都接到同一个 SlidePager 上，
// 用于确认手势与转场观感。所有内容均为示例数据，与产品功能无关。
#pragma once

#include <QWidget>

class SlidePager;
class DemoStrip;
class NavRail;
class QLabel;
class QPushButton;
class QTimer;

class DemoWindow : public QWidget
{
    Q_OBJECT
public:
    explicit DemoWindow(QWidget *parent = nullptr);

    SlidePager *ModulePager() const { return modules_; }
    SlidePager *SectionPager() const { return sections_; }
    DemoStrip *StripWidget() const { return strip_; }
    NavRail *RailWidget() const { return rail_; }

    void SetCompareFade(bool on);
    bool CompareFade() const { return compare_; }
    bool RulesVisible() const;
    void ShowRules(bool on);
    void StartAutoSequence();
    void EdgeReboundDemo();

protected:
    void resizeEvent(QResizeEvent *event) override;

private:
    QWidget *BuildBody();
    QWidget *BuildHome();
    QWidget *BuildSearch();
    QWidget *BuildCategory();
    QWidget *BuildFavorites();
    QWidget *BuildSettings();
    QWidget *BuildDebugBar();
    void BuildRulesOverlay();
    void UpdateDebug();
    void Toast(const QString &text);

    SlidePager *modules_ = nullptr;
    SlidePager *sections_ = nullptr;
    DemoStrip *strip_ = nullptr;
    NavRail *rail_ = nullptr;
    QLabel *live_ = nullptr;
    QLabel *params_ = nullptr;
    QLabel *toast_ = nullptr;
    QWidget *rules_ = nullptr;
    QPushButton *cmpBtn_ = nullptr;
    QPushButton *ruleBtn_ = nullptr;
    QTimer *debugTimer_ = nullptr;
    QTimer *toastTimer_ = nullptr;
    bool compare_ = false;
};
