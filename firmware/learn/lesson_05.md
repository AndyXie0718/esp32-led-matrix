# 第 5 课：二维像素映射与坐标变换

在掌握了如何点亮单个 LED（第 4 课）和获取重力感应（第 3 课）之后，本课我们将学习如何在 8x8 的“画布”上进行有组织地绘图。这是从“点亮一个灯”到“创作一幅画”的关键跨越。

---

## 1. 逻辑画布 vs 物理硬件

在编写算法（如画一个圆或模拟水流）时，我们习惯使用**笛卡尔坐标系**：
*   左下角为 $(0, 0)$
*   向右为 $X$ 轴正方向
*   向上为 $Y$ 轴正方向

然而，物理硬件上的 LED 往往是按一行一行的**串联**方式焊接的。
*   **物理序列**：$0, 1, 2, ..., 63$

---

## 2. 坐标转换函数：`panel_led_index`

打开 [components/BSP/RGB/panel_config.h](components/BSP/RGB/panel_config.h)，你会发现这一段精妙的代码，它负责将逻辑上的 `(x, y)` 转换为灯带中的索引：

```c
static inline int panel_led_index(int x, int y) {
#if PANEL_COLUMN_MAJOR
    // 列优先布线：第一列是 0-7，第二列是 8-15...
    return x * PANEL_HEIGHT + y;
#endif

#if PANEL_SERPENTINE
    // 蛇形布线：为了省线，第一行从左向右，第二行从右向左
    if (y & 1) // 奇数行
        return y * PANEL_WIDTH + (PANEL_WIDTH - 1 - x);
#endif

    // 标准行优先布线
    return y * PANEL_WIDTH + x;
}
```

### 为什么要有这些判断？
因为市面上买到的 8x8 点阵屏屏接线方式各异。通过宏定义（`PANEL_COLUMN_MAJOR`），我们只需修改一次这个函数，上层所有的绘图算法（火焰、流水等）都无需改动，这就是**软件抽象层**的威力。

---

## 3. 在画布上绘图：从点到线

基于 `panel_led_index`，我们可以封装更高级的绘图 API。

### (1) 绘制一个点
在 [rgb.c](components/BSP/RGB/rgb.c) 中：
```c
void rgb_set_pixel(int x, int y, uint8_t r, uint8_t g, uint8_t b) {
    if (x < 0 || x >= PANEL_WIDTH || y < 0 || y >= PANEL_HEIGHT) return;
    int index = panel_led_index(x, y);
    rgb_set(index, r, g, b); // 最终调用底层驱动
}
```

### (2) 绘制矩形（思维拓展）
要画一个填满屏幕的蓝色矩形，你会怎么写？
```c
for (int x = 0; x < PANEL_WIDTH; x++) {
    for (int y = 0; y < PANEL_HEIGHT; y++) {
        rgb_set_pixel(x, y, 0, 0, 255);
    }
}
rgb_show(); // 刷新显示
```

---

## 4. 旋转与翻转：适配不同的安装角度

由于我们的设备带有重力感应，如果板子被倒着安装了，我们可以通过**坐标变换**在软件层面瞬间“翻转”屏幕。

*   **垂直翻转**：使用 `(x, PANEL_HEIGHT - 1 - y)` 代替 `(y)`。
*   **90度旋转**：交换 `x` 和 `y` 的角色。

在 `Middlewares/GRAVITY` 模块中，传感器读取到的重力矢量会反馈给这些坐标，从而实现“无论你怎么拿板子，水总是往下流”的神奇效果。

---

## 5. 动手实践

1.  **绘制对角线**：尝试在 `main.c` 的初始化后，写一个循环绘制一条从 $(0,0)$ 到 $(7,7)$ 的白线。
2.  **镜像实验**：修改 `panel_config.h` 中的 `panel_led_index`，在本来的返回值基础上尝试将 `x` 改为 `7 - x`，看看你的显示是否发生了左右镜像？
3.  **颜色渐变**：写一个嵌套循环，让 $R$ 通道随 $X$ 坐标增加，让 $B$ 通道随 $Y$ 坐标增加，看看能否在 8x8 的方阵里画出一张漂亮的彩色色卡？

---

## 本课小结
*   **坐标转换**是联通数学算法与物理硬件的桥梁。
*   利用 `panel_led_index` 实现了硬件无关性。
*   掌握了 `(x, y)` 绘图法后，我们就可以在下一章进入复杂的**物理动画模拟**了！

---
[👉 下一课：FreeRTOS 任务创建与生命周期](curriculum_outline.md#第二部分章节) 准备好进入多任务并发的世界了吗？