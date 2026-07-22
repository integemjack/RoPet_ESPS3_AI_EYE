/*
 * c5_transport.h - 把 C5 网桥的一条 link 封装成 ML307 组件的 Transport 抽象
 *                  (同步 Send/Receive 语义, WebSocket 内部会起线程调用 Receive)
 *                  proto = TCP 或 TLS。
 */
#ifndef C5_TRANSPORT_H
#define C5_TRANSPORT_H

#include <transport.h>
#include <string>
#include <mutex>
#include <condition_variable>

#include "c5_bridge.h"

class C5Transport : public Transport {
public:
    C5Transport(C5Bridge& bridge, bool tls);
    ~C5Transport() override;

    bool Connect(const char* host, int port) override;
    void Disconnect() override;
    int  Send(const char* data, size_t length) override;
    int  Receive(char* buffer, size_t bufferSize) override;

private:
    C5Bridge& bridge_;
    bool tls_;
    int  link_id_ = -1;

    std::mutex mutex_;
    std::condition_variable cv_;
    std::string rx_buffer_;      // 累积从 C5 收到的数据
    size_t rx_read_pos_ = 0;     // 已消费位置; 用游标代替 erase(0,n) 的 O(n) 搬移
    bool open_done_ = false;
    bool open_ok_ = false;
    bool closed_ = false;        // 远端关闭
};

#endif // C5_TRANSPORT_H
