/*
 * c5_motion.h - S3 侧动作下发接口
 *
 * 舵机(四肢+尾巴)物理上挂在 C5 上, 因为 S3 的引脚已经用尽。
 * 分层:
 *   S3 (这里)  = 语义层。谁来决定做动作: MCP 工具调用 / 情绪 / 设备状态 / 触摸。
 *   C5         = 运动层。50Hz 插值 + LEDC 硬件 PWM, 见 c5_wifi_bridge/main/servo_ctrl.c。
 *
 * 一次调用只发一条 ~10 字节的帧。刻意不做"S3 按帧下发角度":
 * 那会和音频/socket 抢这根无硬件流控的 UART, 抖动会直接变成舵机颤动。
 */
#ifndef C5_MOTION_H
#define C5_MOTION_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

#include "bridge_protocol.h"
#include "c5_bridge.h"

// 一次动作的参数。默认值就是"中等速度、中等幅度、走一遍"。
struct MotionRequest {
    bridge_motion_action_t action = BRIDGE_MOTION_HOME;
    int  repeat    = 1;      // 重复周期数 1..50
    int  period_ms = 600;    // 单周期时长 200..5000, 越小越快
    int  amount    = 60;     // 幅度 0..100 (%)
    int  direction = 0;      // -1 / 0 / +1, 含义随动作而定
    bool preempt   = false;  // 打断当前动作立即执行
    bool home_end  = true;   // 走完回归中位
};

struct C5MotionState {
    bool    busy = false;
    uint8_t queued = 0;
    bool    attached = false;
    uint8_t count = 0;
    int16_t angle_x10[BRIDGE_SERVO_MAX] = {0};
};

class C5Motion {
public:
    explicit C5Motion(C5Bridge& bridge) : bridge_(bridge) {}

    // 播放一个预置动作。非阻塞, 返回是否成功发出帧。
    // 完成时 C5 会回 EVT_MOTION_DONE, 通过 OnDone 拿到结果。
    bool Play(const MotionRequest& req);

    // 便捷重载: Play(BRIDGE_MOTION_WAG_TAIL, 3) 这种写法
    bool Play(bridge_motion_action_t action, int repeat = 1, int amount = 60,
              int period_ms = 600, bool preempt = false);

    // 直接给角度, 仅用于标定/上位机调试。正常业务不要用这条路径。
    bool Pose(const std::vector<bridge_servo_pose_t>& poses);

    // 停止。打断时用 (例如用户打断了机器人说话, 动作不该继续跳)。
    void Stop(bool clear_queue = true, bool go_home = true, bool detach = false);

    // 写中位微调, 存在 C5 的 NVS 里 (跟着舵机走, 换主板不用重标)
    bool SetTrim(int ch, float degrees);

    // 请求一次状态上报 (异步, 结果走 OnState)
    void RequestState();

    void OnDone(std::function<void(uint16_t seq, uint8_t action, uint8_t result)> cb);
    void OnState(std::function<void(const C5MotionState&)> cb);

    // 由 C5Bridge::DispatchFrame 调用
    void HandleDoneFrame(const uint8_t* payload, uint16_t len);
    void HandleStateFrame(const uint8_t* payload, uint16_t len);

private:
    C5Bridge& bridge_;
    std::atomic<uint16_t> seq_{0};
    std::function<void(uint16_t, uint8_t, uint8_t)> on_done_;
    std::function<void(const C5MotionState&)> on_state_;
    std::mutex cb_mutex_;
};

#endif  // C5_MOTION_H
