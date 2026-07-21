/*
 * c5_http.cc - 精简 HTTP/1.1 客户端 (跑在 C5 网桥的 TCP/TLS 传输上)
 *
 * 采用同步模型: Open() 发送请求后同步接收响应头; ReadAll()/Read() 继续拉取响应体。
 * 传输为阻塞式 Transport::Receive。
 */
#include "c5_http.h"

#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <esp_log.h>

#define TAG "C5Http"

C5Http::C5Http(C5Bridge& bridge) : bridge_(bridge) {}

C5Http::~C5Http() {
    Close();
}

bool C5Http::ParseUrl(const std::string& url, bool& tls, std::string& host, int& port, std::string& path) {
    size_t p = url.find("://");
    if (p == std::string::npos) return false;
    std::string scheme = url.substr(0, p);
    tls = (scheme == "https");
    port = tls ? 443 : 80;
    size_t host_start = p + 3;
    size_t path_start = url.find('/', host_start);
    std::string hostport = (path_start == std::string::npos)
        ? url.substr(host_start)
        : url.substr(host_start, path_start - host_start);
    path = (path_start == std::string::npos) ? "/" : url.substr(path_start);

    size_t colon = hostport.find(':');
    if (colon != std::string::npos) {
        host = hostport.substr(0, colon);
        port = atoi(hostport.substr(colon + 1).c_str());
    } else {
        host = hostport;
    }
    return !host.empty();
}

int C5Http::FillMore(int timeout_ms) {
    // Transport::Receive 会阻塞直到有数据或连接关闭
    char buf[1024];
    int n = transport_->Receive(buf, sizeof(buf));
    if (n > 0) rx_buffer_.append(buf, n);
    return n;
}

bool C5Http::Open(const std::string& method, const std::string& url) {
    bool tls; std::string host, path; int port;
    if (!ParseUrl(url, tls, host, port, path)) {
        return false;
    }

    transport_ = std::make_unique<C5Transport>(bridge_, tls);
    if (!transport_->Connect(host.c_str(), port)) {
        return false;
    }

    // 组请求
    std::string req = method + " " + path + " HTTP/1.1\r\n";
    req += "Host: " + host + "\r\n";
    if (headers_.find("Content-Type") == headers_.end() && !content_.empty()) {
        headers_["Content-Type"] = "application/json";
    }
    if (!content_.empty()) {
        headers_["Content-Length"] = std::to_string(content_.size());
    }
    headers_["Connection"] = "close";
    for (auto& h : headers_) {
        req += h.first + ": " + h.second + "\r\n";
    }
    req += "\r\n";
    if (!content_.empty()) req += content_;

    if (transport_->Send(req.data(), req.size()) != (int)req.size()) {
        return false;
    }

    return RecvUntilHeaders();
}

bool C5Http::RecvUntilHeaders() {
    size_t hdr_end = std::string::npos;
    while (true) {
        hdr_end = rx_buffer_.find("\r\n\r\n");
        if (hdr_end != std::string::npos) break;
        if (FillMore(timeout_ms_) <= 0) return false;   // 连接关闭且头未收全
    }

    std::string header_block = rx_buffer_.substr(0, hdr_end);
    header_end_pos_ = hdr_end + 4;

    size_t line_end = header_block.find("\r\n");
    std::string status_line = header_block.substr(0, line_end);
    {
        size_t sp1 = status_line.find(' ');
        if (sp1 != std::string::npos) {
            status_code_ = atoi(status_line.c_str() + sp1 + 1);
        }
    }

    size_t pos = line_end + 2;
    while (pos < header_block.size()) {
        size_t eol = header_block.find("\r\n", pos);
        if (eol == std::string::npos) eol = header_block.size();
        std::string line = header_block.substr(pos, eol - pos);
        size_t colon = line.find(':');
        if (colon != std::string::npos) {
            std::string k = line.substr(0, colon);
            std::string v = line.substr(colon + 1);
            size_t vs = v.find_first_not_of(" \t");
            if (vs != std::string::npos) v = v.substr(vs);
            std::transform(k.begin(), k.end(), k.begin(), ::tolower);
            resp_headers_[k] = v;
        }
        pos = eol + 2;
    }

    auto it = resp_headers_.find("content-length");
    if (it != resp_headers_.end()) body_length_ = (size_t)atol(it->second.c_str());
    auto te = resp_headers_.find("transfer-encoding");
    if (te != resp_headers_.end() && te->second.find("chunked") != std::string::npos) {
        chunked_ = true;
    }
    return true;
}

std::string C5Http::GetResponseHeader(const std::string& key) const {
    std::string kl = key;
    std::transform(kl.begin(), kl.end(), kl.begin(), ::tolower);
    auto it = resp_headers_.find(kl);
    return it != resp_headers_.end() ? it->second : std::string();
}

std::string C5Http::ReadAll() {
    if (!chunked_ && body_length_ > 0) {
        while (rx_buffer_.size() < header_end_pos_ + body_length_) {
            if (FillMore(timeout_ms_) <= 0) break;
        }
        size_t avail = rx_buffer_.size() > header_end_pos_ ? rx_buffer_.size() - header_end_pos_ : 0;
        size_t n = std::min(avail, body_length_);
        return rx_buffer_.substr(header_end_pos_, n);
    }

    // chunked 或无 content-length: 读到连接关闭
    while (FillMore(timeout_ms_) > 0) { /* keep reading */ }
    std::string raw = rx_buffer_.substr(header_end_pos_);
    if (!chunked_) return raw;

    std::string out;
    size_t p = 0;
    while (p < raw.size()) {
        size_t eol = raw.find("\r\n", p);
        if (eol == std::string::npos) break;
        long chunk_len = strtol(raw.substr(p, eol - p).c_str(), nullptr, 16);
        p = eol + 2;
        if (chunk_len <= 0) break;
        if (p + chunk_len > raw.size()) break;
        out.append(raw, p, chunk_len);
        p += chunk_len + 2;
    }
    return out;
}

int C5Http::Read(char* buffer, size_t buffer_size) {
    size_t body_start = header_end_pos_ + read_off_;
    while (rx_buffer_.size() <= body_start) {
        if (FillMore(timeout_ms_) <= 0) break;
    }
    if (rx_buffer_.size() <= body_start) return 0;
    size_t avail = rx_buffer_.size() - body_start;
    size_t n = std::min(avail, buffer_size);
    memcpy(buffer, rx_buffer_.data() + body_start, n);
    read_off_ += n;
    return (int)n;
}

int C5Http::Write(const char* buffer, size_t buffer_size) {
    if (!transport_) return -1;
    return transport_->Send(buffer, buffer_size);
}

void C5Http::Close() {
    if (transport_) {
        transport_->Disconnect();
        transport_.reset();
    }
}
