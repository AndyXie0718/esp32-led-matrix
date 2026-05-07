// doom.c
#include "doom.h"

#include "dsps_mem.h"

#include <math.h>

#include <string.h>

#define PI_F 3.14159265358979323846f

/**
 * @brief 限定整数在某个区间内 (Clamp)
 */
static inline int iclamp(int x, int lo, int hi) {
    return x < lo ? lo : (x > hi ? hi : x);
}
static inline float fmax2(float a, float b) { return a > b ? a : b; }
static inline float fmin2(float a, float b) { return a < b ? a : b; }

/**
 * @brief 极简高效的随机数生成器 (Xorshift32 算法)
 * @details Doom 火焰特效运算需要产生大量的随机数。直接调用 rand() 会非常消耗系统资源。
 *          Xorshift 系列速度极快，只需要很少的移位和异或计算，很适合特效渲染。
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
 * @brief 快速生成 0.0 ~ 1.0 之间的随机浮点数
 */
static inline float rng_f01(uint32_t* s) {
    // 右移 8 位并除以 2^24 (16777216)，把整型映射到浮点
    return (xs32(s) >> 8) * (1.0f / 16777216.0f);
}

/**
 * @brief 生成 0 到 n-1 之间的随机整数
 */
static inline int rng_int(uint32_t* s, int n) {
    return (int)(xs32(s) % (uint32_t)n);
}

/**
 * @brief Doom 火焰引擎初始化
 * @param f 指向引擎数据结构的指针
 * @param seed 初始化随机数种子
 */
void doom_fire_init(doom_fire_t* f, uint32_t seed) {
    dsps_memset(f, 0, sizeof(*f));
    f->decay = 3.0f;       // 基础冷却/衰减率 (值越大火苗越短)
    f->intensity = 36.0f;  // 底部火源的初始温度绝对值
    f->rng = (seed == 0) ? 1u : seed;
}

/**
 * @brief 重置火焰引擎的状态，全屏灭火
 */
void doom_fire_reset(doom_fire_t* f) {
    dsps_memset(f->heat, 0, sizeof(f->heat));
    dsps_memset(f->next, 0, sizeof(f->next));
}

/**
 * @brief 获取当前帧渲染后的温度场
 */
const float* doom_fire_heat(const doom_fire_t* f) { return f->heat; }

/**
 * @brief 核心蔓延物理算法 (热对流与扩散)
 * @details 负责把底部的热量一格一格地往上方矩阵里抽，并根据重力/风向发生偏流，同时引入随机冷却以生成跳动的火苗。
 * @param f 引擎实例指针
 * @param gravity_x 水平方向的外力 (用于模拟风吹过火焰时产生的偏斜)
 */
