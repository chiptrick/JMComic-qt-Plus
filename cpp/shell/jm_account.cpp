#include "jm_account.h"

#include "app_paths.h"
#include "node_http.h"

#include <QRegularExpression>
#include <QSettings>
#include <QUrl>
#include <QUrlQuery>

#include <memory>

namespace {

// 与 Python GlobalConfig.UrlList 的前几项一致。这些域名会随时间轮换，实际以
// 更新配置的 UrlList 为准（FetchConfig 后会写进 ui.ini 供这里读取）。
const char *kDefaultWebBases[] = {
    "https://comic18j-jjeg.cc",
    "https://18comic.vip",
    "https://jmcomic.me",
    "https://18comic.tw",
    "https://jmcomic-zzz.org",
};

// Python GetWebHeader 的 UA（网页表单按浏览器口径校验）
const char *kWebUserAgent =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
    "Chrome/114.0.0.0 Safari/537.36 Edg/114.0.1823.43";

QString IniPath()
{
    return AppPaths::ConfigFile(QStringLiteral("ui.ini"));
}

}  // namespace

JmAccount::JmAccount(QObject *parent) : QObject(parent), http_(new NodeHttp(this))
{
    bases_ = DefaultWebBases();
    connect(http_, &NodeHttp::Log, this, [this](const QString &l) { emit Log(l); });
}

QStringList JmAccount::DefaultWebBases()
{
    const QString env = qEnvironmentVariable("JM_WEB_BASE").trimmed();
    if (!env.isEmpty()) {
        QStringList out;
        for (const QString &s : env.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
            const QString t = s.trimmed();
            if (!t.isEmpty())
                out << t;
        }
        if (!out.isEmpty())
            return out;
    }
    QStringList out;
    const QString ini = IniPath();
    if (!ini.isEmpty()) {
        QSettings s(ini, QSettings::IniFormat);
        const QString cached = s.value(QStringLiteral("update/webUrls")).toString();
        for (const QString &u : cached.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
            const QString t = u.trimmed();
            if (!t.isEmpty())
                out << t;
        }
    }
    for (const char *b : kDefaultWebBases) {
        const QString u = QString::fromLatin1(b);
        if (!out.contains(u))
            out << u;
    }
    return out;
}

void JmAccount::SetBases(const QStringList &bases)
{
    if (bases.isEmpty())
        return;
    bases_ = bases;
    baseIndex_ = 0;
}

QPair<bool, QString> JmAccount::ParseToastr(const QByteArray &html)
{
    // 与 Python ParseMsg 同口径：只认 toastr['error'] / toastr['success'] 的字符串参数。
    // 允许参数里有转义引号（\"）——站点偶尔会这么输出。
    static const QRegularExpression errRe(
        QStringLiteral("toastr\\['error'\\]\\(\\s*\"((?:[^\"\\\\]|\\\\.)*)\""));
    static const QRegularExpression okRe(
        QStringLiteral("toastr\\['success'\\]\\(\\s*\"((?:[^\"\\\\]|\\\\.)*)\""));

    const QString text = QString::fromUtf8(html);
    QStringList errors;
    auto it = errRe.globalMatch(text);
    while (it.hasNext()) {
        const QString msg = it.next().captured(1).trimmed();
        if (!msg.isEmpty())
            errors << msg;
    }
    auto ok = okRe.globalMatch(text);
    QString success;
    if (ok.hasNext())
        success = ok.next().captured(1).trimmed();

    const bool hasSuccess = !success.isEmpty();
    if (hasSuccess)
        return {true, success};
    if (!errors.isEmpty())
        return {false, errors.join(QLatin1Char('\n'))};
    return {false, QString()};
}

QByteArray JmAccount::EncodeForm(const QVector<QPair<QString, QString>> &fields)
{
    QByteArray out;
    for (const auto &f : fields) {
        if (!out.isEmpty())
            out += '&';
        out += QUrl::toPercentEncoding(f.first);
        out += '=';
        out += QUrl::toPercentEncoding(f.second);
    }
    return out;
}

QString JmAccount::MergeCookies(const QString &oldCookie, const QHash<QString, QString> &headers)
{
    // set-cookie 可能有多个（NodeHttp 用换行拼接），只取 name=value 部分。
    QStringList pairs;
    const QString all = headers.value(QStringLiteral("set-cookie"));
    for (const QString &line : all.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        const QString first = line.split(QLatin1Char(';')).first().trimmed();
        if (first.isEmpty())
            continue;
        const QString name = first.split(QLatin1Char('=')).first().trimmed();
        if (name.isEmpty())
            continue;
        // 同名覆盖：先移除旧值
        for (int i = pairs.size() - 1; i >= 0; --i) {
            if (pairs.at(i).startsWith(name + QLatin1Char('=')))
                pairs.removeAt(i);
        }
        pairs << first;
    }
    if (pairs.isEmpty())
        return oldCookie;
    return pairs.join(QStringLiteral("; "));
}

