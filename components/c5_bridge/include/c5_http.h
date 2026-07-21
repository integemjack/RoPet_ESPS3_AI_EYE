/*
 * c5_http.h - 在 C5 网桥的 TCP/TLS 传输上实现精简 HTTP/1.1 客户端
 *             实现 ML307 组件的 Http 抽象 (2.1.6), 供 OTA / 配置拉取使用。
 */
#ifndef C5_HTTP_H
#define C5_HTTP_H

#include <http.h>
#include <map>
#include <string>
#include <memory>

#include "c5_bridge.h"
#include "c5_transport.h"

class C5Http : public Http {
public:
    explicit C5Http(C5Bridge& bridge);
    ~C5Http() override;

    void SetTimeout(int timeout_ms) override { timeout_ms_ = timeout_ms; }
    void SetHeader(const std::string& key, const std::string& value) override { headers_[key] = value; }
    void SetContent(std::string&& content) override { content_ = std::move(content); }
    bool Open(const std::string& method, const std::string& url) override;
    void Close() override;
    int  Read(char* buffer, size_t buffer_size) override;
    int  Write(const char* buffer, size_t buffer_size) override;
    int  GetStatusCode() override { return status_code_; }
    std::string GetResponseHeader(const std::string& key) const override;
    size_t GetBodyLength() override { return body_length_; }
    std::string ReadAll() override;

private:
    bool ParseUrl(const std::string& url, bool& tls, std::string& host, int& port, std::string& path);
    bool RecvUntilHeaders();
    int  FillMore(int timeout_ms);   // 从传输读一段追加到 rx_buffer_

    C5Bridge& bridge_;
    std::unique_ptr<C5Transport> transport_;
    std::map<std::string, std::string> headers_;
    std::map<std::string, std::string> resp_headers_;
    std::string content_;
    std::string rx_buffer_;
    size_t read_off_ = 0;      // ReadAll/Read 已消费的 body 偏移

    int  timeout_ms_ = 10000;
    int  status_code_ = 0;
    size_t body_length_ = 0;
    size_t header_end_pos_ = 0;
    bool chunked_ = false;
};

#endif // C5_HTTP_H
