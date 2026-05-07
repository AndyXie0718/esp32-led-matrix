# 第 13 课：平滑切换：引擎状态调度器

前面三节我们已经把火焰、流体碰撞、时间步进这些“物理味”很浓的内容拆开讲了。但真实项目里，效果模块不会永远只跑一个：用户可能上一秒在看火焰，下一秒切到水流，再下一秒切到自定义图案。

这就引出了本课的核心主题：**状态调度器（State Manager）**。它解决的不是“怎么画”，而是“**什么时候停、什么时候起、切换时如何不炸内存、不撕屏、不死锁**”。

本项目里，这个角色由 [components/Middlewares/SIM/sim_manager.c](components/Middlewares/SIM/sim_manager.c) 完成，它统一管理 `fire_sim`、`water_sim`、`custom_sim` 三种模式的生命周期。

---

## 1. 为什么需要调度器？

如果没有调度器，主程序会直接调用某个特效模块的 `start` 和 `stop`。这会带来三个问题：

1. **状态混乱**：上一个任务还没完全退出，下一个任务就开始写 LED 缓冲区，画面会出现闪烁和残影。
2. **资源泄漏**：每个特效都可能创建自己的任务、互斥锁、临时数组。如果没有统一回收，很快就会把 ESP32 的内存耗尽。
3. **调用者逻辑过于复杂**：上层代码不应该知道火焰、水流、自定义图案内部各自怎么启动，只需要告诉系统“我要切到哪个模式”。

所以调度器的本质就是：**把“模式切换”这件事封装成一个稳定、统一、可控的接口。**

---

## 2. 模式枚举：调度器管理哪些引擎？

先看头文件 [components/Middlewares/SIM/sim_manager.h](components/Middlewares/SIM/sim_manager.h)。

```c
#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SIM_MODE_NONE = -1,
    SIM_MODE_WATER = 0,
    SIM_MODE_FIRE = 1,
    SIM_MODE_CUSTOM = 2,
} sim_mode_t;

typedef struct {
    int core_id;
    uint32_t stack_size;
    int priority;
    uint32_t stop_timeout_ms;
} sim_runtime_config_t;

esp_err_t sim_manager_init(const sim_runtime_config_t* cfg);
esp_err_t sim_manager_start(sim_mode_t mode);
esp_err_t sim_manager_switch(sim_mode_t mode);
esp_err_t sim_manager_stop(void);
sim_mode_t sim_manager_current(void);

#ifdef __cplusplus
}
#endif
```

### 这段代码的意义

`sim_mode_t` 是状态机的核心。它不是“画面内容”，而是“当前运行哪一个特效模块”的抽象标签。

`SIM_MODE_NONE` 表示当前没有任何模拟任务运行。这个状态很重要，因为它是“安全切换”的前提：只有先回到空闲状态，才能放心启动下一个任务。

`sim_runtime_config_t` 则是调度器的运行参数：
- `core_id`：任务跑在哪个核上。
- `stack_size`：任务栈大小。
- `priority`：任务优先级。
- `stop_timeout_ms`：停止任务时最多等多久。

这里的设计很典型：**把策略参数和逻辑控制分离**。这样未来如果想调整流体任务的栈大小，不需要改调度逻辑，只改配置即可。

---

## 3. 启动、停止、切换：调度器的三大动作

打开实现文件 [components/Middlewares/SIM/sim_manager.c](components/Middlewares/SIM/sim_manager.c)，先看最关键的三个内部函数。

```c
static esp_err_t start_mode(sim_mode_t mode) {
    if (mode == SIM_MODE_WATER) {
        return water_sim_start(s_cfg.core_id, s_cfg.stack_size, s_cfg.priority);
    }
    if (mode == SIM_MODE_FIRE) {
        return fire_sim_start(s_cfg.core_id, s_cfg.stack_size, s_cfg.priority);
    }
    if (mode == SIM_MODE_CUSTOM) {
        return custom_sim_start(s_cfg.core_id, s_cfg.stack_size, s_cfg.priority);
    }
    return ESP_ERR_INVALID_ARG;
}

static esp_err_t stop_mode(sim_mode_t mode) {
    if (mode == SIM_MODE_WATER) {
        return water_sim_stop(s_cfg.stop_timeout_ms);
    }
    if (mode == SIM_MODE_FIRE) {
        return fire_sim_stop(s_cfg.stop_timeout_ms);
    }
    if (mode == SIM_MODE_CUSTOM) {
        return custom_sim_stop(s_cfg.stop_timeout_ms);
    }
    return ESP_OK;
}
```