void JmAccount::RotateBase(const QString &why)
{
    if (bases_.size() <= 1)
        return;
    baseIndex_ = (baseIndex_ + 1) % bases_.size();
    cookie_.clear();   // 换了站点，旧站点会话无效
    emit Log(QStringLiteral("account base -> %1（%2）").arg(bases_.value(baseIndex_), why));
}

void JmAccount::Send(const QByteArray &method, const QString &path, const QByteArray &body,
                     const QString &cookie, const QString &referer, int timeoutMs, qint64 maxBytes,
                     const std::function<void(bool, int, const QByteArray &,
                                              const QHash<QString, QString> &, const QString &)> &cb)
{
    const QString base = bases_.value(baseIndex_);
    QUrl url(base + path);
    if (!url.isValid()) {
        cb(false, 0, QByteArray(), {}, QStringLiteral("站点地址非法：%1").arg(base + path));
        return;
    }
    NodeHttpRequest req;
    req.method = method;
    req.url = url;
    req.timeoutMs = timeoutMs;
    req.maxBytes = maxBytes;
    // 表单流程固定走 TLS/HTTP1.1：要拿 set-cookie 与确定的状态码，H3 只会增加变量。
    req.allowQuic = false;
    req.headers.insert(QStringLiteral("User-Agent"), QString::fromLatin1(kWebUserAgent));
    req.headers.insert(QStringLiteral("Accept"),
                       QStringLiteral("text/html,application/xhtml+xml,*/*;q=0.8"));
    req.headers.insert(QStringLiteral("Accept-Language"), QStringLiteral("zh-CN,zh;q=0.9"));
    req.headers.insert(QStringLiteral("Origin"), base);
    if (!referer.isEmpty())
        req.headers.insert(QStringLiteral("Referer"), referer);
    if (!cookie.isEmpty())
        req.headers.insert(QStringLiteral("Cookie"), cookie);
    if (method == QByteArrayLiteral("POST"))
        req.headers.insert(QStringLiteral("Content-Type"),
                           QStringLiteral("application/x-www-form-urlencoded"));
    req.body = body;

    http_->Send(req, [cb](const NodeHttpResponse &r) {
        cb(r.ok, r.status, r.body, r.headers,
           r.ok ? (r.httpError ? QStringLiteral("HTTP %1").arg(r.status) : QString())
                : (r.error.isEmpty() ? QStringLiteral("网络失败") : r.error));
    });
}

void JmAccount::SubmitForm(const QString &path,
                           const QVector<QPair<QString, QString>> &fields,
                           const QString &referer,
                           const std::function<void(bool, int, const QByteArray &, const QString &)> &cb)
{
    auto self = this;
    auto body = std::make_shared<QByteArray>(EncodeForm(fields));
    auto ref = std::make_shared<QString>(referer);

    // 第一步：GET 表单页，顺带收下站点会话 Cookie（失败不致命）。
    Send(QByteArrayLiteral("GET"), path, QByteArray(), cookie_, *ref, 12000, 512 * 1024,
         [self, path, body, ref, cb](bool ok, int, const QByteArray &,
                                     const QHash<QString, QString> &headers,
                                     const QString &error) {
             if (ok)
                 self->cookie_ = MergeCookies(self->cookie_, headers);
             else
                 self->Log(QStringLiteral("account pre-GET %1 失败：%2").arg(path, error));

             auto attempt = std::make_shared<std::function<void(int)>>();
             *attempt = [self, path, body, ref, cb, attempt](int tried) {
                 self->Send(QByteArrayLiteral("POST"), path, *body, self->cookie_, *ref, 15000,
                            512 * 1024,
                            [self, tried, cb, attempt](bool ok2, int status2,
                                                       const QByteArray &html,
                                                       const QHash<QString, QString> &h2,
                                                       const QString &error2) {
                                if (ok2) {
                                    self->cookie_ = MergeCookies(self->cookie_, h2);
                                    cb(true, status2, html, QString());
                                    return;
                                }
                                // 只在"连不上/没收全"时换域名；4xx/5xx 说明已经连上了。
                                if (tried + 1 < self->bases_.size()) {
                                    self->RotateBase(error2);
                                    (*attempt)(tried + 1);
                                    return;
                                }
                                cb(false, status2, html, error2);
                            });
             };
             (*attempt)(0);
         });
}

