/*
 * c5_network.h - 把 C5 网桥封装成 esp-ml307 3.x 的 NetworkInterface 工厂
 *
 * 上层 (Ota/协议层) 通过 Board::GetNetwork() 拿到本类, 再调用
 * CreateHttp/CreateTcp/CreateSsl/CreateUdp/CreateMqtt/CreateWebSocket。
 *
 * - CreateTcp / CreateSsl -> C5Tcp (明文 / TLS)
 * - CreateUdp             -> C5Udp
 * - CreateMqtt            -> C5Mqtt
 * - CreateHttp            -> 复用组件自带 HttpClient (跑在 C5Tcp 之上)
 * - CreateWebSocket       -> 复用组件自带 WebSocket   (跑在 C5Tcp 之上)
 */
#ifndef C5_NETWORK_H
#define C5_NETWORK_H

#include <network_interface.h>

#include "c5_bridge.h"

class C5Network : public NetworkInterface {
public:
    explicit C5Network(C5Bridge& bridge) : bridge_(bridge) {}
    ~C5Network() override = default;

    std::unique_ptr<Http> CreateHttp(int connect_id = -1) override;
    std::unique_ptr<Tcp> CreateTcp(int connect_id = -1) override;
    std::unique_ptr<Tcp> CreateSsl(int connect_id = -1) override;
    std::unique_ptr<Udp> CreateUdp(int connect_id = -1) override;
    std::unique_ptr<Mqtt> CreateMqtt(int connect_id = -1) override;
    std::unique_ptr<WebSocket> CreateWebSocket(int connect_id = -1) override;

private:
    C5Bridge& bridge_;
};

#endif // C5_NETWORK_H
