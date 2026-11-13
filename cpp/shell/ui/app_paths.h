// 配置/数据/缓存的统一路径解析。
//
// 为什么需要这一层：
//   * Android 的 QStandardPaths::AppConfigLocation 在部分 Qt/ROM 组合下返回空串，
//     或指向一个不可写目录。旧代码此时退回 QCoreApplication::applicationDirPath()，
//     而 Android 上那是 /data/app/~~<hash>/<pkg>-<ver>/lib/<abi>：
//       ① 只读 —— QSettings 写不进去，且 **不报错**；
//       ② 每次安装（含覆盖升级）路径都变。
//     表现就是"重启/升级后登录态和个性化设置全部丢失"。
//   * session.ini / ui.ini（字体、分流、缓存目录）必须落在同一个稳定目录，
//     否则一次版本升级等于换了一个配置根。
//
// 规则：
//   1. JM_UI_CONFIG 显式覆盖（回归/多实例隔离，优先级最高）；
//   2. AppConfigLocation；
//   3. AppLocalDataLocation/settings、AppDataLocation/settings；
//   4. Android：HOME/settings、HOME（HOME 由 Qt 设为 /data/user/0/<pkg>/files）；
//   5. 桌面：applicationDirPath()/config —— 仅非 Android 才允许；
//      Android 上宁可用临时目录也不用 appdir（appdir 每次安装都会变）。
#pragma once

#include <QString>

namespace AppPaths {

// 配置文件目录（已 mkpath）。不可写时返回空串，调用方需按"配置不可用"处理。
QString ConfigDir();
// 配置文件绝对路径（目录不可用时返回空串）。
QString ConfigFile(const QString &name);

// 数据目录（AppLocalDataLocation；Android = /data/user/0/<pkg>/files）
QString DataDir();
// 默认缓存根目录（不含 ui.ini 里的自定义覆盖）
QString CacheDir();

// 目录当前是否可写（用探针文件实测，避免只读挂载的假阳性）。
bool IsWritable(const QString &dir);

// 兼容旧安装：若新目录里没有 name，而旧位置（appdir / 旧 Qt 配置目录）有，
// 复制过来。Android 上 appdir 只读，命中概率低，但桌面端会用到。
bool MigrateLegacyFile(const QString &name);

// 一行诊断（--path-report）：各标准路径 + 实际生效目录 + 可写性。
QString Report();

}  // namespace AppPaths
