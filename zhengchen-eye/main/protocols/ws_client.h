/*
 * ws_client.h - WebSocket 客户端 (RFC 6455 子集)
 *
 * 为什么不用 managed_components/78__esp-ml307 里的 WebSocket:
 *
 *  1. 帧长度解析用有符号 char 做位运算, 长度低字节 >= 0x80 的帧 (16k opus 60ms
 *     帧长 150~200 字节, 命中概率约一半) 会算出一个巨大的 payload_length,
 *     于是 "数据不够" 分支永远成立 —— 接收缓冲再也不会前进, 连接静默变哑,
 *     没有任何日志。
 *  2. 分片/握手状态是函数内 static, 跨实例、跨重连共享。
 *  3. 断开时只说 "disconnected", 不区分 close 帧 / 远端 FIN / 心跳超时,
 *     排查掉线原因时拿不到任何信息。
 *  4. 没有心跳, Send 也没有加锁 (多任务并发发送会交错破坏帧边界)。
 *
 * managed_components/ 在 .gitignore 里, 改那边会被下一次 reconfigure 覆盖,
 * 所以在项目内重写一份。
 *
 * 线程模型: OnData / OnDisconnected 在底层传输的接收任务上被调用 (C5 网桥的
 * c5_rx 任务)。回调里不要销毁本对象, 也不要做长阻塞 —— 应该 Schedule 到主循环。
 */
#ifndef _WS_CLIENT_H_
#define _WS_CLIENT_H_

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>

#include <tcp.h>

class NetworkInterface;

enum WsCloseReason {
    kWsCloseLocal,          // 本端 Close() 或析构
    kWsClosePeer,           // 收到对端 close 帧
    kWsCloseTransport,      // 底层 TCP 断开 (FIN 或出错)
    kWsClosePingTimeout,    // 心跳窗口内没有任何入向数据
    kWsCloseProtocolError,  // 帧解析失败
};

const char* WsCloseReasonToString(WsCloseReason reason);

class WsClient {
public:
    WsClient(NetworkInterface* network, int connect_id);
    ~WsClient();

    void SetHeader(const char* key, const char* value);

    bool IsConnected() const { return connected_; }

    bool Connect(const char* uri);
    void Close();

    bool Send(const std::string& text);
    bool Send(const void* data, size_t len, bool binary = false, bool fin = true);
    bool Ping();

    /* 每 interval_s 发一个 ping; 连续 timeout_s 收不到任何入向数据 (含 pong)
     * 就判定链路已死并上报 kWsClosePingTimeout。Connect() 之前调用。
     * timeout_s <= 0 表示关闭心跳。 */
    void EnableHeartbeat(int interval_s, int timeout_s);

    void OnConnected(std::function<void()> cb) { on_connected_ = std::move(cb); }
    void OnDisconnected(std::function<void(WsCloseReason)> cb) { on_disconnected_ = std::move(cb); }
    void OnData(std::function<void(const char*, size_t, bool binary)> cb) { on_data_ = std::move(cb); }
    void OnError(std::function<void(int)> cb) { on_error_ = std::move(cb); }

private:
    static constexpr EventBits_t kHandshakeOk = BIT0;
    static constexpr EventBits_t kHandshakeFailed = BIT1;
    static constexpr EventBits_t kHeartbeatStop = BIT2;

    /* 单帧 payload 上限。超过就认为对端/链路已经错乱, 直接报协议错误而不是
     * 无限攒缓冲 (原实现在这里会悄悄卡死)。 */
    static constexpr size_t kMaxFramePayload = 64 * 1024;

    NetworkInterface* network_;
    int connect_id_;
    std::unique_ptr<Tcp> tcp_;

    EventGroupHandle_t events_ = nullptr;
    std::mutex send_mutex_;

    std::atomic<bool> connected_{false};
    std::atomic<bool> disconnect_reported_{true};
    bool handshake_completed_ = false;
    bool continuation_ = false;

    /* 接收侧状态: 只在传输接收任务上访问 */
    std::string receive_buffer_;
    std::string current_message_;
    bool message_fragmented_ = false;
    bool message_binary_ = false;

    /* 心跳 */
    int heartbeat_interval_s_ = 0;
    int heartbeat_timeout_s_ = 0;
    TaskHandle_t heartbeat_task_ = nullptr;
    std::atomic<int64_t> last_incoming_us_{0};

    std::map<std::string, std::string> headers_;
    std::function<void(const char*, size_t, bool binary)> on_data_;
    std::function<void(int)> on_error_;
    std::function<void()> on_connected_;
    std::function<void(WsCloseReason)> on_disconnected_;

    void OnTcpData(const std::string& data);
    bool ParseFrames();
    bool SendControlFrame(uint8_t opcode, const void* data, size_t len);
    bool SendFrameLocked(const void* data, size_t len, uint8_t first_byte);
    void ReportDisconnected(WsCloseReason reason);
    void StartHeartbeat();
    void StopHeartbeat();
    void HeartbeatTask();
};

#endif  // _WS_CLIENT_H_
