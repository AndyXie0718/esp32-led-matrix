#include "doom.h"

#include <math.h>
#include <string.h>

#define PI_F 3.14159265358979323846f

/**
 * @brief 将整数限制在指定区间内
 * @details 用于把火星飞溅后的目标坐标钳制在屏幕范围内，避免越界访问数组。
 */
static inline int iclamp(int x, int lo, int hi) {
    return x < lo ? lo : (x > hi ? hi : x);
}
static inline float fmax2(float a, float b) { return a > b ? a : b; }
static inline float fmin2(float a, float b) { return a < b ? a : b; }

/**
 * @brief Xorshift32 伪随机数生成器
 * @details 这是一个非常轻量的随机数算法，只需要少量位移和异或运算。
 *          在 ESP32 这种实时渲染场景里，它比标准 rand() 更适合做火焰特效。
 */
static inline uint32_t xs32(uint32_t* s) {
    uint32_t x = (*s == 0) ? 1u : *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return x;
}

/**
 * @brief 生成 0.0 ~ 1.0 之间的随机浮点数
 * @details 将 32 位随机整数映射到单位区间，供冷却、火星概率等计算使用。
 */
static inline float rng_f01(uint32_t* s) {
    return (xs32(s) >> 8) * (1.0f / 16777216.0f);
}

/**
 * @brief 生成 0 ~ n-1 的随机整数
 */
static inline int rng_int(uint32_t* s, int n) {
    return (int)(xs32(s) % (uint32_t)n);
}

/**
 * @brief 初始化 Doom 火焰引擎
 * @param f 火焰状态对象
 * @param seed 随机数种子
 */
void doom_fire_without_dsps_init(doom_fire_t* f, uint32_t seed) {
    memset(f, 0, sizeof(*f));
    f->decay = 3.0f;      // 基础衰减强度，值越大火焰越短、消散越快
    f->intensity = 36.0f; // 底部火源的初始热量
    f->rng = (seed == 0) ? 1u : seed;
}

/**
 * @brief 清空当前火焰场与下一帧缓冲
 * @details 相当于把屏幕上的火焰彻底熄灭。
 */
void doom_fire_without_dsps_reset(doom_fire_t* f) {
    memset(f->heat, 0, sizeof(f->heat));
    memset(f->next, 0, sizeof(f->next));
}

/**
 * @brief 获取当前帧热量场指针
 * @details 外部渲染模块可直接读取这份热量数据，再映射成颜色。
 */
const float* doom_fire_without_dsps_heat(const doom_fire_t* f) { return f->heat; }

/**
 * @brief 火焰扩散与上升传播的核心计算
 * @details 这一部分负责模拟火焰的“往上窜”和“左右摆动”。
 *          计算方式不是严格物理仿真，而是针对像素矩阵特效做的经验性加权滤波。
 * @param f 火焰状态对象
 * @param gravity_x 横向扰动，用来模拟风向或姿态倾斜造成的火焰偏移
 */
