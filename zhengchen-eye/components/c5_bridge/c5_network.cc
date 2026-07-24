/*
 * c5_network.cc - NetworkInterface (esp-ml307 3.x) 工厂实现
 */
#include "c5_network.h"

#include "c5_tcp.h"
#include "c5_udp.h"
#include "c5_mqtt.h"

#include <http_client.h>
#include <web_socket.h>

std::unique_ptr<Http> C5Network::CreateHttp(int connect_id) {
    // HttpClient 内部通过 NetworkInterface::CreateTcp/CreateSsl 建连,
    // 因此会自动落到 C5Tcp 上。
    return std::make_unique<HttpClient>(this, connect_id);
}

std::unique_ptr<Tcp> C5Network::CreateTcp(int connect_id) {
    return std::make_unique<C5Tcp>(bridge_, false);
}

std::unique_ptr<Tcp> C5Network::CreateSsl(int connect_id) {
    return std::make_unique<C5Tcp>(bridge_, true);
}

std::unique_ptr<Udp> C5Network::CreateUdp(int connect_id) {
    return std::make_unique<C5Udp>(bridge_);
}

std::unique_ptr<Mqtt> C5Network::CreateMqtt(int connect_id) {
    return std::make_unique<C5Mqtt>(bridge_);
}

std::unique_ptr<WebSocket> C5Network::CreateWebSocket(int connect_id) {
    // WebSocket 内部同样通过 CreateTcp/CreateSsl 建连 -> C5Tcp。
    return std::make_unique<WebSocket>(this, connect_id);
}
