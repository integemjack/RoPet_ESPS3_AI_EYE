/*
 * servo_ctrl.c - C5 侧舵机运动控制
 *
 * 实现要点:
 *   1. LEDC 硬件 PWM, 50Hz / 14bit, 每路一个 channel。
 *   2. 20ms (50Hz) 插值任务。动作统一建模为每通道的正弦振荡
 *        angle = home + trim + offset·k + amp·k·sin(2π·t/T + φ)
 *      静态姿态就是 amp=0 的特例, 于是坐下/趴下和摇尾巴共用一套引擎。
 *   3. 任务优先级刻意压在 UART RX 任务之下: 运动卡住只是舵机不动,
 *      UART 卡住是整机掉线。
 *   4. 三重保护 —— 单动作超时、链路看门狗、空闲 detach。舵机堵转会把
 *      C5 的电拉垮进而断网, 所以宁可少动也不能堵着。
 */
#include "servo_ctrl.h"
#include "bridge_internal.h"

#include <math.h>
#include <string.h>

#include "driver/ledc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "servo";

/* ---------------- 硬件参数 ---------------- */

/* XIAO ESP32C5 引脚: 五路舵机连排 D0~D4 (四肢 D0~D3, 尾巴 D4)。
 *
 * 必须避开的:
 *   D9=GPIO9 / D10=GPIO10  网桥 UART (bridge_internal.h)
 *   GPIO13/14              USB D-/D+, 动了就烧不进去 (XIAO 上未引出)
 *
 * D2=GPIO25 和 D3=GPIO7 是 strapping 脚, 但都不参与启动模式 (C5 的启动模式
 * 由 GPIO26/27/28 决定, XIAO 上没引出), 所以能用:
 *   GPIO25 -> SDIO 采样/驱动时钟沿, 本工程不用 SDIO slave, 无影响
 *   GPIO7  -> JTAG 信号源选择, 最坏情况是复位瞬间干扰 C5 自己的 USB-JTAG 调试
 *
 * 剩余可用: D5=GPIO24, D6=GPIO11, D7=GPIO12, D8=GPIO8 */
#define SERVO_GPIO_FRONT_LEFT   1    /* D0 */
#define SERVO_GPIO_FRONT_RIGHT  0    /* D1 */
#define SERVO_GPIO_REAR_LEFT    25   /* D2 */
#define SERVO_GPIO_REAR_RIGHT   7    /* D3 */
#define SERVO_GPIO_TAIL         23   /* D4 */

#define SERVO_PWM_FREQ_HZ       50
#define SERVO_PWM_RES           LEDC_TIMER_14_BIT
#define SERVO_PWM_MAX_DUTY      ((1 << 14) - 1)
#define SERVO_MIN_PULSE_US      500      /* 对应 0° */
#define SERVO_MAX_PULSE_US      2500     /* 对应 180° */
#define SERVO_RANGE_DEG         180

#define SERVO_TICK_MS           20       /* 50Hz 插值 */
#define SERVO_IDLE_DETACH_MS    3000     /* 空闲多久后断 PWM 松力 */
#define SERVO_JOB_TIMEOUT_MS    10000    /* 单个动作最长执行时间 */
#define SERVO_LINK_TIMEOUT_MS   30000    /* 多久收不到 S3 的帧就认为链路死了 */
#define SERVO_QUEUE_DEPTH       4

/* 单个通道的机械定义。装反了把 invert 置 1, 不要去改动作表。 */
typedef struct {
    int      gpio;
    int16_t  min_x10;    /* 机械下限 (0.1°) */
    int16_t  max_x10;    /* 机械上限 */
    int16_t  home_x10;   /* 归中位 */
    bool     invert;
} servo_cfg_t;

