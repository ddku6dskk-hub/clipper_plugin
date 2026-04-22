#pragma once
#include <cmath>

namespace kyohei::dsp
{
/**
 * 対称・奇関数のソフトクリップシェイパー（C2 連続）。
 *
 *   |x| ≤ T - K/2 : y = x                             (線形)
 *   T-K/2 < |x| < T+K/2 : 5次 Hermite 多項式 blend
 *   |x| ≥ T + K/2 : y = sign(x) * T                   (ceiling pin)
 *
 * Knee 領域:
 *   u = (|x| - (T - K/2)) / K  ∈ [0, 1]
 *   f(u) = u - u³ + u⁴/2
 *   y   = sign(x) · [ T - K/2 + K · f(u) ]
 *
 * f は以下を満たす:
 *   f(0)=0, f(1)=1/2, f'(0)=1, f'(1)=0, f''(0)=0, f''(1)=0
 *   → 線形側と ceiling 側の両境界で値・1階・2階微分が連続（C2）。
 *   → 旧版の C1 (f''不連続) に対し、生成倍音のロールオフが −12→−18 dB/oct に急峻化。
 *
 * 奇対称のため 2次倍音ゼロ（奇数次倍音のみ）。
 */
template <typename T>
inline T symmetricSoftClip (T x, T threshold, T knee) noexcept
{
    const T ax = std::abs (x);
    const T halfK = knee * T (0.5);

    // 線形領域
    if (ax <= threshold - halfK)
        return x;

    // ceiling 領域（knee == 0 の hard clip もこちらに落ちる）
    if (ax >= threshold + halfK)
        return x < T (0) ? -threshold : threshold;

    // knee 遷移領域（C2 5次 Hermite、実質 4次で係数確定）
    const T u  = (ax - (threshold - halfK)) / knee;
    const T u2 = u * u;
    const T u3 = u2 * u;
    const T u4 = u2 * u2;
    const T f  = u - u3 + T (0.5) * u4;
    const T mag = threshold - halfK + knee * f;
    return x < T (0) ? -mag : mag;
}
} // namespace kyohei::dsp
