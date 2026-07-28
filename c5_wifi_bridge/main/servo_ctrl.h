/*
 * servo_ctrl.h - C5 侧舵机运动控制
 *
 * 分工: S3 决定"做什么动作"(语义层), C5 决定"怎么把它走出来"(运动层)。
 * S3 只发一条 ~10 字节的 CMD_MOTION_PLAY, 本模块在本地做 50Hz 插值和
 * LEDC 硬件 PWM 输出。UART 上不跑角度流。
 *
 * 机构: 四肢 + 尾巴, 共 5 路 (见 bridge_protocol.h 的 BRIDGE_SERVO_*)。
 */
#ifndef SERVO_CTRL_H
#define SERVO_CTRL_H

#include <stdint.h>
#include <stdbool.h>
#include "bridge_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 初始化 LEDC + 从 NVS 读 trim + 起运动任务。app_main 里调一次。 */
void servo_ctrl_init(void);

/* ---- 以下由 bridge_dispatch_frame 调用, 参数即帧 payload ---- */

/* 入队一个预置动作 (payload 短于结构体则忽略) */
void servo_ctrl_play(const uint8_t *payload, uint16_t len);

/* 直接给角度 (标定/上位机调试用)。payload: count(1) + count×bridge_servo_pose_t */
void servo_ctrl_pose(const uint8_t *payload, uint16_t len);

/* 停止。flags: BRIDGE_MOTION_STOP_* */
void servo_ctrl_stop(uint8_t flags);

/* 写中位微调并存 NVS。payload: count(1) + count×bridge_servo_trim_t */
void servo_ctrl_set_trim(const uint8_t *payload, uint16_t len);

/* 主动上报一次 EVT_MOTION_STATE */
void servo_ctrl_report_state(void);

/* 链路存活喂狗: 每收到任意一帧 S3 的数据就调一次。
 * 超过 SERVO_LINK_TIMEOUT_MS 没喂 -> 认为 S3 挂了或 UART 断了,
 * 自动回中位并 detach, 免得舵机一直堵在极限位置烧掉。 */
void servo_ctrl_notify_link_alive(void);

#ifdef __cplusplus
}
#endif

#endif /* SERVO_CTRL_H */