### 这段代码说明了什么？

它把“模式编号”翻译成“具体模块 API”。

这就是典型的**分发器（dispatcher）**设计：上层只认 `SIM_MODE_FIRE`，底层去调用 `fire_sim_start()`。这样一来，系统其他地方不需要关心具体模块的函数名，耦合度就会大幅降低。

更重要的是，所有模式的启动参数是统一的：
- 核心号
- 栈大小
- 优先级

而停止参数统一为超时时间。这说明调度器已经把三套引擎的生命周期抽象成了一种协议。

---

## 4. 初始化：调度器先“就位”再工作

```c
esp_err_t sim_manager_init(const sim_runtime_config_t* cfg) {
    if (cfg) {
        s_cfg = *cfg;
    }
    s_current = SIM_MODE_NONE;
    s_inited = true;
    return ESP_OK;
}
```

### 这里的关键点

`sim_manager_init()` 不做任何实际启动，它只做两件事：

1. 复制配置到静态变量 `s_cfg`。
2. 标记系统已经初始化，`s_inited = true`。

这是一种很稳健的工程习惯：**初始化只负责建立规则，不负责消耗资源。**

如果初始化阶段就直接开任务，一旦后面上层配置还没准备好，就会出现“启动时机过早”的问题。把初始化和运行分开，系统行为会更可预测。

---

## 5. 启动：只允许从空闲进入运行

```c
esp_err_t sim_manager_start(sim_mode_t mode) {
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_current != SIM_MODE_NONE) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = start_mode(mode);
    if (err == ESP_OK) {
        s_current = mode;
    } else {
        ESP_LOGE(TAG, "start mode %d failed: %s", (int)mode, esp_err_to_name(err));
    }
    return err;
}
```

### 为什么要先检查 `s_current != SIM_MODE_NONE`？

因为这个接口的语义不是“再开一个新特效”，而是“从空闲状态进入一个特效”。

如果当前已经在跑火焰，再调用 `sim_manager_start(SIM_MODE_WATER)`，意味着系统要同时有两个特效抢同一块 LED 缓冲区。那会造成：
- 显示内容互相覆盖
- 任务竞争资源
- 调试难度指数上升

所以 `sim_manager_start()` 是一个**单入口、单实例**模型：同一时间只允许一个模拟任务存在。

---

## 6. 切换：先停，再起，中间必须保证一致性

这是本课最重要的部分。

```c
esp_err_t sim_manager_switch(sim_mode_t mode) {
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    if (mode == s_current) {
        return ESP_OK;
    }

    esp_err_t err = stop_mode(s_current);
    if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
        ESP_LOGE(TAG, "stop mode %d failed: %s", (int)s_current, esp_err_to_name(err));
        return err;
    }
    s_current = SIM_MODE_NONE;

    err = start_mode(mode);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "start mode %d failed: %s", (int)mode, esp_err_to_name(err));
        return err;
    }

    s_current = mode;
    return ESP_OK;
}
```

### 这里的逻辑为什么要这样写？

切换不是“简单地把当前模式变量改掉”，而是一个完整的生命周期交接：

1. 先停止旧模式。
2. 等旧任务真正退出。
3. 再启动新模式。
4. 启动成功后再更新当前状态。

这个顺序非常关键。因为如果先把 `s_current` 改成新模式，再去停旧任务，旧任务可能仍然在跑，而且它会以为自己还是当前唯一运行者。那就容易出现状态撕裂。

### 为什么允许 `ESP_ERR_TIMEOUT`？

因为停止任务是异步过程。`water_sim_stop()`、`fire_sim_stop()`、`custom_sim_stop()` 都是先把 `s_running = false`，然后等待任务自己退出。

如果任务因为某些原因没有及时退出来，调度器会接受 `ESP_ERR_TIMEOUT` 作为一种“已尽力停止但未成功”的状态，再继续做清理和状态复位。这是一个工程上比较现实的折中。

---

## 7. 停止：统一收口，回到空闲态

```c
esp_err_t sim_manager_stop(void) {
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = stop_mode(s_current);
    s_current = SIM_MODE_NONE;
    return err;
}
```

这段代码很短，但很重要。

它说明调度器的职责是“收口”：不管当前跑的是火焰还是水流，都要通过统一接口回到 `SIM_MODE_NONE`。对于上层逻辑来说，停机后系统就回到了一个干净状态，可以等待下一次启动。

---

