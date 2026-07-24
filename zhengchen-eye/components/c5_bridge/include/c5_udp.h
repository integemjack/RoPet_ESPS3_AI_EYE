/*
 * c5_udp.h - 把 C5 网桥的一条 link 封装成 ML307 组件的 Udp 抽象
 */
#ifndef C5_UDP_H
#define C5_UDP_H

#include <udp.h>
#include <mutex>
#include <condition_variable>

#include "c5_bridge.h"

class C5Udp : public Udp {
public:
    explicit C5Udp(C5Bridge& bridge);
    ~C5Udp() override;

    bool Connect(const std::string& host, int port) override;
    void Disconnect() override;
    int  Send(const std::string& data) override;

private:
    C5Bridge& bridge_;
    int link_id_ = -1;

    std::mutex mutex_;
    std::condition_variable cv_;
    bool open_done_ = false;
    bool open_ok_ = false;
};

#endif // C5_UDP_H
