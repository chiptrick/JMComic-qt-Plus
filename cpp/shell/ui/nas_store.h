// 网络存储（NAS）节点：本地目录 / WebDAV / SMB 共享的注册表 + 可用性校验。
//
// 与 Python 的对应关系：
//   * python/src/view/nas/nas_db.py        —— 节点表（sqlite）；这里落地到 ui.ini 的
//                                              [nas] nodes（JSON 数组），不引数据库；
//   * python/src/view/nas/nas_add_view.py  —— 添加节点表单字段（名称/类型/地址/账号）；
//   * python/src/task/upload_webdav.py     —— WebDAV 上传/探测；这里只做**探测**。
//
// 能力边界（必须如实标注，不能假装支持）：
//   * local —— 目录是否存在且可读；"打开"直接进本地页浏览图片；
//   * webdav —— 用 PROPFIND (Depth:0) 校验可达性与认证；浏览/下载需要完整的
//               WebDAV 客户端（PROPFIND Depth:1 + XML + PUT），本期未实现；
//   * smb —— 只校验"路径形态 + 系统是否已挂载"（Windows 的 \\host\share）。
//             SMB 协议栈（握手/NTLM/签名）不在本项目范围内，也不会为它写一个
//             半成品——用户需先在系统里映射网络驱动器。
//
// 密码保存策略：与 session.ini 一致，明文存 ui.ini（已在该文件头注明风险）。
// 不存密码则每次校验/浏览都要重输；这里选择"可保存"，但界面上明确标注。
#pragma once

#include <QString>
#include <QVector>

#include <functional>

struct NasNode
{
    QString id;       // "nas-<ms>-<seq>"，仅本地唯一
    QString name;     // 显示名
    QString type;     // local / webdav / smb
    QString target;   // 本地路径 / https://host/dav/ / \\host\share
    QString user;
    QString pass;
    // ---- 运行时（不落盘）----
    bool ok = false;
    QString status;   // "未校验" / "可用" / "失败：..."
    qint64 checkedMs = 0;
};

namespace NasStore {

QStringList Types();                       // local / webdav / smb
QString TypeLabel(const QString &type);    // 本地目录 / WebDAV / SMB 共享
QString TypeGlyphHint(const QString &type);// 行首角标字符（表格用）

QVector<NasNode> Load();
bool Save(const QVector<NasNode> &nodes, QString *error);

// 静态校验：字段完整性（不做网络/文件访问）
bool Validate(NasNode *node, QString *error);

// 可用性校验（异步，回调一定在主线程）：
//   local/smb -> QFileInfo 是否可见；
//   webdav    -> PROPFIND Depth:0，207/200 视为可用，401/403 报认证失败。
void Probe(const NasNode &node, const std::function<void(const NasNode &)> &done);

// 回归/自检：构造一个 WebDAV 探测请求（不发送），返回 method / url / 关键头
QString DescribeProbe(const NasNode &node);

// 回归：节点表的增/校验/持久化/删全链路（用临时目录，结束后还原原配置）。
// 返回多行 ASCII 报告，每行以 "nas-store:" 开头。
QString SelfTest();

}  // namespace NasStore
