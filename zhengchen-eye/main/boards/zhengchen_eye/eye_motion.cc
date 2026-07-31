/*
 * eye_motion.cc - 语义层实现
 */
#include "eye_motion.h"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>

#include <esp_log.h>
#include <esp_timer.h>

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

// ---------------- 本地意图 (不经 LLM) ----------------
//
// 为什么要有这一层: 动作原本只有两条路进来 —— 服务端 LLM 调 MCP 工具, 或者
// LLM 给的情绪标签。两条都要等服务端一个来回, 而且服务端不支持 MCP 时"往前走"
// 根本不会动。这里在 STT 文本落地的那一刻直接认关键词, 本地就把帧发出去:
//   * 不依赖服务端是否支持工具调用
//   * 比 LLM 回来快 1-2 秒, 说完就动
// LLM 那条路照常保留 —— 它能理解"绕着桌子转一圈"这种关键词表覆盖不了的说法。
// 两条路撞在一起时靠 RecentlyPlayedLocally() 去重。

namespace {

// 哨兵: 表示"停止"而不是某个具体动作
constexpr bridge_motion_action_t kIntentStop = BRIDGE_MOTION_MAX;

struct IntentRule {
    const char*            keyword;
    bridge_motion_action_t action;
    int                    repeat;
    int                    period_ms;
    int                    amount;
};

// 只把 A-Z 压成小写, 不碰多字节。用 std::tolower 逐字节扫 UTF-8 是危险的:
// 汉字的续接字节 >127, 在非 C 区域下可能被改掉。
std::string LowerAscii(const std::string& s) {
    std::string r = s;
    for (char& c : r) {
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    }
    return r;
}

bool IsAsciiKeyword(const char* k) {
    for (const char* p = k; *p != '\0'; ++p) {
        if (static_cast<unsigned char>(*p) > 127) return false;
    }
    return true;
}

// 中文按子串找; 英文必须按词边界找 —— 英文子串会大面积误伤:
// "visit" 里有 "sit", "sidewalk" 里有 "walk", "going" 里有 "go"。
size_t FindKeyword(const std::string& t, const char* kw) {
    if (!IsAsciiKeyword(kw)) return t.find(kw);

    const size_t len = strlen(kw);
    for (size_t from = 0;;) {
        size_t p = t.find(kw, from);
        if (p == std::string::npos) return p;

        bool left_ok  = (p == 0) ||
                        !isalnum(static_cast<unsigned char>(t[p - 1]));
        bool right_ok = (p + len >= t.size()) ||
                        !isalnum(static_cast<unsigned char>(t[p + len]));
        if (left_ok && right_ok) return p;
        from = p + 1;
    }
}

// 顺序即优先级, 第一条命中的赢。所以:
//   * "停" 类放最前 —— 任何时候都要能叫停
//   * "跳舞" 必须排在 "跳" 前面, 否则 "跳个舞" 会被当成蹦跳
//   * 泛化的单字词 ("走"/"尾巴") 垫底做兜底
const IntentRule kIntentTable[] = {
    // —— 停止 ——
    {"停下",     kIntentStop,              0, 0,    0},
    {"停止",     kIntentStop,              0, 0,    0},
    {"别动",     kIntentStop,              0, 0,    0},
    {"不要动",   kIntentStop,              0, 0,    0},
    {"停一下",   kIntentStop,              0, 0,    0},
    {"stop",     kIntentStop,              0, 0,    0},
    {"freeze",   kIntentStop,              0, 0,    0},
    {"hold still", kIntentStop,            0, 0,    0},
    {"stay still", kIntentStop,            0, 0,    0},
    {"don't move", kIntentStop,            0, 0,    0},

    // —— 站起/归位 ——
    {"站起来",   BRIDGE_MOTION_HOME,       1, 500,  0},
    {"起立",     BRIDGE_MOTION_HOME,       1, 500,  0},
    {"站好",     BRIDGE_MOTION_HOME,       1, 500,  0},
    {"站直",     BRIDGE_MOTION_HOME,       1, 500,  0},
    {"stand up", BRIDGE_MOTION_HOME,       1, 500,  0},
    {"get up",   BRIDGE_MOTION_HOME,       1, 500,  0},
    {"stand",    BRIDGE_MOTION_HOME,       1, 500,  0},

    // —— 跳舞 (必须在"跳"之前) ——
    {"跳舞",     BRIDGE_MOTION_DANCE,      3, 700,  75},
    {"跳个舞",   BRIDGE_MOTION_DANCE,      3, 700,  75},
    {"来段舞",   BRIDGE_MOTION_DANCE,      3, 700,  75},
    {"舞蹈",     BRIDGE_MOTION_DANCE,      3, 700,  75},
    {"扭一扭",   BRIDGE_MOTION_DANCE,      3, 650,  70},
    {"dance",    BRIDGE_MOTION_DANCE,      3, 700,  75},
    {"dancing",  BRIDGE_MOTION_DANCE,      3, 700,  75},
    {"perform",  BRIDGE_MOTION_DANCE,      3, 700,  75},
    {"do a trick", BRIDGE_MOTION_DANCE,    3, 700,  75},
    {"show me a trick", BRIDGE_MOTION_DANCE, 3, 700, 75},

    // —— 行走 (四肢) ——
    {"往前走",   BRIDGE_MOTION_WALK,       4, 700,  70},
    {"向前走",   BRIDGE_MOTION_WALK,       4, 700,  70},
    {"往后退",   BRIDGE_MOTION_WALK,       3, 700,  70},
    {"向后退",   BRIDGE_MOTION_WALK,       3, 700,  70},
    {"前进",     BRIDGE_MOTION_WALK,       4, 700,  70},
    {"后退",     BRIDGE_MOTION_WALK,       3, 700,  70},
    {"倒退",     BRIDGE_MOTION_WALK,       3, 700,  70},
    {"退后",     BRIDGE_MOTION_WALK,       3, 700,  70},
    {"走两步",   BRIDGE_MOTION_WALK,       2, 700,  70},
    {"走一走",   BRIDGE_MOTION_WALK,       4, 700,  70},
    {"走起来",   BRIDGE_MOTION_WALK,       4, 700,  70},
    {"走路",     BRIDGE_MOTION_WALK,       4, 700,  70},
    {"过来",     BRIDGE_MOTION_WALK,       4, 650,  75},
    {"walk forward", BRIDGE_MOTION_WALK,   4, 700,  70},
    {"go forward",   BRIDGE_MOTION_WALK,   4, 700,  70},
    {"move forward", BRIDGE_MOTION_WALK,   4, 700,  70},
    {"walk backward", BRIDGE_MOTION_WALK,  3, 700,  70},
    {"go backward",  BRIDGE_MOTION_WALK,   3, 700,  70},
    {"back up",      BRIDGE_MOTION_WALK,   3, 700,  70},
    {"step back",    BRIDGE_MOTION_WALK,   3, 700,  70},
    {"come here",    BRIDGE_MOTION_WALK,   4, 650,  75},
    {"come over",    BRIDGE_MOTION_WALK,   4, 650,  75},
    {"walk",         BRIDGE_MOTION_WALK,   4, 700,  70},
    // ASR 容错: 实测 "walk forward" 被听成 "welcome forward all that"。
    // 这类词组在正常对话里不成句, 拿来当指令不会误伤。
    {"welcome forward", BRIDGE_MOTION_WALK, 4, 700, 70},

    // —— 转向 (方向在下面按"左"字判定, 所以这里左右共用规则) ——
    {"向左转",   BRIDGE_MOTION_TURN,       3, 700,  75},
    {"往左转",   BRIDGE_MOTION_TURN,       3, 700,  75},
    {"向右转",   BRIDGE_MOTION_TURN,       3, 700,  75},
    {"往右转",   BRIDGE_MOTION_TURN,       3, 700,  75},
    {"左转",     BRIDGE_MOTION_TURN,       3, 700,  75},
    {"右转",     BRIDGE_MOTION_TURN,       3, 700,  75},
    {"左拐",     BRIDGE_MOTION_TURN,       3, 700,  75},
    {"右拐",     BRIDGE_MOTION_TURN,       3, 700,  75},
    {"掉头",     BRIDGE_MOTION_TURN,       6, 700,  80},
    {"调头",     BRIDGE_MOTION_TURN,       6, 700,  80},   // 同音异形, STT 多输出这个
    // STT 容错: 实测中文 ASR 稳定把"左转/右转"听成"左板/右板"("转"→"板")。
    // 这两个词在中文里不成词, 拿来当转向指令不会误伤正常说话。
    {"向左板",   BRIDGE_MOTION_TURN,       3, 700,  75},
    {"向右板",   BRIDGE_MOTION_TURN,       3, 700,  75},
    {"左板",     BRIDGE_MOTION_TURN,       3, 700,  75},
    {"右板",     BRIDGE_MOTION_TURN,       3, 700,  75},
    {"转圈",     BRIDGE_MOTION_TURN,       6, 650,  80},
    {"转一圈",   BRIDGE_MOTION_TURN,       6, 650,  80},
    {"转个圈",   BRIDGE_MOTION_TURN,       6, 650,  80},
    {"转身",     BRIDGE_MOTION_TURN,       4, 700,  80},
    {"turn left",   BRIDGE_MOTION_TURN,    3, 700,  75},
    {"turn right",  BRIDGE_MOTION_TURN,    3, 700,  75},
    {"turn around", BRIDGE_MOTION_TURN,    6, 700,  80},
    {"spin around", BRIDGE_MOTION_TURN,    6, 650,  80},
    {"spin",        BRIDGE_MOTION_TURN,    6, 650,  80},
    {"turn",        BRIDGE_MOTION_TURN,    3, 700,  75},

    // —— 蹦跳 ——
    {"跳一下",   BRIDGE_MOTION_JUMP,       2, 450,  80},
    {"跳一跳",   BRIDGE_MOTION_JUMP,       2, 450,  80},
    {"跳起来",   BRIDGE_MOTION_JUMP,       2, 450,  85},
    {"原地跳",   BRIDGE_MOTION_JUMP,       3, 450,  80},
    {"蹦",       BRIDGE_MOTION_JUMP,       2, 450,  80},
    {"jump",     BRIDGE_MOTION_JUMP,       2, 450,  80},
    {"hop",      BRIDGE_MOTION_JUMP,       2, 450,  80},

    // —— 静态姿态 ——
    {"坐下",     BRIDGE_MOTION_SIT,        1, 800,  80},
    {"坐好",     BRIDGE_MOTION_SIT,        1, 800,  80},
    {"请坐",     BRIDGE_MOTION_SIT,        1, 800,  80},
    {"趴下",     BRIDGE_MOTION_LIE,        1, 1200, 80},
    {"躺下",     BRIDGE_MOTION_LIE,        1, 1200, 80},
    {"趴着",     BRIDGE_MOTION_LIE,        1, 1200, 80},
    {"睡觉",     BRIDGE_MOTION_LIE,        1, 1500, 85},
    {"休息",     BRIDGE_MOTION_LIE,        1, 1500, 80},
    {"sit down", BRIDGE_MOTION_SIT,        1, 800,  80},
    {"sit",      BRIDGE_MOTION_SIT,        1, 800,  80},
    {"lie down", BRIDGE_MOTION_LIE,        1, 1200, 80},
    {"lay down", BRIDGE_MOTION_LIE,        1, 1200, 80},
    {"go to sleep", BRIDGE_MOTION_LIE,     1, 1500, 85},
    {"take a rest", BRIDGE_MOTION_LIE,     1, 1500, 80},
    {"sleep",    BRIDGE_MOTION_LIE,        1, 1500, 85},

    // —— 上肢 / 招呼 ——
    {"挥挥手",   BRIDGE_MOTION_WAVE,       3, 450,  75},
    {"挥手",     BRIDGE_MOTION_WAVE,       3, 450,  75},
    {"招手",     BRIDGE_MOTION_WAVE,       3, 450,  75},
    {"打招呼",   BRIDGE_MOTION_WAVE,       3, 450,  75},
    {"握手",     BRIDGE_MOTION_WAVE,       2, 500,  70},
    {"抬手",     BRIDGE_MOTION_WAVE,       2, 500,  70},
    {"举手",     BRIDGE_MOTION_WAVE,       2, 500,  70},
    // "shake hands" 必须排在下面 shiver 的 "shake" 前面, 否则握手会变成发抖
    {"shake hands", BRIDGE_MOTION_WAVE,    2, 500,  70},
    {"high five",   BRIDGE_MOTION_WAVE,    2, 500,  70},
    {"wave",        BRIDGE_MOTION_WAVE,    3, 450,  75},
    {"say hi",      BRIDGE_MOTION_WAVE,    3, 450,  75},
    {"say hello",   BRIDGE_MOTION_WAVE,    3, 450,  75},
    {"raise your paw", BRIDGE_MOTION_WAVE, 2, 500,  70},
    {"give me your paw", BRIDGE_MOTION_WAVE, 2, 500, 70},

    // —— 其它 ——
    {"伸懒腰",   BRIDGE_MOTION_STRETCH,    1, 1600, 80},
    {"懒腰",     BRIDGE_MOTION_STRETCH,    1, 1600, 80},
    {"伸展",     BRIDGE_MOTION_STRETCH,    1, 1600, 70},
    {"点点头",   BRIDGE_MOTION_NOD_BODY,   2, 700,  70},
    {"点头",     BRIDGE_MOTION_NOD_BODY,   2, 700,  70},
    {"点个头",   BRIDGE_MOTION_NOD_BODY,   2, 700,  70},
    {"发抖",     BRIDGE_MOTION_SHIVER,     5, 280,  85},
    {"抖一抖",   BRIDGE_MOTION_SHIVER,     5, 280,  85},
    {"哆嗦",     BRIDGE_MOTION_SHIVER,     5, 280,  85},
    {"摇尾巴",   BRIDGE_MOTION_WAG_TAIL,   4, 320,  90},
    {"甩尾巴",   BRIDGE_MOTION_WAG_TAIL,   4, 320,  90},
    {"晃尾巴",   BRIDGE_MOTION_WAG_TAIL,   4, 320,  90},
    {"stretch",  BRIDGE_MOTION_STRETCH,    1, 1600, 80},
    {"nod",      BRIDGE_MOTION_NOD_BODY,   2, 700,  70},
    {"shiver",   BRIDGE_MOTION_SHIVER,     5, 280,  85},
    {"tremble",  BRIDGE_MOTION_SHIVER,     5, 280,  85},
    {"shake",    BRIDGE_MOTION_SHIVER,     5, 280,  85},
    {"wag your tail", BRIDGE_MOTION_WAG_TAIL, 4, 320, 90},
    {"wag",      BRIDGE_MOTION_WAG_TAIL,   4, 320,  90},

    // —— 兜底的泛化单词, 一定放最后 ——
    {"走",       BRIDGE_MOTION_WALK,       3, 700,  70},
    {"尾巴",     BRIDGE_MOTION_WAG_TAIL,   3, 350,  85},
    // 和中文的 "走" 同样是宽泛兜底: "let's go" 这类闲聊也会命中。中文那边
    // 已经接受了这个代价, 英文保持一致。真嫌吵就把这两条删掉。
    {"go",       BRIDGE_MOTION_WALK,       3, 700,  70},
    {"tail",     BRIDGE_MOTION_WAG_TAIL,   3, 350,  85},
};

// 关键词前面一小段里出现否定词就不触发 —— "别走了"/"don't sit" 不该动。
// 停止类规则本身就是否定式, 不走这个检查。
bool NegatedBefore(const std::string& t, size_t pos) {
    // 16 字节: 中文 3 个汉字 (9 字节) 够用, 英文要装下 "you don't have to "
    // 这种前缀, 所以取更宽的窗口。窗口太宽会误杀 ("I don't like carrots,
    // now dance"), 16 是折中。
    const size_t kLookBack = 16;
    size_t start = pos > kLookBack ? pos - kLookBack : 0;
    std::string prefix = t.substr(start, pos - start);

    static const char* kNegatives[] = {
        "不", "别", "没", "无需",
        "don't", "do not", "dont", "not ", "never", "no need", "stop ",
    };
    for (const char* n : kNegatives) {
        if (prefix.find(n) != std::string::npos) return true;
    }
    return false;
}

// "走两步" / "跳3下" -> 次数。没说次数返回 0 (用规则表的默认值)。
int ParseCount(const std::string& t) {
    static const struct { const char* w; int v; } kDigits[] = {
        {"两", 2}, {"一", 1}, {"二", 2}, {"三", 3}, {"四", 4}, {"五", 5},
        {"六", 6}, {"七", 7}, {"八", 8}, {"九", 9}, {"十", 10},
    };
    static const char* kUnits[] = {"下", "次", "步", "圈", "遍", "回"};

    for (const char* unit : kUnits) {
        size_t p = t.find(unit);
        if (p == std::string::npos || p == 0) continue;

        for (const auto& d : kDigits) {
            size_t dl = strlen(d.w);
            if (p >= dl && t.compare(p - dl, dl, d.w) == 0) return d.v;
        }
        // 阿拉伯数字, 最多两位 ("跳12下")
        if (isdigit(static_cast<unsigned char>(t[p - 1]))) {
            int v = t[p - 1] - '0';
            if (p >= 2 && isdigit(static_cast<unsigned char>(t[p - 2]))) {
                v += (t[p - 2] - '0') * 10;
            }
            return v;
        }
    }

    // 英文: "twice" / "two steps" / "jump 3 times"
    if (FindKeyword(t, "twice") != std::string::npos) return 2;
    if (FindKeyword(t, "once") != std::string::npos) return 1;

    static const struct { const char* w; int v; } kEnDigits[] = {
        {"one", 1}, {"two", 2}, {"three", 3}, {"four", 4}, {"five", 5},
        {"six", 6}, {"seven", 7}, {"eight", 8}, {"nine", 9}, {"ten", 10},
    };
    // "times" 排在 "time" 前只是为了读着顺 —— FindKeyword 认词边界,
    // "3 times" 不会被 "time" 命中。
    static const char* kEnUnits[] = {"times", "time", "steps", "step",
                                     "laps", "lap", "circles", "circle", "hops"};
    for (const char* unit : kEnUnits) {
        size_t p = FindKeyword(t, unit);
        if (p == std::string::npos || p == 0) continue;

        // 往前跳过空格, 取紧邻的那个词
        size_t e = p;
        while (e > 0 && isspace(static_cast<unsigned char>(t[e - 1]))) --e;
        size_t b = e;
        while (b > 0 && isalnum(static_cast<unsigned char>(t[b - 1]))) --b;
        if (b == e) continue;

        std::string word = t.substr(b, e - b);
        if (isdigit(static_cast<unsigned char>(word[0]))) return atoi(word.c_str());
        for (const auto& d : kEnDigits) {
            if (word == d.w) return d.v;
        }
    }
    return 0;
}

int Clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

}  // namespace

