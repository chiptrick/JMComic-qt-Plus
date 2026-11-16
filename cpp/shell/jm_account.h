// 账号相关的"网页端点"：注册 / 找回密码 / 重发验证邮件 / 图形验证码。
//
// 为什么不在 JmApi 里做：
//   * 这几个接口是**站点网页表单**（POST 到 https://<web>/signup 等），不是加密的
//     APP API：没有 token 头、没有 AES 包裹，返回的是 HTML；
//   * 网站域名与 API 域名是两套（Python GlobalConfig.Url vs GetApiUrl），
//     而且网站域名会从更新配置的 UrlList 动态更新；
//   * 需要浏览器式的 Cookie 复用（先 GET 表单页拿 cookie，再 POST 提交）。
// 因此单独一层，走 NodeHttp（可复用节点/DoH/ECH 通道），不碰 JmApi 的单通道队列。
//
// 与 Python 的对应关系（src/server/req.py）：
//   RegisterReq           POST /signup      username/password/password_confirm/email/
//                                           verification/gender/age/terms/submit_signup
//   RegisterVerifyMailReq POST /confirm     username/password/submit_confirm
//   ResetPasswordReq      POST /lost        email/submit_lost
//   GetCaptchaReq         GET  /captcha     （同一 Cookie 会话下取图）
//
// 结果判定照搬 ToolUtil.ParseMsg：从 HTML 里取 toastr['error'](...) / toastr['success'](...)。
// 站点被 Cloudflare 拦截或域名不可达时，这里只如实回"网络/HTTP 错误"，由界面提示
// 用户改用网页注册（不伪造成功）。
#pragma once

#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>

class NodeHttp;

class JmAccount : public QObject
{
    Q_OBJECT
public:
    explicit JmAccount(QObject *parent = nullptr);

    // Web 域名表：JM_WEB_BASE（逗号分隔）> ui.ini [update] webUrls > 内置默认
    static QStringList DefaultWebBases();
    QStringList Bases() const { return bases_; }
    void SetBases(const QStringList &bases);
    QString CurrentBase() const { return bases_.value(baseIndex_); }

    // 纯函数：从 HTML 里抽 toastr 文案。返回 (是否有 success 提示, 文案)。
    // 多条 error 用换行拼接（与 Python 的 "\n".join 一致）。
    static QPair<bool, QString> ParseToastr(const QByteArray &html);

    // 表单编码：键值按插入顺序，全部 percent-encode（等价 Python DictToUrl + 表单 POST）
    static QByteArray EncodeForm(const QVector<QPair<QString, QString>> &fields);

    void Register(const QString &user, const QString &email, const QString &password,
                  const QString &password2, const QString &gender, const QString &captcha);
    void ResetPassword(const QString &email);
    void ResendVerifyMail(const QString &user, const QString &password);
    void FetchCaptcha();

signals:
    void RegisterFinished(bool ok, const QString &message);
    void ResetPasswordFinished(bool ok, const QString &message);
    void VerifyMailFinished(bool ok, const QString &message);
    void CaptchaReady(bool ok, const QByteArray &image, const QString &contentType,
                      const QString &error);
    void Log(const QString &line);

private:
    // 先 GET 表单页拿 cookie（best-effort），再 POST。cookie 为空也照常提交：
    // 有些镜像不要求会话，过度依赖反而会把可用路径挡掉。
    void SubmitForm(const QString &path, const QVector<QPair<QString, QString>> &fields,
                    const QString &referer,
                    const std::function<void(bool, int, const QByteArray &, const QString &)> &cb);
    void Send(const QByteArray &method, const QString &path, const QByteArray &body,
              const QString &cookie, const QString &referer, int timeoutMs, qint64 maxBytes,
              const std::function<void(bool, int, const QByteArray &, const QHash<QString, QString> &,
                                       const QString &)> &cb);
    static QString MergeCookies(const QString &oldCookie, const QHash<QString, QString> &headers);
    void RotateBase(const QString &why);

    NodeHttp *http_ = nullptr;
    QStringList bases_;
    int baseIndex_ = 0;
    QString cookie_;   // 站点会话（进程内；不落盘）
};
