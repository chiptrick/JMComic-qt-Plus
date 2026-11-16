// CPU 侧 IEEE 754 binary16（half）转换。
//
// Vulkan fp16 路径（NCNN fp16_packing 模式）的存储格式：显存里权重/激活都是
// half（2 字节），shader 里 unpack 成 float 累加、pack 回 half 写回。
// 转换只用位操作（无依赖），足够覆盖超分模型数值范围（|x| < 65504，
// denormal 直接按 0 处理——激活值不会这么小）。
#pragma once

#include <QtGlobal>

#include <cstdint>
#include <cstring>

namespace vk {

// float -> half（IEEE binary16）。NaN 保持 NaN，inf 保持 inf，超出范围按 inf。
inline quint16 FloatToHalf(float v)
{
    quint32 bits = 0;
    std::memcpy(&bits, &v, 4);
    const quint32 sign = (bits >> 16) & 0x8000u;
    const quint32 exp = (bits >> 23) & 0xFFu;
    const quint32 man = bits & 0x7FFFFFu;
    if (exp == 0xFF)   // inf / nan
        return quint16(sign | 0x7C00u | (man != 0 ? 0x200u : 0));
    const int e = int(exp) - 127 + 15;   // 重定标：fp32 指数 -127，half +15
    if (e >= 0x1F)   // 溢出 -> inf
        return quint16(sign | 0x7C00u);
    if (e <= 0) {
        // 太小：half denormal / 0。超分权重与激活都远大于 denormal，截断到 0
        // 造成的误差小于 fp16 的正常舍入误差
        if (e < -10)
            return quint16(sign);
        const quint32 m = man | 0x800000u;
        const quint32 shift = quint32(14 - e);
        return quint16(sign | ((m >> shift) + ((m >> (shift - 1)) & 1)));
    }
    // 常规数：round-to-nearest-even。+1 时尾数全 1 会自然进位到指数位
    //（0x3FF -> 0x400），这正是规格化进位的正确行为。
    quint32 h = sign | (quint32(e) << 10) | (man >> 13);
    const quint32 roundBits = man & 0x1FFFu;
    if (roundBits > 0x1000u || (roundBits == 0x1000u && (h & 1u) != 0))
        h += 1;
    return quint16(h);
}

// half -> float（IEEE binary16）
inline float HalfToFloat(quint16 h)
{
    const quint32 sign = quint32(h & 0x8000u) << 16;
    const quint32 exp = quint32((h >> 10) & 0x1Fu);
    const quint32 man = quint32(h & 0x3FFu);
    quint32 bits;
    if (exp == 0) {
        if (man == 0) {
            bits = sign;   // ±0
        } else {
            // half denormal -> fp32 normal：规格化
            quint32 e = 0;
            quint32 m = man;
            while ((m & 0x400u) == 0) {
                m <<= 1;
                ++e;
            }
            m &= 0x3FFu;
            bits = sign | ((127 - 15 - e + 1) << 23) | (m << 13);
        }
    } else if (exp == 0x1F) {
        bits = sign | 0xFFu << 23 | (man << 13);   // inf / nan
    } else {
        bits = sign | ((exp - 15 + 127) << 23) | (man << 13);
    }
    float out;
    std::memcpy(&out, &bits, 4);
    return out;
}

}  // namespace vk
