// 自定义缓存路径：封面磁盘缓存与章节图片缓存共用的根目录。
//
// 解析优先级（与其它配置项同一套思路）：
//   1. 环境变量 JM_CACHE_DIR（测试/回归用，优先级最高）；
//   2. ui.ini 的 [cache] root（设置页"缓存目录"写入）；
//   3. 默认 AppLocalDataLocation/cache（Android 上即 /data/user/0/<pkg>/files/cache）。
//
// 为什么不复用 config.ini：那份文件是 Python 版共用的 schema（section/key 必须
// 一一对应），加 C++ 专有键会污染互通性；字体设置（AppFont）也落在 ui.ini，
// 这里保持一致。
#pragma once

#include <QString>

namespace CachePath {

// 默认根目录（不读配置）
QString DefaultRoot();
// 当前生效的根目录（已 mkpath）
QString Root();
// 是否配置了自定义目录（false = 用默认目录）
bool IsCustom();
// 设置自定义目录；传空串 = 恢复默认。成功后立即建目录并返回是否可用。
bool SetRoot(const QString &path);

QString CoverDir();
QString PageDir();

// 界面配置文件（ui.ini，AppConfigLocation）：字体、阅读模式、缓存目录都写在这里。
// 与 Python 版共用的 config.ini 分开，避免污染互通的 section/key。
QString UiIniPath();
// 兼容旧名字
QString ConfigFile();

// 回归/自检：把当前生效路径与应用到缓存层的结果打成一行日志
QString StatusLine();

}  // namespace CachePath