void JmAccount::Register(const QString &user, const QString &email, const QString &password,
                         const QString &password2, const QString &gender, const QString &captcha)
{
    // 与 Python RegisterReq 的字段顺序/取值一致（age/terms 固定 on，submit_signup 为空值）
    const QVector<QPair<QString, QString>> fields = {
        {QStringLiteral("username"), user},
        {QStringLiteral("password"), password},
        {QStringLiteral("email"), email},
        {QStringLiteral("verification"), captcha},
        {QStringLiteral("password_confirm"), password2},
        {QStringLiteral("gender"), gender.isEmpty() ? QStringLiteral("Male") : gender},
        {QStringLiteral("age"), QStringLiteral("on")},
        {QStringLiteral("terms"), QStringLiteral("on")},
        {QStringLiteral("submit_signup"), QString()},
    };
    emit Log(QStringLiteral("account register user=%1 base=%2").arg(user, CurrentBase()));
    SubmitForm(QStringLiteral("/signup"), fields, CurrentBase() + QStringLiteral("/signup"),
               [this](bool ok, int status, const QByteArray &html, const QString &error) {
                   if (!ok) {
                       emit RegisterFinished(false, error);
                       return;
                   }
                   const auto parsed = ParseToastr(html);
                   if (!parsed.second.isEmpty()) {
                       emit RegisterFinished(parsed.first, parsed.second);
                       return;
                   }
                   // 有的镜像注册成功是 302 到首页，正文没有 toastr：2xx 视为"已提交，查收邮件"
                   if (status >= 200 && status < 400) {
                       emit RegisterFinished(
                           true, QStringLiteral("注册请求已提交，请到邮箱完成验证（HTTP %1）")
                                     .arg(status));
                       return;
                   }
                   emit RegisterFinished(false, QStringLiteral("HTTP %1").arg(status));
               });
}

void JmAccount::ResetPassword(const QString &email)
{
    // Python ResetPasswordReq：email + submit_lost="恢復密碼"
    const QVector<QPair<QString, QString>> fields = {
        {QStringLiteral("email"), email},
        {QStringLiteral("submit_lost"), QStringLiteral("恢復密碼")},
    };
    emit Log(QStringLiteral("account reset email=%1 base=%2").arg(email, CurrentBase()));
    SubmitForm(QStringLiteral("/lost"), fields, CurrentBase() + QStringLiteral("/lost"),
               [this](bool ok, int status, const QByteArray &html, const QString &error) {
                   if (!ok) {
                       emit ResetPasswordFinished(false, error);
                       return;
                   }
                   const auto parsed = ParseToastr(html);
                   if (!parsed.second.isEmpty()) {
                       emit ResetPasswordFinished(parsed.first, parsed.second);
                       return;
                   }
                   if (status >= 200 && status < 400) {
                       emit ResetPasswordFinished(
                           true, QStringLiteral("找回密码请求已提交，请查收邮件（HTTP %1）").arg(status));
                       return;
                   }
                   emit ResetPasswordFinished(false, QStringLiteral("HTTP %1").arg(status));
               });
}

void JmAccount::ResendVerifyMail(const QString &user, const QString &password)
{
    // Python RegisterVerifyMailReq：username + password + submit_confirm="發送EMAIL"
    const QVector<QPair<QString, QString>> fields = {
        {QStringLiteral("username"), user},
        {QStringLiteral("password"), password},
        {QStringLiteral("submit_confirm"), QStringLiteral("發送EMAIL")},
    };
    emit Log(QStringLiteral("account resend-verify user=%1 base=%2").arg(user, CurrentBase()));
    SubmitForm(QStringLiteral("/confirm"), fields, CurrentBase() + QStringLiteral("/confirm"),
               [this](bool ok, int status, const QByteArray &html, const QString &error) {
                   if (!ok) {
                       emit VerifyMailFinished(false, error);
                       return;
                   }
                   const auto parsed = ParseToastr(html);
                   if (!parsed.second.isEmpty()) {
                       emit VerifyMailFinished(parsed.first, parsed.second);
                       return;
                   }
                   if (status >= 200 && status < 400) {
                       emit VerifyMailFinished(
                           true, QStringLiteral("验证邮件请求已提交（HTTP %1）").arg(status));
                       return;
                   }
                   emit VerifyMailFinished(false, QStringLiteral("HTTP %1").arg(status));
               });
}

void JmAccount::FetchCaptcha()
{
    // Python GetCaptchaReq：GET /captcha，同一会话下取图；Referer 指向 signup。
    const QString referer = CurrentBase() + QStringLiteral("/signup");
    Send(QByteArrayLiteral("GET"), QStringLiteral("/captcha"), QByteArray(), cookie_, referer, 12000,
         512 * 1024,
         [this](bool ok, int status, const QByteArray &body, const QHash<QString, QString> &headers,
                const QString &error) {
             if (!ok) {
                 emit Log(QStringLiteral("account captcha failed: status=%1 err=%2")
                              .arg(status)
                              .arg(error));
                 emit CaptchaReady(false, {}, QString(), error);
                 return;
             }
             if (status != 200 || body.isEmpty()) {
                 emit CaptchaReady(false, {}, QString(), QStringLiteral("HTTP %1").arg(status));
                 return;
             }
             cookie_ = MergeCookies(cookie_, headers);
             const QString type = headers.value(QStringLiteral("content-type"));
             emit CaptchaReady(true, body, type, QString());
         });
}