bool EyeMotion::OnUserText(const char* text) {
    if (text == nullptr || *text == '\0') return false;
    // 表里英文关键词一律小写, 先把输入的 A-Z 压平; 中文字节不受影响。
    std::string t = LowerAscii(text);

    for (const auto& rule : kIntentTable) {
        size_t pos = FindKeyword(t, rule.keyword);
        if (pos == std::string::npos) continue;

        if (rule.action == kIntentStop) {
            ESP_LOGI(TAG, "local intent '%s' -> stop", rule.keyword);
            Stop();
            return true;
        }
        if (NegatedBefore(t, pos)) continue;   // "别走了" —— 接着找下一条

        MotionRequest req;
        req.action    = rule.action;
        req.repeat    = rule.repeat;
        req.period_ms = rule.period_ms;
        req.amount    = rule.amount;
        // 用户直接下的命令, 和 MCP 一样抢占当前动作。
        req.preempt   = true;
        // 坐下/趴下是静态姿态, 走完不能弹回中位。
        req.home_end  = (rule.action != BRIDGE_MOTION_SIT &&
                         rule.action != BRIDGE_MOTION_LIE);

        // 方向: 走路认前后, 挥手认左右。
        if (rule.action == BRIDGE_MOTION_WALK) {
            bool backward = t.find("后") != std::string::npos ||
                            t.find("退") != std::string::npos ||
                            t.find("倒") != std::string::npos ||
                            FindKeyword(t, "back") != std::string::npos ||
                            FindKeyword(t, "backward") != std::string::npos ||
                            FindKeyword(t, "backwards") != std::string::npos ||
                            FindKeyword(t, "reverse") != std::string::npos;
            req.direction = backward ? -1 : 1;
        } else if (rule.action == BRIDGE_MOTION_WAVE ||
                   rule.action == BRIDGE_MOTION_TURN) {
            // 没提左右时默认右 ("转个圈" 这种)。"左转右转" 会认先出现的那个。
            bool left = t.find("左") != std::string::npos ||
                        FindKeyword(t, "left") != std::string::npos;
            req.direction = left ? -1 : 1;
        }

        // "走两步" / "跳三下"
        int count = ParseCount(t);
        if (count > 0) req.repeat = count;

        // 程度副词。HOME 没有幅度/次数可调, 跳过。
        if (rule.action != BRIDGE_MOTION_HOME) {
            bool faster = t.find("快") != std::string::npos ||
                          FindKeyword(t, "fast") != std::string::npos ||
                          FindKeyword(t, "faster") != std::string::npos ||
                          FindKeyword(t, "quick") != std::string::npos ||
                          FindKeyword(t, "quickly") != std::string::npos;
            bool slower = t.find("慢") != std::string::npos ||
                          FindKeyword(t, "slow") != std::string::npos ||
                          FindKeyword(t, "slower") != std::string::npos ||
                          FindKeyword(t, "slowly") != std::string::npos;
            if (faster) req.period_ms = req.period_ms * 3 / 5;
            if (slower) req.period_ms = req.period_ms * 8 / 5;

            if (t.find("用力") != std::string::npos || t.find("使劲") != std::string::npos ||
                t.find("大一点") != std::string::npos || t.find("大点") != std::string::npos ||
                FindKeyword(t, "hard") != std::string::npos ||
                FindKeyword(t, "harder") != std::string::npos ||
                FindKeyword(t, "big") != std::string::npos ||
                FindKeyword(t, "bigger") != std::string::npos) {
                req.amount = 100;
            }
            if (t.find("轻") != std::string::npos || t.find("小一点") != std::string::npos ||
                t.find("小点") != std::string::npos ||
                FindKeyword(t, "gently") != std::string::npos ||
                FindKeyword(t, "gentle") != std::string::npos ||
                FindKeyword(t, "softly") != std::string::npos ||
                FindKeyword(t, "little") != std::string::npos ||
                FindKeyword(t, "small") != std::string::npos) {
                req.amount = 40;
            }
        }

        req.repeat    = Clamp(req.repeat, 1, 50);
        req.period_ms = Clamp(req.period_ms, 200, 5000);
        req.amount    = Clamp(req.amount, 0, 100);

        ESP_LOGI(TAG, "local intent '%s' -> action %d (repeat=%d period=%d amount=%d dir=%d)",
                 rule.keyword, (int)req.action, req.repeat, req.period_ms,
                 req.amount, req.direction);

        if (!Play(req)) {
            ESP_LOGW(TAG, "local intent matched but motion link unavailable");
            return false;
        }
        MarkPlayedLocally(rule.action);
        return true;
    }
    return false;
}

