// 会话落库：把"已登录"状态写进用户配置目录，下次启动自动恢复。
//
// 存 UI 展示字段（用户名 / 称号 / J Coins / 等级 / 收藏数 / 屏蔽词）与真实登录
// 拿到的 AVS cookie；**不存密码**。AVS 是服务端下发的会话凭证，等价于登录态，
// 所以这个文件本身要按敏感文件对待（当前明文 INI，见 docs 里的风险说明）。
//
// 位置与 ui.ini 同目录（QStandardPaths::AppConfigLocation），Windows 上是
// %APPDATA%/JMComic/JMComic/session.ini，测试用 APPDATA 隔离。
#pragma once

#include <QString>

namespace SessionStore {

// 读取并应用到 UserSession::Ref()；文件不存在时保持未登录。
void Load();

// 把当前 UserSession::Ref() 写回磁盘（登录成功后调用）。
void Save();

// 清除会话（AVS / uid / 账号展示字段）并把内存状态重置为未登录。
// 本地偏好（屏蔽词、分流展示名）不受影响：Clear 只清账号相关字段，
// 然后照常写回 session.ini（loggedIn=false）。
void Clear();

// 归因/自检用：当前实际使用的 session.ini 路径（空串 = 配置目录不可写）。
QString Path();

}  // namespace SessionStore
