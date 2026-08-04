/*
 * eye_motion.cc - 语义层实现
 */
#include "eye_motion.h"

#include <cstring>
#include <string>

#include <esp_log.h>

#include "device_state.h"
#include "mcp_server.h"

#define TAG "EyeMotion"

EyeMotion& EyeMotion::GetInstance() {
    static EyeMotion instance;
    return instance;
}

void EyeMotion::Initialize(std::function<C5Motion*()> resolver) {
    std::lock_guard<std::mutex> lk(mutex_);
    resolver_ = std::move(resolver);
    initialized_ = true;
}

C5Motion* EyeMotion::Motion() {
    std::function<C5Motion*()> r;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        if (!initialized_) return nullptr;
        r = resolver_;
    }
    if (!r) return nullptr;

    C5Motion* m = r();
    if (m == nullptr) return nullptr;

    // 首次拿到链路时挂上回帧日志。放在这里而不是 Initialize(): 板卡构造期
    // C5Motion 还没准备好, resolver 那时返回 nullptr。
    if (!callbacks_bound_) {
        callbacks_bound_ = true;
        m->OnDone([](uint16_t seq, uint8_t action, uint8_t result) {
            ESP_LOGI(TAG, "done: seq=%u action=%u result=%u", seq, action, result);
        });
        m->OnState([](const C5MotionState& s) {
            ESP_LOGI(TAG, "state: busy=%d queued=%u attached=%d count=%u "
                          "angles=%.1f %.1f %.1f %.1f %.1f",
                     (int)s.busy, s.queued, (int)s.attached, s.count,
                     s.angle_x10[0] / 10.0f, s.angle_x10[1] / 10.0f, s.angle_x10[2] / 10.0f,
                     s.angle_x10[3] / 10.0f, s.angle_x10[4] / 10.0f);
        });
    }
    return m;
}

bool EyeMotion::Play(const MotionRequest& req) {
    auto* m = Motion();
    if (m == nullptr) {
        // 非 C5 网络模式下没有舵机链路, 静默降级。不要在这里报错刷屏 ——
        // 每条情绪都会走到这儿。
        return false;
    }
    return m->Play(req);
}

void EyeMotion::Stop() {
    auto* m = Motion();
    if (m) m->Stop(/*clear_queue=*/true, /*go_home=*/true, /*detach=*/false);
}

// ---------------- 情绪映射 ----------------

namespace {

struct EmotionMotion {
    const char*            emotion;
    bridge_motion_action_t action;
    int                    repeat;
    int                    period_ms;
    int                    amount;
};

// 小智协议里的情绪名 -> 动作。没列到的情绪不做动作 (neutral/relaxed 等
// 本来就该安静), 与其硬凑不如让它站着别动。
const EmotionMotion kEmotionTable[] = {
    {"happy",      BRIDGE_MOTION_WAG_TAIL, 4, 320, 90},
    {"laughing",   BRIDGE_MOTION_WAG_TAIL, 5, 260, 100},
    {"funny",      BRIDGE_MOTION_DANCE,    2, 700, 70},
    {"delicious",  BRIDGE_MOTION_WAG_TAIL, 4, 300, 85},
    {"silly",      BRIDGE_MOTION_DANCE,    2, 800, 60},
    {"loving",     BRIDGE_MOTION_WAG_TAIL, 3, 550, 55},
    {"kissy",      BRIDGE_MOTION_NOD_BODY, 2, 700, 60},
    {"confident",  BRIDGE_MOTION_NOD_BODY, 1, 800, 70},
    {"cool",       BRIDGE_MOTION_WAG_TAIL, 2, 600, 50},
    {"winking",    BRIDGE_MOTION_WAVE,     2, 500, 75},
    {"surprised",  BRIDGE_MOTION_JUMP,     1, 450, 80},
    {"shocked",    BRIDGE_MOTION_JUMP,     2, 380, 95},
    {"angry",      BRIDGE_MOTION_SHIVER,   6, 260, 90},
    {"embarrassed",BRIDGE_MOTION_SHIVER,   4, 400, 40},
    {"confused",   BRIDGE_MOTION_SHIVER,   3, 500, 35},
    {"thinking",   BRIDGE_MOTION_NOD_BODY, 1, 1200, 35},
    {"sad",        BRIDGE_MOTION_LIE,      1, 1500, 70},
    {"crying",     BRIDGE_MOTION_LIE,      1, 1800, 85},
    {"sleepy",     BRIDGE_MOTION_LIE,      1, 2000, 90},
};

}  // namespace