void EyeMotion::MarkPlayedLocally(bridge_motion_action_t action) {
    std::lock_guard<std::mutex> lk(mutex_);
    local_action_    = action;
    local_action_us_ = esp_timer_get_time();
}

bool EyeMotion::RecentlyPlayedLocally(bridge_motion_action_t action) {
    // 3 秒: 服务端 LLM 的工具回调通常在 1-2 秒内到, 再久就该当成新的一次命令。
    const int64_t kWindowUs = 3 * 1000 * 1000;
    std::lock_guard<std::mutex> lk(mutex_);
    if (local_action_ != action) return false;
    int64_t dt = esp_timer_get_time() - local_action_us_;
    return dt >= 0 && dt < kWindowUs;
}

// ---------------- MCP 工具 ----------------

void EyeMotion::RegisterMcpTools() {
    auto& mcp = McpServer::GetInstance();

    // 通用的"播一个动作"闭包工厂, 省得每个工具都抄一遍参数解包。
    auto make_handler = [this](bridge_motion_action_t action, bool hold) {
        return [this, action, hold](const PropertyList& properties) -> ReturnValue {
            // 本地关键词刚播过同一个动作 —— 这是同一句话的回声 (用户说"坐下",
            // 本地已经坐了, 服务端 LLM 又调了一次工具), 跳过免得动两遍。
            if (RecentlyPlayedLocally(action)) return true;

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
                    if (RecentlyPlayedLocally(BRIDGE_MOTION_WALK)) return true;
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
                    if (RecentlyPlayedLocally(BRIDGE_MOTION_TURN)) return true;
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
                    if (RecentlyPlayedLocally(BRIDGE_MOTION_WAVE)) return true;
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