static const servo_cfg_t s_cfg[BRIDGE_SERVO_COUNT] = {
    /* 四肢: 900 = 90.0° 为站立中位, ±60° 行程 */
    [BRIDGE_SERVO_FRONT_LEFT]  = { SERVO_GPIO_FRONT_LEFT,  300, 1500, 900, false },
    [BRIDGE_SERVO_FRONT_RIGHT] = { SERVO_GPIO_FRONT_RIGHT, 300, 1500, 900, true  },
    [BRIDGE_SERVO_REAR_LEFT]   = { SERVO_GPIO_REAR_LEFT,   300, 1500, 900, false },
    [BRIDGE_SERVO_REAR_RIGHT]  = { SERVO_GPIO_REAR_RIGHT,  300, 1500, 900, true  },
    /* 尾巴行程小一些, 转得快 */
    [BRIDGE_SERVO_TAIL]        = { SERVO_GPIO_TAIL,        450, 1350, 900, false },
};

/* ---------------- 动作表 ----------------
 * amp_pct    : 振幅, 相对该通道"半行程"的百分比, 负值表示反相。再乘 amount%。
 * phase_deg  : 相位。四肢步态靠相位差区分对角步/同步跳。
 * offset_pct : 静态偏置, 同样相对半行程, 也乘 amount%。amp 全 0 时就是静态姿态。
 * 通道顺序: FL, FR, RL, RR, TAIL
 */
typedef struct {
    int8_t  amp_pct[BRIDGE_SERVO_COUNT];
    int16_t phase_deg[BRIDGE_SERVO_COUNT];
    int8_t  offset_pct[BRIDGE_SERVO_COUNT];
} motion_pattern_t;

static const motion_pattern_t s_patterns[BRIDGE_MOTION_MAX] = {
    [BRIDGE_MOTION_HOME] = {
        {0, 0, 0, 0, 0}, {0, 0, 0, 0, 0}, {0, 0, 0, 0, 0},
    },
    [BRIDGE_MOTION_WAG_TAIL] = {          /* 只有尾巴动 */
        {0, 0, 0, 0, 100}, {0, 0, 0, 0, 0}, {0, 0, 0, 0, 0},
    },
    [BRIDGE_MOTION_WALK] = {              /* 对角步态: FL+RR 同相, FR+RL 反相 */
        {70, 70, 70, 70, 40},
        {0, 180, 180, 0, 0},
        {0, 0, 0, 0, 0},
    },
    [BRIDGE_MOTION_WAVE] = {              /* 抬前肢挥手, 具体哪只由 direction 选 */
        {0, 0, 0, 0, 50}, {0, 0, 0, 0, 0}, {0, 0, 0, 0, 0},
    },
    [BRIDGE_MOTION_STRETCH] = {           /* 前肢前伸、后肢后蹬, 慢 */
        {20, 20, 20, 20, 0},
        {0, 0, 180, 180, 0},
        {60, 60, -60, -60, 0},
    },
    [BRIDGE_MOTION_SHIVER] = {            /* 全身小幅抖 */
        {18, -18, 18, -18, 25}, {0, 0, 90, 90, 0}, {0, 0, 0, 0, 0},
    },
    [BRIDGE_MOTION_SIT] = {               /* 后肢折叠, 静态 */
        {0, 0, 0, 0, 30}, {0, 0, 0, 0, 0}, {20, 20, -90, -90, 0},
    },
    [BRIDGE_MOTION_LIE] = {               /* 四肢全折, 静态 */
        {0, 0, 0, 0, 0}, {0, 0, 0, 0, 0}, {-85, -85, -85, -85, 0},
    },
    [BRIDGE_MOTION_JUMP] = {              /* 四肢同相大幅 */
        {95, 95, 95, 95, 60}, {0, 0, 0, 0, 0}, {0, 0, 0, 0, 0},
    },
    [BRIDGE_MOTION_DANCE] = {             /* 四肢交替 + 尾巴双倍频的快摇 */
        {80, 80, 80, 80, 100},
        {0, 180, 90, 270, 0},
        {20, 20, 0, 0, 0},
    },
    [BRIDGE_MOTION_NOD_BODY] = {          /* 前肢下压再起, 像点头 */
        {55, 55, 0, 0, 30}, {0, 0, 0, 0, 180}, {-20, -20, 0, 0, 0},
    },
};

/* ---------------- 运行时状态 ---------------- */

typedef struct {
    uint16_t seq;
    uint8_t  action;
    uint8_t  repeat;
    uint16_t period_ms;
    uint8_t  amount;
    int8_t   direction;
    uint8_t  flags;
} motion_job_t;

