// 升级迁移与配置保护层（docs/data_persistence_upgrade_analysis.md §3.2/§3.5）。
//
// 为什么需要这一层：
//   * 旧版本没有 schemaVersion，也没有迁移步骤——键改名后 QSettings 直接回退
//     默认值（静默丢配置）；没有升级前备份，出问题无法回滚。
//   * 配置文件损坏（掉电/杀毒/半截写入）时，QSettings 会把坏文件当空文件读，
//     下一次 Save 再用默认值整体覆盖——登录态当场蒸发。
//
// 规则：
//   1. Run() 在 main() 里、任何读写 ui.ini / session.ini 的代码之前调用。
//   2. 损坏隔离：解析失败的文件改名 .corrupt-<ts> 保留现场；同名 .bak 可用则
//      拷回来，用户无感恢复。
//   3. 升级备份：[meta] schemaVersion < kCurrentSchema 时，先把两份 ini（含
//      .bak）拷到 <ConfigDir>/backup/v<from>/，任何失败都不删原文件。
//   4. 逐步执行 v<n>->v<n+1> 迁移；失败不提升版本号，下次启动重试。
//   5. 幂等：schemaVersion 已是当前版本时直接返回，重复运行不改文件。
//   6. 只 setValue 已知键，绝不 clear()——未知键永远保留。
#pragma once

#include <QString>

namespace DataMigrator {

// 当前 schema 版本。新增迁移步骤时 +1，并在 Run() 的 switch 里补 v<n>->v<n+1>。
constexpr int kCurrentSchema = 2;

// 启动时的一次性检查/迁移/备份。见文件头注释。
void Run();

// 一次成功写入后的滚动备份（主文件 -> .bak，写成功才覆盖备份）。
// session.ini 的 Save() 自带等价逻辑；ui.ini 的主要写盘点（UiPrefs/ProxyConfig/
// CachePath/AppFont/SearchHistory）调用它兜住"写坏后无备份可回滚"的窗口。
void RollingBackup(const QString &iniPath);

// 损坏隔离 + .bak 恢复（Run() 内部步骤，也可独立调用）。
// 返回隔离/恢复动作的日志串（多行），无动作时为空。
QString QuarantineIfBroken(const QString &iniPath);

// --export-data <dir>：把 session.ini / ui.ini（连同 .bak）拷到目录。
// 目录用文件而不是 zip：透明、可审阅、不引入手工 zip 解析的脆弱性。
// 返回错误串，空串=成功。
QString ExportData(const QString &dirPath);

// --import-data <dir>：把导出目录里的 ini 拷回配置目录；
// 目标已存在时先滚动备份（.bak 语义），下次启动生效的键立即生效。
// 返回错误串，空串=成功。
QString ImportData(const QString &dirPath);

// 当前 [meta] schemaVersion（ui.ini 不存在或无该键时返回 1）。
int SchemaVersion();

}  // namespace DataMigrator
