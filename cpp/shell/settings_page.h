// C1-W5：设置页 —— 读写 Python 版同款 config.ini（QSettings INI，section/key 完全一致）。
//
// 设计要点：
//   * 路径：环境变量 `JM_C1_CONFIG_PATH` 优先（把 C1 指到 Python 版的 config.ini，
//     用于桌面/同机互通测试）；否则 `AppLocalDataLocation/config.ini`（与 Python 版
//     在 Android 上的私有目录语义一致，但包名不同所以实际文件不同）。
//   * 格式：QSettings IniFormat，section/key 用 Python 侧的同名 tag + 变量名
//     （GeneraSetting/ThemeIndex、ProxySetting/IsUseHttps 等），保证 Python 版能读。
#pragma once

#include <QLineEdit>
#include <QCheckBox>
#include <QComboBox>
#include <QWidget>

class QLabel;

class SettingsPage : public QWidget
{
    Q_OBJECT
public:
    explicit SettingsPage(QWidget *parent = nullptr);

    // 当前生效的 config.ini 绝对路径
    QString ConfigPath() const { return path_; }
    // 重新从磁盘加载
    void Load();
    // 把 UI 值写回 config.ini
    void Save();
    // 冒烟自检：写探针 key -> 读回 -> 日志；不碰真实设置值
    void RunSelfTest();

signals:
    void StatusMessage(const QString &text);

private:
    QString ResolveConfigPath() const;
    void UpdatePathLabel();

    QString path_;
    QComboBox *theme_ = nullptr;
    QCheckBox *https_ = nullptr;
    QLineEdit *savePath_ = nullptr;
    QComboBox *readMode_ = nullptr;
    QCheckBox *waifu_ = nullptr;
    QLineEdit *userId_ = nullptr;
    QLabel *pathLabel_ = nullptr;
};
