#include "region_code.h"

#include <QHash>
#include <QStringList>

namespace {

// 中文国家/地区名 -> 二字代码。顺序与 Python 表一致，便于两边对照维护。
const QHash<QString, QString> &CountryCode()
{
    static const QHash<QString, QString> table = {
        // 亚太
        {QStringLiteral("日本"), QStringLiteral("jp")},
        {QStringLiteral("韩国"), QStringLiteral("kr")},
        {QStringLiteral("朝鲜"), QStringLiteral("kp")},
        {QStringLiteral("中国香港"), QStringLiteral("hk")},
        {QStringLiteral("香港"), QStringLiteral("hk")},
        {QStringLiteral("中国台湾"), QStringLiteral("tw")},
        {QStringLiteral("台湾"), QStringLiteral("tw")},
        {QStringLiteral("中国澳门"), QStringLiteral("mo")},
        {QStringLiteral("澳门"), QStringLiteral("mo")},
        {QStringLiteral("中国大陆"), QStringLiteral("cn")},
        {QStringLiteral("中国"), QStringLiteral("cn")},
        {QStringLiteral("新加坡"), QStringLiteral("sg")},
        {QStringLiteral("马来西亚"), QStringLiteral("my")},
        {QStringLiteral("泰国"), QStringLiteral("th")},
        {QStringLiteral("越南"), QStringLiteral("vn")},
        {QStringLiteral("印度尼西亚"), QStringLiteral("id")},
        {QStringLiteral("印尼"), QStringLiteral("id")},
        {QStringLiteral("菲律宾"), QStringLiteral("ph")},
        {QStringLiteral("印度"), QStringLiteral("in")},
        {QStringLiteral("巴基斯坦"), QStringLiteral("pk")},
        {QStringLiteral("孟加拉国"), QStringLiteral("bd")},
        {QStringLiteral("孟加拉"), QStringLiteral("bd")},
        {QStringLiteral("斯里兰卡"), QStringLiteral("lk")},
        {QStringLiteral("尼泊尔"), QStringLiteral("np")},
        {QStringLiteral("哈萨克斯坦"), QStringLiteral("kz")},
        {QStringLiteral("柬埔寨"), QStringLiteral("kh")},
        {QStringLiteral("缅甸"), QStringLiteral("mm")},
        {QStringLiteral("蒙古"), QStringLiteral("mn")},
        {QStringLiteral("乌兹别克斯坦"), QStringLiteral("uz")},
        // 美洲
        {QStringLiteral("美国"), QStringLiteral("us")},
        {QStringLiteral("加拿大"), QStringLiteral("ca")},
        {QStringLiteral("墨西哥"), QStringLiteral("mx")},
        {QStringLiteral("巴西"), QStringLiteral("br")},
        {QStringLiteral("阿根廷"), QStringLiteral("ar")},
        {QStringLiteral("智利"), QStringLiteral("cl")},
        {QStringLiteral("哥伦比亚"), QStringLiteral("co")},
        {QStringLiteral("秘鲁"), QStringLiteral("pe")},
        {QStringLiteral("委内瑞拉"), QStringLiteral("ve")},
        {QStringLiteral("厄瓜多尔"), QStringLiteral("ec")},
        {QStringLiteral("乌拉圭"), QStringLiteral("uy")},
        {QStringLiteral("巴拿马"), QStringLiteral("pa")},
        // 欧洲
        {QStringLiteral("英国"), QStringLiteral("gb")},
        {QStringLiteral("爱尔兰"), QStringLiteral("ie")},
        {QStringLiteral("法国"), QStringLiteral("fr")},
        {QStringLiteral("德国"), QStringLiteral("de")},
        {QStringLiteral("荷兰"), QStringLiteral("nl")},
        {QStringLiteral("比利时"), QStringLiteral("be")},
        {QStringLiteral("瑞士"), QStringLiteral("ch")},
        {QStringLiteral("奥地利"), QStringLiteral("at")},
        {QStringLiteral("意大利"), QStringLiteral("it")},
        {QStringLiteral("西班牙"), QStringLiteral("es")},
        {QStringLiteral("葡萄牙"), QStringLiteral("pt")},
        {QStringLiteral("波兰"), QStringLiteral("pl")},
        {QStringLiteral("捷克"), QStringLiteral("cz")},
        {QStringLiteral("斯洛伐克"), QStringLiteral("sk")},
        {QStringLiteral("匈牙利"), QStringLiteral("hu")},
        {QStringLiteral("罗马尼亚"), QStringLiteral("ro")},
        {QStringLiteral("保加利亚"), QStringLiteral("bg")},
        {QStringLiteral("希腊"), QStringLiteral("gr")},
        {QStringLiteral("瑞典"), QStringLiteral("se")},
        {QStringLiteral("挪威"), QStringLiteral("no")},
        {QStringLiteral("芬兰"), QStringLiteral("fi")},
        {QStringLiteral("丹麦"), QStringLiteral("dk")},
        {QStringLiteral("乌克兰"), QStringLiteral("ua")},
        {QStringLiteral("俄罗斯"), QStringLiteral("ru")},
        {QStringLiteral("白俄罗斯"), QStringLiteral("by")},
        {QStringLiteral("立陶宛"), QStringLiteral("lt")},
        {QStringLiteral("拉脱维亚"), QStringLiteral("lv")},
        {QStringLiteral("爱沙尼亚"), QStringLiteral("ee")},
        {QStringLiteral("塞尔维亚"), QStringLiteral("rs")},
        {QStringLiteral("克罗地亚"), QStringLiteral("hr")},
        {QStringLiteral("斯洛文尼亚"), QStringLiteral("si")},
        {QStringLiteral("冰岛"), QStringLiteral("is")},
        {QStringLiteral("卢森堡"), QStringLiteral("lu")},
        {QStringLiteral("摩尔多瓦"), QStringLiteral("md")},
        // 中东 / 非洲
        {QStringLiteral("土耳其"), QStringLiteral("tr")},
        {QStringLiteral("以色列"), QStringLiteral("il")},
        {QStringLiteral("阿联酋"), QStringLiteral("ae")},
        {QStringLiteral("阿拉伯联合酋长国"), QStringLiteral("ae")},
        {QStringLiteral("沙特阿拉伯"), QStringLiteral("sa")},
        {QStringLiteral("沙特"), QStringLiteral("sa")},
        {QStringLiteral("卡塔尔"), QStringLiteral("qa")},
        {QStringLiteral("科威特"), QStringLiteral("kw")},
        {QStringLiteral("阿曼"), QStringLiteral("om")},
        {QStringLiteral("约旦"), QStringLiteral("jo")},
        {QStringLiteral("巴林"), QStringLiteral("bh")},
        {QStringLiteral("伊朗"), QStringLiteral("ir")},
        {QStringLiteral("伊拉克"), QStringLiteral("iq")},
        {QStringLiteral("黎巴嫩"), QStringLiteral("lb")},
        {QStringLiteral("埃及"), QStringLiteral("eg")},
        {QStringLiteral("南非"), QStringLiteral("za")},
        {QStringLiteral("摩洛哥"), QStringLiteral("ma")},
        {QStringLiteral("尼日利亚"), QStringLiteral("ng")},
        {QStringLiteral("肯尼亚"), QStringLiteral("ke")},
        {QStringLiteral("突尼斯"), QStringLiteral("tn")},
        {QStringLiteral("阿尔及利亚"), QStringLiteral("dz")},
        // 大洋洲
        {QStringLiteral("澳大利亚"), QStringLiteral("au")},
        {QStringLiteral("新西兰"), QStringLiteral("nz")},
        {QStringLiteral("斐济"), QStringLiteral("fj")},
    };
    return table;
}

}  // namespace