static QueueHandle_t   s_queue;
static int16_t         s_cur_x10[BRIDGE_SERVO_COUNT];   /* 当前输出角度 */
static int16_t         s_trim_x10[BRIDGE_SERVO_COUNT];  /* NVS 里的中位微调 */
static bool            s_attached;
static volatile bool   s_abort;          /* 抢占/停止请求 */
static volatile uint8_t s_stop_flags;
static volatile int64_t s_last_alive_us;
static volatile uint8_t s_queued;        /* 仅用于状态上报 */

/* ---------------- 底层输出 ---------------- */

static inline ledc_channel_t chan_of(int ch) { return (ledc_channel_t)ch; }

static void servo_attach(void)
{
    if (s_attached) return;
    for (int i = 0; i < BRIDGE_SERVO_COUNT; i++) {
        ledc_channel_config_t cc = {
            .gpio_num   = s_cfg[i].gpio,
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel    = chan_of(i),
            .intr_type  = LEDC_INTR_DISABLE,
            .timer_sel  = LEDC_TIMER_0,
            .duty       = 0,
            .hpoint     = 0,
        };
        ledc_channel_config(&cc);
    }
    s_attached = true;
}

/* 断 PWM, 输出拉低。舵机收不到脉冲就松力, 既省电又不会在极限位上堵着发热。 */
static void servo_detach(void)
{
    if (!s_attached) return;
    for (int i = 0; i < BRIDGE_SERVO_COUNT; i++) {
        ledc_stop(LEDC_LOW_SPEED_MODE, chan_of(i), 0);
    }
    s_attached = false;
}

static void servo_write_raw(int ch, int16_t angle_x10)
{
    const servo_cfg_t *c = &s_cfg[ch];

    if (angle_x10 < c->min_x10) angle_x10 = c->min_x10;
    if (angle_x10 > c->max_x10) angle_x10 = c->max_x10;
    s_cur_x10[ch] = angle_x10;

    int32_t out_x10 = c->invert ? (int32_t)(SERVO_RANGE_DEG * 10) - angle_x10 : angle_x10;
    int32_t pulse_us = SERVO_MIN_PULSE_US +
        out_x10 * (SERVO_MAX_PULSE_US - SERVO_MIN_PULSE_US) / (SERVO_RANGE_DEG * 10);

    uint32_t duty = (uint32_t)((int64_t)pulse_us * (SERVO_PWM_MAX_DUTY + 1) *
                               SERVO_PWM_FREQ_HZ / 1000000);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, chan_of(ch), duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, chan_of(ch));
}

/* ---------------- trim 持久化 ---------------- */

static void trim_load(void)
{
    nvs_handle_t nvs;
    if (nvs_open("servo", NVS_READONLY, &nvs) != ESP_OK) return;
    size_t len = sizeof(s_trim_x10);
    if (nvs_get_blob(nvs, "trim", s_trim_x10, &len) != ESP_OK || len != sizeof(s_trim_x10)) {
        memset(s_trim_x10, 0, sizeof(s_trim_x10));
    }
    nvs_close(nvs);
}

static void trim_save(void)
{
    nvs_handle_t nvs;
    if (nvs_open("servo", NVS_READWRITE, &nvs) != ESP_OK) return;
    nvs_set_blob(nvs, "trim", s_trim_x10, sizeof(s_trim_x10));
    nvs_commit(nvs);
    nvs_close(nvs);
}

/* ---------------- 动作解算 ---------------- */

/* 该通道从中位往任一侧能走的最大行程, 振幅和偏置都按它的百分比算 */
static inline int16_t half_travel(int ch)
{
    int16_t up = s_cfg[ch].max_x10 - s_cfg[ch].home_x10;
    int16_t dn = s_cfg[ch].home_x10 - s_cfg[ch].min_x10;
    return up < dn ? up : dn;
}

