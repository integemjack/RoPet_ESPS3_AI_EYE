#ifndef _WEBSOCKET_PROTOCOL_H_
#define _WEBSOCKET_PROTOCOL_H_


#include "protocol.h"
#include "ws_client.h"

#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>

#define WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT (1 << 0)

/* 心跳: 每 20s 一个 ping, 60s 内没有任何入向数据就认为链路已死。
 * 服务器侧常见的空闲断开是 30~60s 量级, ping 也顺带让中间设备不回收连接。 */
#define WEBSOCKET_PING_INTERVAL_SECONDS 20
#define WEBSOCKET_PING_TIMEOUT_SECONDS  60

/* 掉线自动重连: 退避 1s / 2s / 4s, 最多 3 次 */
#define WEBSOCKET_MAX_RECONNECT_ATTEMPTS 3

class WebsocketProtocol : public Protocol {
public:
    WebsocketProtocol();
    ~WebsocketProtocol();

    bool Start() override;
    bool SendAudio(std::unique_ptr<AudioStreamPacket> packet) override;
    bool OpenAudioChannel() override;
    void CloseAudioChannel() override;
    bool IsAudioChannelOpened() const override;

private:
    EventGroupHandle_t event_group_handle_;
    std::unique_ptr<WsClient> websocket_;
    int version_ = 1;

    std::string url_;
    std::string token_;

    /* CloseAudioChannel() 主动关闭时置位, 避免触发重连 */
    std::atomic<bool> closing_{false};
    std::atomic<bool> reconnecting_{false};

    void ParseServerHello(const cJSON* root);
    bool SendText(const std::string& text) override;
    std::string GetHelloMessage();

    bool EstablishConnection();
    void HandleDisconnected(WsCloseReason reason);
    void ScheduleReconnect();
};

#endif
