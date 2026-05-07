# 第 1 课：环境搭建与工程源码解析

欢迎来到第一课！在这一课中，我们将完成开发环境的确认，并对整个项目的“骨架”进行全景扫描。理解工程的组织方式是编写和维护复杂嵌入式系统的第一步。

---

## 1. 核心开发环境：ESP-IDF
本项目基于 **Espressif IoT Development Framework (ESP-IDF)** 5.x 版本。这是一个基于 CMake 构建系统的多任务实时操作系统框架。

### 关键工具链确认：
*   **构建系统**：CMake (处理跨平台编译逻辑)。
*   **编译后端**：Ninja (执行并行编译)。
*   **包管理器**：IDF Component Manager (自动下载托管在云端的第三方库)。

---

## 2. 工程目录结构全扫描
打开 VS Code 左侧的服务树，你会看到以下布局。这是 ESP32 项目的标准结构，但加入了一些特定的层次：

### 📁 顶层配置文件
*   **`CMakeLists.txt`**：工程总入口，定义了项目名称及其包含的子目录。
*   **`sdkconfig`**：这是编译生成的最终配置文件。它包含了 1000 多个选项（如 CPU 频率、内存预留、编译优化级别等）。
*   **`partitions-16MiB.csv`**：**分区表**。定义了 ESP32 内部 Flash 的内存布局（例如多少空间存程序，多少空间存网页，多少空间存 Wi-Fi 账密）。

### 📁 `main/` (业务代码层)
这是你最常呆的地方，负责统筹所有的功能：
*   **`main.c`**：系统的初始化起点（`app_main`）。
*   **`web_control.c`**：处理复杂的网络交互逻辑（HTTP 服务器、AI 接口代理）。
*   **`idf_component.yml`**：**极度重要**。它声明了本项目依赖的在线库（如 `led_strip` 驱动和 `esp-dsp` 库）。

### 📁 `components/` (功能模块层)
本项目将功能解耦到了两类组件中：
*   **`BSP/` (Board Support Package)**：板级支持包。如果你换了不同型号的传感器或 LED 灯珠，只需修改这里。
    *   `mpu6050/`：处理陀螺仪 I2C 通信。
    *   `RGB/`：负责把像素颜色刷入灯条。
*   **`Middlewares/` (中间件层)**：这里是“大脑”相关的算法。
    *   `GRAVITY/`：复杂的数学计算，将加速度转化为重力角度。
    *   `SIM/`：特效管理器，决定现在是跑“火焰”还是跑“流水”。
    *   `FLIP/`：物理引擎，计算流体碰撞。

---

## 3. 源码入口初探：`app_main`
打开 [main/main.c](main/main.c)，观察函数的启动顺序：

1.  **`gravity_init()`**：初始化 I2C 和 传感器硬件。
2.  **`sim_manager_init()`**：在内存中为各种特效申请缓冲区。
3.  **`web_control_start()`**：启动 Wi-Fi 和 HTTP 服务器。
4.  **`xTaskCreate(...)`**：启动后台监控任务（如按键检测）。

---

## 4. 动手实践：编译你的第一版固件
在 VS Code 中，你可以通过以下步骤验证工程完整性（确保你已经安装了 Espressif 插件）：

1.  **选择设备型号**：点击状态栏底部的 `ESP-IDF: Set ESP-IDF Target`，选择 `esp32c3`（根据你实际芯片型号而定）。
2.  **配置系统参数**：点击 `ESP-IDF: SDK Configuration Editor (menuconfig)`，在这里你可以看到所有被代码引用的 `CONFIG_...` 前缀的宏定义。
3.  **开始编译**：点击状态栏的 **小火炬图标 (Build)**。

---

## 本课小结
*   ESP-IDF 是一个基于 CMake 的框架。
*   **解耦思想**是本项目的核心：驱动放 `BSP`，算法放 `Middlewares`，逻辑放 `main`。
*   `sdkconfig` 和 `partitions` 决定了硬件运行的基础环境。

---
**课后思考**：
尝试在 `main/main.c` 中找到 `g_boot_mode` 变量，看看它是如何决定设备开机时第一个显示的动画效果的？如果你把它从 `SIM_MODE_WATER` 改成 `SIM_MODE_FIRE`，开机会发生什么？

---
[👉 下一课：GPIO 与按键状态机](curriculum_outline.md#第二部分章节)建议在掌握本课结构后再开启。