/* 取基础动作表, 按 direction 做镜像/选边, 写入 out */
static void build_pattern(uint8_t action, int8_t direction, motion_pattern_t *out)
{
    *out = s_patterns[action];

    switch (action) {
    case BRIDGE_MOTION_WAVE: {
        /* direction < 0 挥左前肢, 否则右前肢。抬起来再摆, 所以偏置和振幅一起给。 */
        int limb = (direction < 0) ? BRIDGE_SERVO_FRONT_LEFT : BRIDGE_SERVO_FRONT_RIGHT;
        out->amp_pct[limb]    = 85;
        out->offset_pct[limb] = 70;
        break;
    }
    case BRIDGE_MOTION_WALK:
        /* direction < 0 = 后退: 整体相位翻转, 对角步态就反着走 */
        if (direction < 0) {
            for (int i = 0; i < BRIDGE_SERVO_COUNT; i++) {
                out->phase_deg[i] = (int16_t)((out->phase_deg[i] + 180) % 360);
            }
        }
        break;
    default:
        break;
    }
}

/* 把某一时刻 t (动作内相对毫秒) 的姿态算出来并输出 */
static void render_tick(const motion_pattern_t *p, const motion_job_t *job, uint32_t t_ms)
{
    float k = job->amount / 100.0f;
    float cycle = (float)t_ms / (float)job->period_ms;   /* 已走过的周期数 */

    for (int i = 0; i < BRIDGE_SERVO_COUNT; i++) {
        int16_t ht = half_travel(i);
        float amp    = ht * (p->amp_pct[i] / 100.0f) * k;
        float offset = ht * (p->offset_pct[i] / 100.0f) * k;
        float phase  = p->phase_deg[i] * (float)M_PI / 180.0f;

        float a = s_cfg[i].home_x10 + s_trim_x10[i] + offset +
                  amp * sinf(2.0f * (float)M_PI * cycle + phase);
        servo_write_raw(i, (int16_t)lroundf(a));
    }
}

/* 在 duration_ms 内平滑走回中位。被 abort 打断则立即返回。 */
static void glide_home(uint32_t duration_ms)
{
    int16_t from[BRIDGE_SERVO_COUNT];
    memcpy(from, s_cur_x10, sizeof(from));

    uint32_t steps = duration_ms / SERVO_TICK_MS;
    if (steps == 0) steps = 1;

    for (uint32_t s = 1; s <= steps; s++) {
        for (int i = 0; i < BRIDGE_SERVO_COUNT; i++) {
            int32_t target = s_cfg[i].home_x10 + s_trim_x10[i];
            int32_t a = from[i] + (target - from[i]) * (int32_t)s / (int32_t)steps;
            servo_write_raw(i, (int16_t)a);
        }
        vTaskDelay(pdMS_TO_TICKS(SERVO_TICK_MS));
    }
}

static void report_done(const motion_job_t *job, uint8_t result)
{
    bridge_motion_done_t d = {
        .seq = job->seq, .action = job->action, .result = result,
    };
    bridge_send_frame(BRIDGE_EVT_MOTION_DONE, BRIDGE_NO_LINK, (const uint8_t *)&d, sizeof(d));
}

/* ---------------- 运动任务 ---------------- */

static void run_job(const motion_job_t *job)
{
    if (job->action >= BRIDGE_MOTION_MAX || job->period_ms == 0) {
        report_done(job, BRIDGE_MOTION_R_BADPARAM);
        return;
    }

    servo_attach();

    /* HOME 是个特例: 不振荡, 直接平滑归位 */
    if (job->action == BRIDGE_MOTION_HOME) {
        glide_home(job->period_ms);
        report_done(job, s_abort ? BRIDGE_MOTION_R_PREEMPTED : BRIDGE_MOTION_R_OK);
        return;
    }

    motion_pattern_t pat;
    build_pattern(job->action, job->direction, &pat);

    uint32_t total_ms = (uint32_t)job->period_ms * (job->repeat ? job->repeat : 1);
    int64_t  start_us = esp_timer_get_time();
    uint8_t  result   = BRIDGE_MOTION_R_OK;

    for (uint32_t t = 0; t < total_ms; t += SERVO_TICK_MS) {
        if (s_abort) { result = BRIDGE_MOTION_R_PREEMPTED; break; }

        /* 单动作超时: 参数再离谱也不能让舵机一直堵着 */
        if (esp_timer_get_time() - start_us > (int64_t)SERVO_JOB_TIMEOUT_MS * 1000) {
            ESP_LOGW(TAG, "job timeout, action=%u", job->action);
            result = BRIDGE_MOTION_R_FAULT;
            break;
        }

        render_tick(&pat, job, t);
        vTaskDelay(pdMS_TO_TICKS(SERVO_TICK_MS));
    }

    if (result == BRIDGE_MOTION_R_OK && (job->flags & BRIDGE_MOTION_F_HOME_END)) {
        glide_home(200);
    }
    report_done(job, result);
}

