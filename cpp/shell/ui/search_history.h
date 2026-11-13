// 搜索历史（最近搜索）。
//
// 之前搜索页的四个词是写死的演示文案：用户搜过的词不会记录，重启后永远是
// 那四个；「清空」按钮也没接线。这里补一份真正的持久化：
//
//   * 存储：ui.ini 的 [search] recent（QStringList），与字体/分流/偏好同一目录，
//     走 AppPaths（Android 上不会退回只读的 applicationDirPath）；
//   * 语义：最近搜索在前（MRU），忽略大小写去重，最多 kMaxItems 条；
//   * 读写都在 SearchHistory 里，搜索页只负责展示与调用。
#pragma once

#include <QString>
#include <QStringList>

namespace SearchHistory {

// 最多保留多少条（超过后丢最旧的）
int MaxItems();

// 最近搜索列表（最新的在前）
QStringList Items();

// 记录一次搜索：trim 后为空则忽略；已存在（忽略大小写）则移到最前。
// 返回 true 表示列表发生了变化（调用方可据此刷新 UI）。
bool Add(const QString &keyword);

// 清空并落库
void Clear();

// 诊断（--data-report）：count=N first=xxx
QString Report();

}  // namespace SearchHistory