namespace {

// 二字代码 -> 展示名（反查用的固定表）。
// 为什么不直接反查上面那张表：同一代码有多个别名（hk = 中国香港/香港），
// QHash 迭代顺序不确定，反查结果会随构建变化；这里显式钉死展示名。
const QHash<QString, QString> &CountryName()
{
    static const QHash<QString, QString> table = {
        {QStringLiteral("jp"), QStringLiteral("日本")},
        {QStringLiteral("kr"), QStringLiteral("韩国")},
        {QStringLiteral("kp"), QStringLiteral("朝鲜")},
        {QStringLiteral("hk"), QStringLiteral("香港")},
        {QStringLiteral("tw"), QStringLiteral("台湾")},
        {QStringLiteral("mo"), QStringLiteral("澳门")},
        {QStringLiteral("cn"), QStringLiteral("中国大陆")},
        {QStringLiteral("sg"), QStringLiteral("新加坡")},
        {QStringLiteral("my"), QStringLiteral("马来西亚")},
        {QStringLiteral("th"), QStringLiteral("泰国")},
        {QStringLiteral("vn"), QStringLiteral("越南")},
        {QStringLiteral("id"), QStringLiteral("印度尼西亚")},
        {QStringLiteral("ph"), QStringLiteral("菲律宾")},
        {QStringLiteral("in"), QStringLiteral("印度")},
        {QStringLiteral("pk"), QStringLiteral("巴基斯坦")},
        {QStringLiteral("bd"), QStringLiteral("孟加拉国")},
        {QStringLiteral("lk"), QStringLiteral("斯里兰卡")},
        {QStringLiteral("np"), QStringLiteral("尼泊尔")},
        {QStringLiteral("kz"), QStringLiteral("哈萨克斯坦")},
        {QStringLiteral("kh"), QStringLiteral("柬埔寨")},
        {QStringLiteral("mm"), QStringLiteral("缅甸")},
        {QStringLiteral("mn"), QStringLiteral("蒙古")},
        {QStringLiteral("uz"), QStringLiteral("乌兹别克斯坦")},
        {QStringLiteral("us"), QStringLiteral("美国")},
        {QStringLiteral("ca"), QStringLiteral("加拿大")},
        {QStringLiteral("mx"), QStringLiteral("墨西哥")},
        {QStringLiteral("br"), QStringLiteral("巴西")},
        {QStringLiteral("ar"), QStringLiteral("阿根廷")},
        {QStringLiteral("cl"), QStringLiteral("智利")},
        {QStringLiteral("co"), QStringLiteral("哥伦比亚")},
        {QStringLiteral("pe"), QStringLiteral("秘鲁")},
        {QStringLiteral("ve"), QStringLiteral("委内瑞拉")},
        {QStringLiteral("ec"), QStringLiteral("厄瓜多尔")},
        {QStringLiteral("uy"), QStringLiteral("乌拉圭")},
        {QStringLiteral("pa"), QStringLiteral("巴拿马")},
        {QStringLiteral("gb"), QStringLiteral("英国")},
        {QStringLiteral("ie"), QStringLiteral("爱尔兰")},
        {QStringLiteral("fr"), QStringLiteral("法国")},
        {QStringLiteral("de"), QStringLiteral("德国")},
        {QStringLiteral("nl"), QStringLiteral("荷兰")},
        {QStringLiteral("be"), QStringLiteral("比利时")},
        {QStringLiteral("ch"), QStringLiteral("瑞士")},
        {QStringLiteral("at"), QStringLiteral("奥地利")},
        {QStringLiteral("it"), QStringLiteral("意大利")},
        {QStringLiteral("es"), QStringLiteral("西班牙")},
        {QStringLiteral("pt"), QStringLiteral("葡萄牙")},
        {QStringLiteral("pl"), QStringLiteral("波兰")},
        {QStringLiteral("cz"), QStringLiteral("捷克")},
        {QStringLiteral("sk"), QStringLiteral("斯洛伐克")},
        {QStringLiteral("hu"), QStringLiteral("匈牙利")},
        {QStringLiteral("ro"), QStringLiteral("罗马尼亚")},
        {QStringLiteral("bg"), QStringLiteral("保加利亚")},
        {QStringLiteral("gr"), QStringLiteral("希腊")},
        {QStringLiteral("se"), QStringLiteral("瑞典")},
        {QStringLiteral("no"), QStringLiteral("挪威")},
        {QStringLiteral("fi"), QStringLiteral("芬兰")},
        {QStringLiteral("dk"), QStringLiteral("丹麦")},
        {QStringLiteral("ua"), QStringLiteral("乌克兰")},
        {QStringLiteral("ru"), QStringLiteral("俄罗斯")},
        {QStringLiteral("by"), QStringLiteral("白俄罗斯")},
        {QStringLiteral("lt"), QStringLiteral("立陶宛")},
        {QStringLiteral("lv"), QStringLiteral("拉脱维亚")},
        {QStringLiteral("ee"), QStringLiteral("爱沙尼亚")},
        {QStringLiteral("rs"), QStringLiteral("塞尔维亚")},
        {QStringLiteral("hr"), QStringLiteral("克罗地亚")},
        {QStringLiteral("si"), QStringLiteral("斯洛文尼亚")},
        {QStringLiteral("is"), QStringLiteral("冰岛")},
        {QStringLiteral("lu"), QStringLiteral("卢森堡")},
        {QStringLiteral("md"), QStringLiteral("摩尔多瓦")},
        {QStringLiteral("tr"), QStringLiteral("土耳其")},
        {QStringLiteral("il"), QStringLiteral("以色列")},
        {QStringLiteral("ae"), QStringLiteral("阿联酋")},
        {QStringLiteral("sa"), QStringLiteral("沙特阿拉伯")},
        {QStringLiteral("qa"), QStringLiteral("卡塔尔")},
        {QStringLiteral("kw"), QStringLiteral("科威特")},
        {QStringLiteral("om"), QStringLiteral("阿曼")},
        {QStringLiteral("jo"), QStringLiteral("约旦")},
        {QStringLiteral("bh"), QStringLiteral("巴林")},
        {QStringLiteral("ir"), QStringLiteral("伊朗")},
        {QStringLiteral("iq"), QStringLiteral("伊拉克")},
        {QStringLiteral("lb"), QStringLiteral("黎巴嫩")},
        {QStringLiteral("eg"), QStringLiteral("埃及")},
        {QStringLiteral("za"), QStringLiteral("南非")},
        {QStringLiteral("ma"), QStringLiteral("摩洛哥")},
        {QStringLiteral("ng"), QStringLiteral("尼日利亚")},
        {QStringLiteral("ke"), QStringLiteral("肯尼亚")},
        {QStringLiteral("tn"), QStringLiteral("突尼斯")},
        {QStringLiteral("dz"), QStringLiteral("阿尔及利亚")},
        {QStringLiteral("au"), QStringLiteral("澳大利亚")},
        {QStringLiteral("nz"), QStringLiteral("新西兰")},
        {QStringLiteral("fj"), QStringLiteral("斐济")},
    };
    return table;
}

}  // namespace