## 8. 模式内部是如何配合调度器的？

调度器本身不创建任务，它只是调用各模块的 `start` / `stop`。真正的任务生命周期在各自模块里完成。

以 [components/Middlewares/SIM/custom_sim.c](components/Middlewares/SIM/custom_sim.c) 为例：

```c
esp_err_t custom_sim_start(int core_id, uint32_t stack_size, int priority) {
    if (s_task) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        if (!s_lock) {
            ESP_LOGE(TAG, "mutex create failed");
            return ESP_ERR_NO_MEM;
        }
    }

    s_running = true;
    BaseType_t ok = pdFAIL;
#if CONFIG_FREERTOS_NUMBER_OF_CORES > 1
    int target_core = core_id;
    if (target_core < 0 || target_core >= CONFIG_FREERTOS_NUMBER_OF_CORES) {
        target_core = tskNO_AFFINITY;
    }
    ok = xTaskCreatePinnedToCore(sim_task, "custom_sim", stack_size, NULL, priority, &s_task, target_core);
#else
    (void)core_id;
    ok = xTaskCreate(sim_task, "custom_sim", stack_size, NULL, priority, &s_task);
#endif
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "custom sim task create failed");
        s_running = false;
        s_task = NULL;
        return ESP_FAIL;
    }
    return ESP_OK;
}
```

### 这里有两个值得注意的设计点

`custom_sim_start()` 先检查 `s_task` 是否已经存在，这是为了防止重复创建任务。

然后它在创建任务前确保互斥锁 `s_lock` 存在。因为自定义图案的 bitmap 和颜色会被外部写入，而任务本身又在不断读取和绘制，所以必须通过互斥锁保证“写入”和“渲染”不会同时进行。

这就是调度器设计的意义：它只管切换状态；每个模式内部自己负责把“能安全停止、能安全重启”这件事做扎实。

---

## 9. 自定义模式：为什么特别适合讲“状态调度”

`custom_sim.c` 其实是一个很好的反例教学材料，因为它最能体现“状态切换”的价值。

```c
static void sim_task(void* arg) {
    (void)arg;

    rgb_init();
    rgb_clear();
    rgb_show();

    TickType_t last_wake = xTaskGetTickCount();
    const TickType_t frame_ticks = pdMS_TO_TICKS(1000 / CUSTOM_FPS);

    while (s_running) {
        vTaskDelayUntil(&last_wake, frame_ticks);

        if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) == pdTRUE) {
            draw_bitmap_locked();
            xSemaphoreGive(s_lock);
        }
    }

    rgb_clear();
    rgb_show();

    s_running = false;
    s_task = NULL;
    vTaskDelete(NULL);
}
```

### 为什么这个模块必须有 `s_lock`？

因为外部可能正在调用 `custom_sim_set_bitmap()` 改变位图，也可能在调用 `custom_sim_set_color8()` 改颜色，而任务循环每一帧都要读取并绘制这些数据。

如果没有锁，就会出现：
- 一半像素用旧数据
- 一半像素用新数据
- 甚至在颜色更新中间被打断，导致显示状态不一致

所以自定义模块里的互斥锁不是可选项，而是保证画面完整性的底线。

---

## 10. 代码中的关键工程思想

这套调度器虽然代码不长，但体现了几个很重要的嵌入式设计原则：

1. **单一职责**：`sim_manager` 只负责模式调度，不负责具体绘制。
2. **状态明确**：`SIM_MODE_NONE` 让系统始终知道自己是不是处于空闲状态。
3. **统一接口**：不管底层是火焰、水流还是自定义图案，上层都只调用同一套 `start / switch / stop`。
4. **优雅退出**：先置位 `s_running = false`，再让任务自行收尾，而不是直接粗暴删除。
5. **资源边界清晰**：每个模块负责自己的任务、锁和缓冲区，调度器只做生命周期协调。

---

## 本课小结

`sim_manager` 是这个项目的“交通警察”。它不负责造车，但负责指挥哪辆车什么时候上路、什么时候停车、什么时候换道。

从工程角度看，第 13 课的重点不是某一个算法，而是：**如何把多个实时任务组织成一个可切换、可回收、可预测的系统**。这也是从“能跑”进阶到“好维护”的关键一步。

---
[👉 返回课程大纲](curriculum_outline.md)
下一课我们将离开物理引擎，进入联网部分：**第 14 课：Wi-Fi AP/STA 混合模式**。你会看到硬件如何同时扮演热点和客户端，给网页控制和后续 NAT 转发打下基础。