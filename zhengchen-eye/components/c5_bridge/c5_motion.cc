/*
 * c5_motion.cc - S3 侧动作下发实现
 */
#include "c5_motion.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <esp_log.h>

#define TAG "C5Motion"

static inline int clampi(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

bool C5Motion::Play(const MotionRequest& req) {
    if (req.action >= BRIDGE_MOTION_MAX) {
        ESP_LOGW(TAG, "invalid action %d", (int)req.action);
        return false;
    }

    bridge_motion_play_t p = {};
    p.seq       = ++seq_;
    p.action    = (uint8_t)req.action;
    p.repeat    = (uint8_t)clampi(req.repeat, 1, 50);
    p.period_ms = (uint16_t)clampi(req.period_ms, 200, 5000);
    p.amount    = (uint8_t)clampi(req.amount, 0, 100);
    p.direction = (int8_t)clampi(req.direction, -1, 1);
    p.flags     = (req.preempt  ? BRIDGE_MOTION_F_PREEMPT  : 0) |
                  (req.home_end ? BRIDGE_MOTION_F_HOME_END : 0);

    return bridge_.SendFrame(BRIDGE_CMD_MOTION_PLAY, BRIDGE_NO_LINK,
                             (const uint8_t*)&p, sizeof(p));
}

bool C5Motion::Play(bridge_motion_action_t action, int repeat, int amount,
                    int period_ms, bool preempt) {
    MotionRequest req;
    req.action    = action;
    req.repeat    = repeat;
    req.amount    = amount;
    req.period_ms = period_ms;
    req.preempt   = preempt;
    return Play(req);
}

bool C5Motion::Pose(const std::vector<bridge_servo_pose_t>& poses) {
    if (poses.empty() || poses.size() > BRIDGE_SERVO_MAX) return false;

    uint8_t buf[1 + BRIDGE_SERVO_MAX * sizeof(bridge_servo_pose_t)];
    buf[0] = (uint8_t)poses.size();
    memcpy(buf + 1, poses.data(), poses.size() * sizeof(bridge_servo_pose_t));

    return bridge_.SendFrame(BRIDGE_CMD_MOTION_POSE, BRIDGE_NO_LINK, buf,
                             (uint16_t)(1 + poses.size() * sizeof(bridge_servo_pose_t)));
}

void C5Motion::Stop(bool clear_queue, bool go_home, bool detach) {
    uint8_t flags = (clear_queue ? BRIDGE_MOTION_STOP_CLEAR  : 0) |
                    (go_home     ? BRIDGE_MOTION_STOP_HOME   : 0) |
                    (detach      ? BRIDGE_MOTION_STOP_DETACH : 0);
    bridge_.SendFrame(BRIDGE_CMD_MOTION_STOP, BRIDGE_NO_LINK, &flags, 1);
}

bool C5Motion::SetTrim(int ch, float degrees) {
    if (ch < 0 || ch >= BRIDGE_SERVO_COUNT) return false;

    uint8_t buf[1 + sizeof(bridge_servo_trim_t)];
    buf[0] = 1;
    bridge_servo_trim_t item = {};
    item.ch = (uint8_t)ch;
    item.trim_x10 = (int16_t)lroundf(degrees * 10.0f);
    memcpy(buf + 1, &item, sizeof(item));

    return bridge_.SendFrame(BRIDGE_CMD_MOTION_TRIM, BRIDGE_NO_LINK, buf, sizeof(buf));
}

void C5Motion::RequestState() {
    bridge_.SendFrame(BRIDGE_CMD_MOTION_QUERY, BRIDGE_NO_LINK, nullptr, 0);
}

void C5Motion::OnDone(std::function<void(uint16_t, uint8_t, uint8_t)> cb) {
    std::lock_guard<std::mutex> lk(cb_mutex_);
    on_done_ = std::move(cb);
}

void C5Motion::OnState(std::function<void(const C5MotionState&)> cb) {
    std::lock_guard<std::mutex> lk(cb_mutex_);
    on_state_ = std::move(cb);
}

void C5Motion::HandleDoneFrame(const uint8_t* payload, uint16_t len) {
    if (len < sizeof(bridge_motion_done_t)) return;
    bridge_motion_done_t d;
    memcpy(&d, payload, sizeof(d));

    std::function<void(uint16_t, uint8_t, uint8_t)> cb;
    {
        std::lock_guard<std::mutex> lk(cb_mutex_);
        cb = on_done_;
    }
    if (cb) cb(d.seq, d.action, d.result);
}

void C5Motion::HandleStateFrame(const uint8_t* payload, uint16_t len) {
    if (len < sizeof(bridge_motion_state_t)) return;
    bridge_motion_state_t s;
    memcpy(&s, payload, sizeof(s));

    C5MotionState st;
    st.busy     = s.busy != 0;
    st.queued   = s.queued;
    st.attached = s.attached != 0;
    st.count    = std::min<uint8_t>(s.count, BRIDGE_SERVO_MAX);
    memcpy(st.angle_x10, s.angle_x10, sizeof(st.angle_x10));

    std::function<void(const C5MotionState&)> cb;
    {
        std::lock_guard<std::mutex> lk(cb_mutex_);
        cb = on_state_;
    }
    if (cb) cb(st);
}
