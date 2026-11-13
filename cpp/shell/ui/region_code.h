// 地区名 ↔ 二字代码（逐条搬自 Python `tools/region_code.py`）。
//
// `NodeTester::FetchProxyIpList()` 会把地区代码拼进子域：
//   https://check.jpacg.cc/resolve?proxyip=proxyip.<code>.cmliussss.net
// 也就是说这里要的是 us / jp / hk 这种二字代码；而 ipinfo 只回「美国·Los Angeles」
// 这样的中文「国家·省份」。下拉里显示中文、发请求时换成代码，中间必须有这张表。
//
// 表里没有的地区：`ToCode()` 返回空（调用方回退"按原文发"，与 Python 一致）。
#pragma once

#include <QString>

namespace RegionCode {

// 「美国·Los Angeles」->「美国」；已经是纯名字原样返回
QString ShortName(const QString &text);

// 地区名（或已经是代码）-> 二字小写代码；认不出来返回空
QString ToCode(const QString &text);

// 二字代码 -> 中文名；认不出来返回空
QString ToName(const QString &code);

}  // namespace RegionCode