static void propagate(doom_fire_t* f, float gravity_x) {
    const float decay = f->decay;
    const float gx = gravity_x * 0.5f;

    const float center_x = (DOOM_W - 1) * 0.5f;
    const float gravity_strength = fabsf(gx);
    const float horizontal_drift = -(gx * 3.0f); // 根据横向重力产生漂移量(风阻)

    // 权重的定义：决定了某个像素在计算周围温度时，往哪个方向吸热更多。
    // w_up最大，说明主要热量是往上跑的（也就是本像素的下方）
    const float w_up = 3.8f;
    const float w_below = 0.5f;
    const float w_left = fmax2(0.0f, 1.0f + horizontal_drift);
    const float w_right = fmax2(0.0f, 1.0f - horizontal_drift);
    const float total = fmax2(1.0f, w_up + w_below + w_left + w_right);
    const float inv_total = 1.0f / total; // 后续乘法代替除法，优化性能
    
    const float taper_relax = fmax2(0.2f, 1.0f - gravity_strength * 0.3f);
    
    // 风力导致火苗飞溅时的位置跳跃趋势
    const int drift_shift = (horizontal_drift == 0.0f)
                                ? 0
                                : ((horizontal_drift > 0.0f) ? 1 : -1) *
                                      (int)ceilf(fabsf(horizontal_drift) *
                                                 0.8f);

    // 新帧初始清空最后一行准备写入
    dsps_memset(&f->next[(DOOM_H - 1) * DOOM_W], 0, sizeof(float) * DOOM_W);

    // 倒序遍历（从上到下填充），利用下一行的旧热量算本行的新热量
    for (int y = 0; y < DOOM_H - 1; y++) {
        const float rows_from_bottom = (float)((DOOM_H - 1) - y);
        const int src_row = (y + 1) * DOOM_W;         // 它的正下方
        const int below_row = (y + 2 < DOOM_H) ? ((y + 2) * DOOM_W) : src_row; // 它的下下方
        
        // 随高度变化(离下端越远)发生更加严重的倾斜
        const float lean_shift = horizontal_drift * rows_from_bottom * 0.35f;
        const float effective_center = center_x + lean_shift;
        
        // 当向上蔓延到距离顶部 6 行时开始大幅增加边缘削减参数 (Taper)，以确保火苗不会平头顶死屏幕
        const float taper_k = (y < DOOM_H - 6)
                                  ? (((float)((DOOM_H - 6) - y)) * 0.045f *
                                     taper_relax)
                                  : 0.0f;

        for (int x = 0; x < DOOM_W; x++) {
            const int left_x = (x > 0) ? (x - 1) : x;
            const int right_x = (x + 1 < DOOM_W) ? (x + 1) : x;

            // 采样四周的老温度
            const float val_up = f->heat[src_row + x];
            const float val_left = f->heat[src_row + left_x];
            const float val_right = f->heat[src_row + right_x];
            const float val_below = f->heat[below_row + x];

            // 通过权重进行滤波加权平均算法
            const float avg = (val_up * w_up + val_left * w_left +
                               val_right * w_right + val_below * w_below) *
                              inv_total;

            // 基于随机数的非线性冷却 (火焰的纹理撕裂质感就来自于此处的随机)
            float cooling = decay * (rng_f01(&f->rng) * 0.4f + 0.8f);
            
            // 加上高空锥形衰减 (让火苗顶端呈尖锐状)
            if (taper_k > 0.0f) {
                const float dist = fabsf((float)x - effective_center);
                cooling += (dist * dist) * taper_k;
            }

            // 得出该点扣除冷却后的新残余热量
            const float new_heat = fmax2(0.0f, avg - cooling);
            f->next[y * DOOM_W + x] = new_heat;

            // --- 火星子 (Sparks) 生成系统 --- 
            // 若当前点特别热(>12.0f)，且运气好命中概率 (0.006f)，就在它的斜上方抛射出一团火星
            if (new_heat > 12.0f && rng_f01(&f->rng) < 0.006f) {
                const int jump = 2 + rng_int(&f->rng, 4); // 向上跳跃的距离：2~5
                const int ty = y - jump;

                // 结合风向偏移火星的落点 X
                int tx = x + (rng_int(&f->rng, 3) - 1) + drift_shift; 
                tx = iclamp(tx, 0, DOOM_W - 1);

                if (ty >= 0) {
                    const int tidx = ty * DOOM_W + tx;
                    // 在落点上增加额外的热量，表现为飘走的火星颗粒
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
 * @brief 在底部发生点火源
 * @details 负责绘制屏幕底部的核心起源热量。利用正弦函数随时间波动的特性实现了火源的呼吸感与晃动感。
 * @param f 引擎实例指针
 * @param t_ms 经过的系统时间 (用于产生基于时间序列的正弦扰动)
 */
static void ignite(doom_fire_t* f, uint32_t t_ms) {
    const float t = (float)t_ms;

    // 用长周期和短周期的正弦波叠加，制造随机不规律的主火堆摇摆
    float sway = sinf(t / 800.0f) * 1.2f + sinf(t / 350.0f) * 0.5f;
    const float sway_limit = fmax2(0.6f, (float)DOOM_W * 0.16f);
    sway = fmin2(fmax2(sway, -sway_limit), sway_limit);
    
    // cx, cy 为主发火圆心
    float cx = (DOOM_W - 1) * 0.5f + sway;
    cx = fmin2(fmax2(cx, 0.8f), (float)DOOM_W - 1.8f);
    const float cy = (float)DOOM_H - 1.8f;
    
    // 发火源的半径根据屏幕大小自动调整
    float radius = (float)((DOOM_W < DOOM_H) ? DOOM_W : DOOM_H) * 0.45f;
    if (radius < 1.8f) {
        radius = 1.8f;
    }
    if (radius > 3.8f) {
        radius = 3.8f;
    }

    // 呼吸波动与闪烁因子
    const float breathing = sinf(t / 200.0f) * 4.0f;
    const float flicker = rng_f01(&f->rng) * 2.0f;
    const float base_intensity = fmax2(0.0f, f->intensity + breathing - flicker);

    // 火源一般只从最底下的几行开始生成，为了减少计算量提频，上头的就不管了
    int y_start = DOOM_H - 12;
    if (y_start < 0) {
        y_start = 0;
    }

    // 将这团火源的热量压入最底部的网格
    for (int y = y_start; y < DOOM_H; y++) {
        for (int x = 0; x < DOOM_W; x++) {
            const float dx = (float)x - cx;
            const float dy = (float)y - cy;
            const float dist = sqrtf(dx * dx + dy * dy);

            // 当落入发火源圆圈半径内部
            if (dist <= radius) {
                // 用余弦函数生成平滑边缘过渡 (中心最热，边缘衰减)
                const float falloff = cosf((dist / radius) * (PI_F * 0.5f));
                const float heat = base_intensity * falloff;
                const int idx = y * DOOM_W + x;
                
                // 取最大值合并进下一帧的温度场数据里
                if (heat > f->next[idx]) {
                    f->next[idx] = heat;
                }
            }
        }
    }
}

/**
 * @brief 执行整个 Doom 火焰的单步物理迭代演算
 * @param f 引擎实例指针
 * @param gravity_x 重力/重力风阻（外部施加环境）
 * @param t_ms 运行绝对时间
 */
void doom_fire_step(doom_fire_t* f, float gravity_x, uint32_t t_ms) {
    // 1. 旧热量做向上扩散蔓延、制冷与飞火星子
    propagate(f, gravity_x);
    // 2. 覆盖生成底部的新一簇原核心热源
    ignite(f, t_ms);
    // 3. 用高速 DSP 库把后备帧 next 的显存全部推覆合并到主显示 heat 帧上
    dsps_memcpy(f->heat, f->next, sizeof(f->heat));
}