static void propagate(doom_fire_t* f, float gravity_x) {
    const float decay = f->decay;
    const float gx = gravity_x * 0.5f;

    const float center_x = (DOOM_W - 1) * 0.5f;
    const float gravity_strength = fabsf(gx);
    const float horizontal_drift = -(gx * 3.0f); // 横向漂移量，决定火焰整体偏左还是偏右

    // 四邻域加权系数：上方热量贡献最大，左右和下方参与补充
    const float w_up = 3.8f;
    const float w_below = 0.5f;
    const float w_left = fmax2(0.0f, 1.0f + horizontal_drift);
    const float w_right = fmax2(0.0f, 1.0f - horizontal_drift);
    const float total = fmax2(1.0f, w_up + w_below + w_left + w_right);
    const float inv_total = 1.0f / total; // 预先求倒数，减少循环中的除法开销
    const float taper_relax = fmax2(0.2f, 1.0f - gravity_strength * 0.3f);
    const int drift_shift = (horizontal_drift == 0.0f)
                                ? 0
                                : ((horizontal_drift > 0.0f) ? 1 : -1) *
                                      (int)ceilf(fabsf(horizontal_drift) *
                                                 0.8f);

    // 除了最底行外，其余行都会在本轮计算中被覆盖，因此这里只清空底部一行
    memset(&f->next[(DOOM_H - 1) * DOOM_W], 0, sizeof(float) * DOOM_W);

    for (int y = 0; y < DOOM_H - 1; y++) {
        const float rows_from_bottom = (float)((DOOM_H - 1) - y);
        const int src_row = (y + 1) * DOOM_W; // 当前像素主要参考它正下方一行
        const int below_row = (y + 2 < DOOM_H) ? ((y + 2) * DOOM_W) : src_row;
        // 离底部越远，整体偏斜越明显，用于制造火焰随风弯曲的效果
        const float lean_shift = horizontal_drift * rows_from_bottom * 0.35f;
        const float effective_center = center_x + lean_shift;
        // 越接近顶部，边缘收缩越明显，让火焰顶端变尖，不至于形成一整块平顶
        const float taper_k = (y < DOOM_H - 6)
                                  ? (((float)((DOOM_H - 6) - y)) * 0.045f *
                                     taper_relax)
                                  : 0.0f;

        for (int x = 0; x < DOOM_W; x++) {
            const int left_x = (x > 0) ? (x - 1) : x;
            const int right_x = (x + 1 < DOOM_W) ? (x + 1) : x;

            // 读取当前点及其邻域的旧热量
            const float val_up = f->heat[src_row + x];
            const float val_left = f->heat[src_row + left_x];
            const float val_right = f->heat[src_row + right_x];
            const float val_below = f->heat[below_row + x];

            // 用邻域加权平均得到传播后的基准温度
            const float avg = (val_up * w_up + val_left * w_left +
                               val_right * w_right + val_below * w_below) *
                              inv_total;

            // 随机冷却：让火焰边缘产生抖动和不规则纹理
            float cooling = decay * (rng_f01(&f->rng) * 0.4f + 0.8f);
            if (taper_k > 0.0f) {
                const float dist = fabsf((float)x - effective_center);
                // 离中心越远，额外冷却越强
                cooling += (dist * dist) * taper_k;
            }

            // 不能低于 0，避免负温度破坏后续颜色映射
            const float new_heat = fmax2(0.0f, avg - cooling);
            f->next[y * DOOM_W + x] = new_heat;

            // 生成少量火星粒子：高温点会随机向上抛出一团更亮的热量
            if (new_heat > 12.0f && rng_f01(&f->rng) < 0.006f) {
                const int jump = 2 + rng_int(&f->rng, 4);
                const int ty = y - jump;

                // 火星横向位置会受到随机扰动和整体风向影响
                int tx = x + (rng_int(&f->rng, 3) - 1) + drift_shift;
                tx = iclamp(tx, 0, DOOM_W - 1);

                if (ty >= 0) {
                    const int tidx = ty * DOOM_W + tx;
                    // 火星热量取较高值，但仍需限制上限，避免数值溢出或显示过曝
                    const float spark =
                        fmin2(new_heat * 2.2f + 10.0f, (float)DOOM_HEAT_MAX);
                    if (spark > f->next[tidx]) {
                        f->next[tidx] = spark;
                    }
                }
            }
        }
    }
}

/**
 * @brief 在底部注入火源，形成“燃料区”
 * @details 通过正弦波控制火源中心和强度，让底部火焰看起来像在呼吸和跳动。
 * @param f 火焰状态对象
 * @param t_ms 当前时间戳（毫秒）
 */
static void ignite(doom_fire_t* f, uint32_t t_ms) {
    const float t = (float)t_ms;

    // 叠加两个不同周期的正弦波，制造自然的摇摆效果
    const float sway = sinf(t / 800.0f) * 1.2f + sinf(t / 350.0f) * 0.5f;
    const float cx = (DOOM_W - 1) * 0.5f + sway;
    const float cy = (float)DOOM_H - 5.5f;
    const float radius = 4.8f; // 火源圆形区域半径，决定底部点火范围

    // 呼吸感：让底部火源强度缓慢上下浮动
    const float breathing = sinf(t / 200.0f) * 4.0f;
    // 再叠加一点随机闪烁，让火焰不那么规律
    const float flicker = rng_f01(&f->rng) * 2.0f;
    const float base_intensity = fmax2(0.0f, f->intensity + breathing - flicker);

    // 只在底部若干行中注入热量，减少计算量并符合火焰从底部燃起的视觉效果
    for (int y = DOOM_H - 12; y < DOOM_H; y++) {
        for (int x = 0; x < DOOM_W; x++) {
            const float dx = (float)x - cx;
            const float dy = (float)y - cy;
            const float dist = sqrtf(dx * dx + dy * dy);

            if (dist <= radius) {
                // 越靠近圆心越热，边缘平滑衰减
                const float falloff = cosf((dist / radius) * (PI_F * 0.5f));
                const float heat = base_intensity * falloff;
                const int idx = y * DOOM_W + x;
                if (heat > f->next[idx]) {
                    f->next[idx] = heat;
                }
            }
        }
    }
}

/**
 * @brief 执行一次火焰帧更新
 * @details 先做热量扩散，再点燃底部火源，最后把 next 帧拷贝到当前帧。
 * @param f 火焰状态对象
 * @param gravity_x 横向偏移参数
 * @param t_ms 当前时间戳（毫秒）
 */
void doom_fire_without_dsps_step(doom_fire_t* f, float gravity_x,
                                 uint32_t t_ms) {
    propagate(f, gravity_x);
    ignite(f, t_ms);
    // 将计算好的下一帧结果同步到当前显示缓冲
    memcpy(f->heat, f->next, sizeof(f->heat));
}
