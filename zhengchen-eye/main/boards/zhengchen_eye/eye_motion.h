/*
 * eye_motion.h - 语义层: 决定"什么时候做什么动作"
 *
 * 舵机(四肢+尾巴)物理上挂在 C5, 运动学在 C5 的 servo_ctrl.c 里。
 * 这里只负责把设备侧发生的事翻译成一条动作指令, 有四个触发源:
 *
 *   1. 本地意图      —— 直接从语音识别文本认关键词 (OnUserText), 不等服务端
 *                        LLM 决策。"往前走"落地就能动, 服务端没配 MCP 也不影响
 *   2. MCP 工具调用  —— 服务端 LLM 主动决定 (self.pet.*), 优先级最高, 抢占
 *   3. 情绪           —— {"type":"llm","emotion":"happy"}, 不抢占
 *   4. 设备状态/触摸  —— 说话时轻摇尾巴、被摸时反应, 优先级最低
 *
 * C5Motion 通过 resolver 惰性获取: UART 要等 StartNetwork() 之后才通,
 * 而本类在板卡构造期就要注册 MCP 工具, 两者时序对不上。
 */
#ifndef EYE_MOTION_H
#define EYE_MOTION_H

#include <functional>
#include <string>
#include <mutex>

#include "c5_motion.h"

class EyeMotion {
public:
    static EyeMotion& GetInstance();

    // resolver 在每次要发动作时调用, 返回 nullptr 表示当前网络模式不是 C5
    // (走 WIFI/ML307 时舵机链路不存在), 此时所有动作静默降级为空操作。
    void Initialize(std::function<C5Motion*()> resolver);

    // 注册 self.pet.* 工具。可在板卡构造期调用。
    void RegisterMcpTools();

    // 本地意图: 从语音识别文本里直接认动作关键词并下发, 不等服务端 LLM。
    // 由 EyeLcdDisplay::SetChatMessage("user", ...) 转发。
    // 返回 true 表示命中了某条关键词并已经发出动作帧。
    bool OnUserText(const char* text);

    // 情绪触发 (由 EyeLcdDisplay::SetEmotion 转发)。同一情绪重复不会重播。
    void OnEmotion(const char* emotion);

    // 设备状态变化 (待机/聆听/说话)
    void OnDeviceStateChanged(int previous_state, int current_state);

    // 触摸: left=true 摸左头, false 摸右头
    void OnTouch(bool left);

    void Stop();

private:
    EyeMotion() = default;

    C5Motion* Motion();
    bool Play(const MotionRequest& req);

    // 本地关键词刚播过同一个动作时返回 true。服务端 LLM 事后再调同名 MCP
    // 工具时用它跳过, 免得一句"往前走"走两遍。
    bool RecentlyPlayedLocally(bridge_motion_action_t action);
    void MarkPlayedLocally(bridge_motion_action_t action);

    std::function<C5Motion*()> resolver_;
    std::string last_emotion_;
    std::mutex  mutex_;
    bool        initialized_ = false;
    bool        callbacks_bound_ = false;

    // 本地意图最近一次下发的动作 (用于和 MCP 回调去重)
    bridge_motion_action_t local_action_ = BRIDGE_MOTION_MAX;
    int64_t                local_action_us_ = 0;
};

#endif  // EYE_MOTION_H