void EyeMotion::OnEmotion(const char* emotion) {
    if (emotion == nullptr) return;

    {
        std::lock_guard<std::mutex> lk(mutex_);
        // 同一情绪连着来好几条是常态 (每句话都会带), 重复播会让机器人一直抽搐
        if (last_emotion_ == emotion) return;
        last_emotion_ = emotion;
    }

    for (const auto& e : kEmotionTable) {
        if (strcmp(e.emotion, emotion) != 0) continue;

        MotionRequest req;
        req.action    = e.action;
        req.repeat    = e.repeat;
        req.period_ms = e.period_ms;
        req.amount    = e.amount;
        // 情绪不抢占 MCP 显式调用的动作 —— 服务端明确要求做的事优先。
        req.preempt   = false;
        // 趴下/坐下这类静态姿态要保持住, 不能走完就弹回中位。
        req.home_end  = (e.action != BRIDGE_MOTION_LIE && e.action != BRIDGE_MOTION_SIT);

        ESP_LOGI(TAG, "emotion '%s' -> action %d", emotion, (int)e.action);
        Play(req);
        return;
    }
}

// ---------------- 设备状态 ----------------

void EyeMotion::OnDeviceStateChanged(int previous_state, int current_state) {
    // 说话结束 / 被打断 -> 立刻停下并归位。用户已经打断了, 机器人还在跳很蠢。
    if (previous_state == kDeviceStateSpeaking && current_state != kDeviceStateSpeaking) {
        Stop();
        std::lock_guard<std::mutex> lk(mutex_);
        last_emotion_.clear();   // 下一轮同样的情绪要能重新触发
        return;
    }

    // 开始说话 -> 轻摇尾巴当伴随动作。幅度刻意压小, 免得盖过情绪动作。
    if (current_state == kDeviceStateSpeaking && previous_state != kDeviceStateSpeaking) {
        MotionRequest req;
        req.action    = BRIDGE_MOTION_WAG_TAIL;
        req.repeat    = 2;
        req.period_ms = 700;
        req.amount    = 35;
        req.preempt   = false;
        Play(req);
    }
}

// ---------------- 触摸 ----------------

void EyeMotion::OnTouch(bool left) {
    // 摸头 -> 抬对应那侧的前肢蹭一下 + 摇尾巴。抢占, 因为这是即时的物理交互,
    // 反馈晚了就没意义了。
    MotionRequest req;
    req.action    = BRIDGE_MOTION_WAVE;
    req.repeat    = 2;
    req.period_ms = 450;
    req.amount    = 70;
    req.direction = left ? -1 : 1;
    req.preempt   = true;
    Play(req);
}

// ---------------- MCP 工具 ----------------

