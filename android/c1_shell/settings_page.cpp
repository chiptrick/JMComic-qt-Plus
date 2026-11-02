#include "settings_page.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QLabel>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QVBoxLayout>

#include <android/log.h>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "C1Settings", __VA_ARGS__)

namespace {

// 与 src/config/setting.py 的同名变量保持一致（section=tag, key=变量名）
const char *kTheme = "GeneraSetting/ThemeIndex";
const char *kHttps = "ProxySetting/IsUseHttps";
const char *kSavePath = "DownloadSetting/SavePath";
const char *kReadMode = "ReadSetting/LookReadMode";
const char *kWaifu = "Waifu2xSetting/IsOpenWaifu";
const char *kUserId = "Other/UserId";

}  // namespace

SettingsPage::SettingsPage(QWidget *parent)
    : QWidget(parent)
{
    path_ = ResolveConfigPath();

    auto *root = new QVBoxLayout(this);
    auto *form = new QFormLayout();

    theme_ = new QComboBox(this);
    theme_->addItems({QStringLiteral("Auto"), QStringLiteral("light"), QStringLiteral("dark")});
    form->addRow(QStringLiteral("Theme"), theme_);

    https_ = new QCheckBox(QStringLiteral("Use HTTPS"), this);
    form->addRow(QStringLiteral("Proxy"), https_);

    savePath_ = new QLineEdit(this);
    form->addRow(QStringLiteral("Save path"), savePath_);

    readMode_ = new QComboBox(this);
    readMode_->addItems({QStringLiteral("Single"), QStringLiteral("Double")});
    form->addRow(QStringLiteral("Read mode"), readMode_);

    waifu_ = new QCheckBox(QStringLiteral("Enable waifu2x"), this);
    form->addRow(QStringLiteral("SR"), waifu_);

    userId_ = new QLineEdit(this);
    form->addRow(QStringLiteral("User id"), userId_);

    root->addLayout(form);

    auto *btns = new QHBoxLayout();
    auto *reload = new QPushButton(QStringLiteral("Reload"), this);
    auto *save = new QPushButton(QStringLiteral("Save"), this);
    btns->addWidget(reload);
    btns->addWidget(save);
    root->addLayout(btns);

    pathLabel_ = new QLabel(this);
    pathLabel_->setWordWrap(true);
    root->addWidget(pathLabel_);
    root->addStretch(1);

    connect(reload, &QPushButton::clicked, this, [this] { Load(); });
    connect(save, &QPushButton::clicked, this, [this] { Save(); });

    UpdatePathLabel();
    Load();
}

QString SettingsPage::ResolveConfigPath() const
{
    if (const QString env = qEnvironmentVariable("JM_C1_CONFIG_PATH"); !env.isEmpty())
        return env;
    const QString base = QStandardPaths::writableLocation(
        QStandardPaths::AppLocalDataLocation);
    return base + QStringLiteral("/config.ini");
}

void SettingsPage::UpdatePathLabel()
{
    pathLabel_->setText(QStringLiteral("config.ini: %1").arg(path_));
}

void SettingsPage::Load()
{
    QDir().mkpath(QFileInfo(path_).absolutePath());
    QSettings s(path_, QSettings::IniFormat);
    theme_->setCurrentIndex(s.value(kTheme, 0).toInt());
    https_->setChecked(s.value(kHttps, 1).toInt() != 0);
    savePath_->setText(s.value(kSavePath, QString()).toString());
    readMode_->setCurrentIndex(s.value(kReadMode, 1).toInt());
    waifu_->setChecked(s.value(kWaifu, 0).toInt() != 0);
    userId_->setText(s.value(kUserId, QString()).toString());
    emit StatusMessage(QStringLiteral("settings loaded from %1").arg(path_));
    LOGI("loaded %s", qPrintable(path_));
}

void SettingsPage::Save()
{
    QDir().mkpath(QFileInfo(path_).absolutePath());
    QSettings s(path_, QSettings::IniFormat);
    s.setValue(kTheme, theme_->currentIndex());
    s.setValue(kHttps, https_->isChecked() ? 1 : 0);
    s.setValue(kSavePath, savePath_->text());
    s.setValue(kReadMode, readMode_->currentIndex());
    s.setValue(kWaifu, waifu_->isChecked() ? 1 : 0);
    s.setValue(kUserId, userId_->text());
    s.sync();
    emit StatusMessage(QStringLiteral("settings saved to %1").arg(path_));
    LOGI("saved %s", qPrintable(path_));
}

void SettingsPage::RunSelfTest()
{
    // 只写探针 key，不动真实设置；用来证明 QSettings INI 文件本身可写可读
    QSettings s(path_, QSettings::IniFormat);
    s.setValue(QStringLiteral("C1SelfTest/Value"), QStringLiteral("ok"));
    s.sync();
    const QString back = s.value(QStringLiteral("C1SelfTest/Value")).toString();
    LOGI("selftest path=%s value=%s", qPrintable(path_), qPrintable(back));
    emit StatusMessage(QStringLiteral("settings selftest %1")
                           .arg(back == QStringLiteral("ok") ? QStringLiteral("OK")
                                                             : QStringLiteral("FAIL")));
}
