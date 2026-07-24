/*
 * c5_tcp.h - 把 C5 网桥的一条 link 封装成 esp-ml307 3.x 的 Tcp 抽象
 *
 * 3.x 的 Tcp 是异步回调模型 (OnStream/OnDisconnected), 与旧版 2.x 的
 * 同步 Transport(Send/Receive) 不同。C5 网桥本身就是回调驱动的, 因此
 * 这里直接把 link 的 on_data 转成 OnStream 回调, on_closed 转成
 * OnDisconnected 回调。
 *
 * proto = TCP 或 TLS (由构造参数决定)。HttpClient / WebSocket 会通过
 * NetworkInterface::CreateTcp / CreateSsl 得到本类实例, 因此 HTTP 和
 * WebSocket 无需单独适配即可跑在 C5 网桥上。
 */
#ifndef C5_TCP_H
#define C5_TCP_H

#include <tcp.h>
#include <string>
#include <mutex>
#include <condition_variable>

#include "c5_bridge.h"

class C5Tcp : public Tcp {
public:
    C5Tcp(C5Bridge& bridge, bool tls);
    ~C5Tcp() override;

    bool Connect(const std::string& host, int port) override;
    void Disconnect() override;
    int  Send(const std::string& data) override;

    // 覆盖 OnStream: 设置回调时把连接建立与设置回调之间到达的数据补发出去,
    // 避免 WebSocket 在 Connect 之后才设置 OnStream 造成的早期数据丢失。
    void OnStream(std::function<void(const std::string& data)> callback) override;

private:
    C5Bridge& bridge_;
    bool tls_;
    int  link_id_ = -1;

    std::mutex mutex_;
    std::condition_variable cv_;
    bool open_done_ = false;
    bool open_ok_ = false;

    // 在 stream_callback_ 设置之前先缓存收到的数据
    std::string pending_rx_;
};

#endif // C5_TCP_H