void EyeMotion::RegisterMcpTools() {
    auto& mcp = McpServer::GetInstance();

    // 通用的"播一个动作"闭包工厂, 省得每个工具都抄一遍参数解包。
    auto make_handler = [this](bridge_motion_action_t action, bool hold) {
        return [this, action, hold](const PropertyList& properties) -> ReturnValue {
            MotionRequest req;
            req.action    = action;
            req.repeat    = properties["repeat"].value<int>();
            req.period_ms = properties["speed"].value<int>();
            req.amount    = properties["amount"].value<int>();
            req.preempt   = true;      // 服务端显式要求的动作, 打断当前的
            req.home_end  = !hold;
            if (!Play(req)) {
                return std::string("舵机链路不可用 (当前网络模式不是 C5)");
            }
            return true;
        };
    };

    // repeat / speed / amount 三个参数所有动作共用, 语义一致, 便于 LLM 复用。
    auto common_props = []() {
        return PropertyList({
            Property("repeat", kPropertyTypeInteger, 3, 1, 50),
            Property("speed", kPropertyTypeInteger, 600, 200, 5000),
            Property("amount", kPropertyTypeInteger, 70, 0, 100),
        });
    };

    const char* kParamDoc =
        "repeat: 重复次数(1-50); speed: 单次周期毫秒(200-5000, 越小越快); "
        "amount: 幅度百分比(0-100)";

    mcp.AddTool("self.pet.wag_tail", std::string("摇尾巴, 表示开心。") + kParamDoc,
                common_props(), make_handler(BRIDGE_MOTION_WAG_TAIL, false));

    mcp.AddTool("self.pet.jump", std::string("原地蹦跳, 表示兴奋或惊讶。") + kParamDoc,
                common_props(), make_handler(BRIDGE_MOTION_JUMP, false));

    mcp.AddTool("self.pet.dance", std::string("跳舞, 四肢交替配合摇尾巴。") + kParamDoc,
                common_props(), make_handler(BRIDGE_MOTION_DANCE, false));

    mcp.AddTool("self.pet.stretch", std::string("伸懒腰。") + kParamDoc,
                common_props(), make_handler(BRIDGE_MOTION_STRETCH, false));

    mcp.AddTool("self.pet.shiver", std::string("发抖, 表示害怕或寒冷。") + kParamDoc,
                common_props(), make_handler(BRIDGE_MOTION_SHIVER, false));

    mcp.AddTool("self.pet.nod", std::string("上身点头, 表示同意。") + kParamDoc,
                common_props(), make_handler(BRIDGE_MOTION_NOD_BODY, false));

    // 静态姿态: hold=true, 走完不回中位, 一直保持到下一个动作
    mcp.AddTool("self.pet.sit", std::string("坐下并保持。") + kParamDoc,
                common_props(), make_handler(BRIDGE_MOTION_SIT, true));

    mcp.AddTool("self.pet.lie_down", std::string("趴下并保持, 表示疲倦或难过。") + kParamDoc,
                common_props(), make_handler(BRIDGE_MOTION_LIE, true));

    mcp.AddTool("self.pet.walk",
                std::string("四肢行走。direction: 1=前进, -1=后退。") + kParamDoc,
                PropertyList({
                    Property("repeat", kPropertyTypeInteger, 4, 1, 50),
                    Property("speed", kPropertyTypeInteger, 700, 200, 5000),
                    Property("amount", kPropertyTypeInteger, 70, 0, 100),
                    Property("direction", kPropertyTypeInteger, 1, -1, 1),
                }),
                [this](const PropertyList& properties) -> ReturnValue {
                    MotionRequest req;
                    req.action    = BRIDGE_MOTION_WALK;
                    req.repeat    = properties["repeat"].value<int>();
                    req.period_ms = properties["speed"].value<int>();
                    req.amount    = properties["amount"].value<int>();
                    req.direction = properties["direction"].value<int>();
                    req.preempt   = true;
                    if (!Play(req)) return std::string("舵机链路不可用");
                    return true;
                });

    mcp.AddTool("self.pet.turn",
                std::string("原地转向。direction: -1=左转, 1=右转。") + kParamDoc,
                PropertyList({
                    Property("repeat", kPropertyTypeInteger, 3, 1, 50),
                    Property("speed", kPropertyTypeInteger, 700, 200, 5000),
                    Property("amount", kPropertyTypeInteger, 75, 0, 100),
                    Property("direction", kPropertyTypeInteger, 1, -1, 1),
                }),
                [this](const PropertyList& properties) -> ReturnValue {
                    MotionRequest req;
                    req.action    = BRIDGE_MOTION_TURN;
                    req.repeat    = properties["repeat"].value<int>();
                    req.period_ms = properties["speed"].value<int>();
                    req.amount    = properties["amount"].value<int>();
                    req.direction = properties["direction"].value<int>();
                    req.preempt   = true;
                    if (!Play(req)) return std::string("舵机链路不可用");
                    return true;
                });

    mcp.AddTool("self.pet.wave",
                std::string("抬起一只前肢挥手打招呼。direction: -1=左前肢, 1=右前肢。") + kParamDoc,
                PropertyList({
                    Property("repeat", kPropertyTypeInteger, 3, 1, 50),
                    Property("speed", kPropertyTypeInteger, 450, 200, 5000),
                    Property("amount", kPropertyTypeInteger, 75, 0, 100),
                    Property("direction", kPropertyTypeInteger, 1, -1, 1),
                }),
                [this](const PropertyList& properties) -> ReturnValue {
                    MotionRequest req;
                    req.action    = BRIDGE_MOTION_WAVE;
                    req.repeat    = properties["repeat"].value<int>();
                    req.period_ms = properties["speed"].value<int>();
                    req.amount    = properties["amount"].value<int>();
                    req.direction = properties["direction"].value<int>();
                    req.preempt   = true;
                    if (!Play(req)) return std::string("舵机链路不可用");
                    return true;
                });

    mcp.AddTool("self.pet.stand_up", "站起来, 四肢回到中位。", PropertyList(),
                [this](const PropertyList&) -> ReturnValue {
                    MotionRequest req;
                    req.action    = BRIDGE_MOTION_HOME;
                    req.period_ms = 500;
                    req.preempt   = true;
                    if (!Play(req)) return std::string("舵机链路不可用");
                    return true;
                });

    mcp.AddTool("self.pet.stop", "立即停止所有动作并回到中位。", PropertyList(),
                [this](const PropertyList&) -> ReturnValue {
                    Stop();
                    return true;
                });

    ESP_LOGI(TAG, "registered self.pet.* motion tools");
}