namespace RegionCode {

QString ShortName(const QString &text)
{
    const QString t = text.trimmed();
    if (t.isEmpty())
        return QString();
    // 与 Python _ShortName 同一组分隔符：「国家·省份」是最常见的形态
    static const QStringList kSeqs = {QStringLiteral("·"), QStringLiteral("・"),
                                     QStringLiteral("•"), QStringLiteral("/"),
                                     QStringLiteral("|"), QStringLiteral(" - "),
                                     QStringLiteral("-"), QStringLiteral("_")};
    for (const QString &sep : kSeqs) {
        const int at = t.indexOf(sep);
        if (at > 0)
            return t.left(at).trimmed();
    }
    return t;
}

QString ToCode(const QString &text)
{
    const QString t = text.trimmed();
    if (t.isEmpty())
        return QString();
    // 用户/配置里本来就是 "us"：两个 ASCII 字母直接当代码用
    if (t.size() == 2 && t.at(0).isLetter() && t.at(1).isLetter() && t.at(0).unicode() < 128
        && t.at(1).unicode() < 128) {
        return t.toLower();
    }
    const QString name = ShortName(t);
    const auto it = CountryCode().constFind(name);
    if (it != CountryCode().constEnd())
        return it.value();
    // 「中国香港」这类带前缀的写法：去掉"中国"再查（Python 同）
    if (name.startsWith(QStringLiteral("中国")) && name.size() > 2) {
        const auto sub = CountryCode().constFind(name.mid(2));
        if (sub != CountryCode().constEnd())
            return sub.value();
    }
    return QString();
}

QString ToName(const QString &code)
{
    const QString c = code.trimmed().toLower();
    if (c.isEmpty())
        return QString();
    const auto it = CountryName().constFind(c);
    return it == CountryName().constEnd() ? QString() : it.value();
}

}  // namespace RegionCode