static void servo_task(void *arg)
{
    (void)arg;
    int64_t idle_since_us = esp_timer_get_time();

    /* 上电先归中位, 让机构从一个已知姿态开始 */
    servo_attach();
    for (int i = 0; i < BRIDGE_SERVO_COUNT; i++) {
        s_cur_x10[i] = s_cfg[i].home_x10 + s_trim_x10[i];
        servo_write_raw(i, s_cur_x10[i]);
    }
    vTaskDelay(pdMS_TO_TICKS(400));

    while (1) {
        motion_job_t job;
        if (xQueueReceive(s_queue, &job, pdMS_TO_TICKS(SERVO_TICK_MS)) == pdTRUE) {
            s_abort = false;
            if (s_queued) s_queued--;
            run_job(&job);
            idle_since_us = esp_timer_get_time();
            continue;
        }

        /* --- 空闲期的三件事 --- */

        /* 1. STOP 请求的后处理 (STOP 帧只置标志, 真正的动作在任务里做,
         *    免得 UART 任务里跑 vTaskDelay 把收帧堵住) */
        uint8_t sf = s_stop_flags;
        if (sf) {
            s_stop_flags = 0;
            if (sf & BRIDGE_MOTION_STOP_HOME)   glide_home(250);
            if (sf & BRIDGE_MOTION_STOP_DETACH) servo_detach();
            idle_since_us = esp_timer_get_time();
            continue;
        }

        int64_t now = esp_timer_get_time();

        /* 2. 链路看门狗: S3 挂了或 UART 断了, 舵机不能一直堵在最后那个角度 */
        if (s_attached && s_last_alive_us > 0 &&
            now - s_last_alive_us > (int64_t)SERVO_LINK_TIMEOUT_MS * 1000) {
            ESP_LOGW(TAG, "S3 link silent > %d ms, homing and detaching", SERVO_LINK_TIMEOUT_MS);
            glide_home(400);
            servo_detach();
            s_last_alive_us = now;   /* 别每个 tick 都刷屏 */
            continue;
        }

        /* 3. 空闲 detach 省电 */
        if (s_attached && now - idle_since_us > (int64_t)SERVO_IDLE_DETACH_MS * 1000) {
            servo_detach();
        }
    }
}

/* ---------------- 对外接口 ---------------- */

void servo_ctrl_init(void)
{
    trim_load();

    ledc_timer_config_t tc = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .duty_resolution = SERVO_PWM_RES,
        .timer_num       = LEDC_TIMER_0,
        .freq_hz         = SERVO_PWM_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    /* 刻意不用 ESP_ERROR_CHECK: 这颗芯片的主职是 WiFi 网桥。舵机起不来
     * 顶多是不会动, 要是在这里 abort 就把整机网络也一起弄没了。 */
    esp_err_t err = ledc_timer_config(&tc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ledc_timer_config failed (%s), motion disabled", esp_err_to_name(err));
        return;
    }

    s_queue = xQueueCreate(SERVO_QUEUE_DEPTH, sizeof(motion_job_t));
    if (s_queue == NULL) {
        ESP_LOGE(TAG, "queue alloc failed, motion disabled");
        return;
    }
    s_last_alive_us = esp_timer_get_time();

    /* 优先级 5: 明显低于 UART RX 任务。运动卡住只是舵机不动,
     * UART 卡住是整机掉线, 后者严重得多。 */
    xTaskCreate(servo_task, "servo", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "servo ready: %d channels, tick=%dms", BRIDGE_SERVO_COUNT, SERVO_TICK_MS);
}

