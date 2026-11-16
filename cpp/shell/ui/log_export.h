// 日志导出：把当天/历史诊断日志 + 一份环境摘要打成一个 zip，供用户直接发给维护者。
//
// 为什么自己写 ZIP（store 模式）：
//   * Qt 只提供 qCompress（zlib 流），不是 zip 容器；
//   * 本项目不引第三方归档库（体积/许可证都不划算）；
//   * "不压缩 + 只存" 的 zip 规范极短（本地头 + 中央目录 + EOCD），几百行能写对，
//     且任何解压工具都能开（Windows 资源管理器、7-Zip、Python zipfile）。
//
// 明确排除的敏感内容：
//   * session.ini（明文 AVS，等价登录态）；
//   * ui.ini（含自定义节点与分流历史，非必要；排障需要时由用户自行提供）。
// 只收 logs/*.log 与 summary.txt（摘要里账号只写用户名，不含任何 token/cookie）。
#pragma once

#include <QString>
#include <QStringList>

namespace LogExport {

// 日志目录（桌面为 AppLocalData/logs；Android 为空——那边日志进 logcat）
QString LogDir();

// 默认文件名：JMComic-logs-<yyyyMMdd-HHmmss>.zip
QString DefaultFileName();

// 导出到 destDir（目录不存在会自动创建）。extraLines 追加到 summary.txt。
// 成功返回 zip 绝对路径；失败返回空串并把原因写进 *error。
QString Export(const QString &destDir, const QStringList &extraLines, QString *error);

// 回归：不写文件，只返回将要写入摘要的正文（便于断言内容）
QString BuildSummary(const QStringList &extraLines);

}  // namespace LogExport