void servo_ctrl_play(const uint8_t *payload, uint16_t len)
{
    if (s_queue == NULL) return;
    if (len < sizeof(bridge_motion_play_t)) return;
    const bridge_motion_play_t *p = (const bridge_motion_play_t *)payload;

    motion_job_t job = {
        .seq       = p->seq,
        .action    = p->action,
        .repeat    = p->repeat ? p->repeat : 1,
        .period_ms = p->period_ms,
        .amount    = p->amount > 100 ? 100 : p->amount,
        .direction = p->direction,
        .flags     = p->flags,
    };
    if (job.period_ms < 200)  job.period_ms = 200;
    if (job.period_ms > 5000) job.period_ms = 5000;
    if (job.repeat > 50)      job.repeat = 50;

    if (p->flags & BRIDGE_MOTION_F_PREEMPT) {
        xQueueReset(s_queue);
        s_queued = 0;
        s_abort = true;          /* 打断正在跑的那个, 它会回 PREEMPTED */
    }

    ESP_LOGI(TAG, "play seq=%u action=%u repeat=%u period=%ums amount=%u dir=%d flags=0x%02x",
             job.seq, job.action, job.repeat, job.period_ms, job.amount,
             job.direction, job.flags);

    if (xQueueSend(s_queue, &job, 0) != pdTRUE) {
        ESP_LOGW(TAG, "queue full, drop action=%u seq=%u", job.action, job.seq);
        report_done(&job, BRIDGE_MOTION_R_PREEMPTED);
        return;
    }
    s_queued++;
}

void servo_ctrl_pose(const uint8_t *payload, uint16_t len)
{
    if (s_queue == NULL) return;
    if (len < 1) return;
    uint8_t count = payload[0];
    if (1 + (size_t)count * sizeof(bridge_servo_pose_t) > len) return;

    servo_attach();
    for (uint8_t i = 0; i < count; i++) {
        bridge_servo_pose_t item;
        memcpy(&item, payload + 1 + i * sizeof(bridge_servo_pose_t), sizeof(item));
        if (item.ch >= BRIDGE_SERVO_COUNT) continue;
        /* duration 交给调用方用连续 POSE 自己插值 —— 这条路径是标定用的,
         * 不该和动作引擎抢通道所有权。 */
        servo_write_raw(item.ch, item.angle_x10 + s_trim_x10[item.ch]);
    }
}

void servo_ctrl_stop(uint8_t flags)
{
    if (s_queue == NULL) return;
    if (flags & BRIDGE_MOTION_STOP_CLEAR) {
        xQueueReset(s_queue);
        s_queued = 0;
    }
    s_abort = true;
    /* 归位/detach 里有 vTaskDelay, 不能在 UART 任务上下文里做, 丢给运动任务 */
    s_stop_flags = flags & (BRIDGE_MOTION_STOP_HOME | BRIDGE_MOTION_STOP_DETACH);
}

void servo_ctrl_set_trim(const uint8_t *payload, uint16_t len)
{
    if (s_queue == NULL) return;
    if (len < 1) return;
    uint8_t count = payload[0];
    if (1 + (size_t)count * sizeof(bridge_servo_trim_t) > len) return;

    for (uint8_t i = 0; i < count; i++) {
        bridge_servo_trim_t item;
        memcpy(&item, payload + 1 + i * sizeof(bridge_servo_trim_t), sizeof(item));
        if (item.ch >= BRIDGE_SERVO_COUNT) continue;
        s_trim_x10[item.ch] = item.trim_x10;
    }
    trim_save();
    ESP_LOGI(TAG, "trim updated and saved");
}

void servo_ctrl_report_state(void)
{
    if (s_queue == NULL) return;
    bridge_motion_state_t st = {0};
    st.busy     = (uxQueueMessagesWaiting(s_queue) > 0 || s_queued > 0) ? 1 : 0;
    st.queued   = s_queued;
    st.attached = s_attached ? 1 : 0;
    st.count    = BRIDGE_SERVO_COUNT;
    for (int i = 0; i < BRIDGE_SERVO_COUNT; i++) {
        st.angle_x10[i] = s_cur_x10[i];
    }
    bridge_send_frame(BRIDGE_EVT_MOTION_STATE, BRIDGE_NO_LINK,
                      (const uint8_t *)&st, sizeof(st));
}

void servo_ctrl_notify_link_alive(void)
{
    s_last_alive_us = esp_timer_get_time();
}
