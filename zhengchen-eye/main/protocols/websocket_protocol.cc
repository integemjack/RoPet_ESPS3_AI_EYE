#include "websocket_protocol.h"
#include "board.h"
#include "system_info.h"
#include "application.h"
#include "settings.h"

#include <cstring>
#include <cJSON.h>
#include <esp_log.h>
#include <arpa/inet.h>
#include <freertos/task.h>
#include "assets/lang_config.h"

#define TAG "WS"

WebsocketProtocol::WebsocketProtocol() {
    event_group_handle_ = xEventGroupCreate();
}

WebsocketProtocol::~WebsocketProtocol() {
    vEventGroupDelete(event_group_handle_);
}

bool WebsocketProtocol::Start() {
    // Only connect to server when audio channel is needed
    return true;
}

bool WebsocketProtocol::SendAudio(std::unique_ptr<AudioStreamPacket> packet) {
    if (websocket_ == nullptr || !websocket_->IsConnected()) {
        return false;
    }

    if (version_ == 2) {
        std::string serialized;
        serialized.resize(sizeof(BinaryProtocol2) + packet->payload.size());
        auto bp2 = (BinaryProtocol2*)serialized.data();
        bp2->version = htons(version_);
        bp2->type = 0;
        bp2->reserved = 0;
        bp2->timestamp = htonl(packet->timestamp);
        bp2->payload_size = htonl(packet->payload.size());
        memcpy(bp2->payload, packet->payload.data(), packet->payload.size());

        return websocket_->Send(serialized.data(), serialized.size(), true);
    } else if (version_ == 3) {
        std::string serialized;
        serialized.resize(sizeof(BinaryProtocol3) + packet->payload.size());
        auto bp3 = (BinaryProtocol3*)serialized.data();
        bp3->type = 0;
        bp3->reserved = 0;
        bp3->payload_size = htons(packet->payload.size());
        memcpy(bp3->payload, packet->payload.data(), packet->payload.size());

        return websocket_->Send(serialized.data(), serialized.size(), true);
    } else {
        return websocket_->Send(packet->payload.data(), packet->payload.size(), true);
    }
}

bool WebsocketProtocol::SendText(const std::string& text) {
    if (websocket_ == nullptr || !websocket_->IsConnected()) {
        return false;
    }

    if (!websocket_->Send(text)) {
        ESP_LOGE(TAG, "Failed to send text: %s", text.c_str());
        SetError(Lang::Strings::SERVER_ERROR);
        return false;
    }

    return true;
}

bool WebsocketProtocol::IsAudioChannelOpened() const {
    return websocket_ != nullptr && websocket_->IsConnected() && !error_occurred_ && !IsTimeout();
}

void WebsocketProtocol::CloseAudioChannel() {
    closing_ = true;
    if (websocket_ != nullptr) {
        websocket_->Close();
    }
    websocket_.reset();
    closing_ = false;
}

bool WebsocketProtocol::OpenAudioChannel() {
    Settings settings("websocket", false);
    url_ = settings.GetString("url");
    token_ = settings.GetString("token");
    int version = settings.GetInt("version");
    if (version != 0) {
        version_ = version;
    }

    error_occurred_ = false;

    if (!EstablishConnection()) {
        return false;
    }

    if (on_audio_channel_opened_ != nullptr) {
        on_audio_channel_opened_();
    }

    return true;
}

/* 建立一条连接并完成 hello 交换。重连时复用, 所以不碰 on_audio_channel_opened_ /
 * on_audio_channel_closed_ —— 那两个是"会话级"事件, 由调用方决定何时触发。 */
bool WebsocketProtocol::EstablishConnection() {
    /* 直接构造 WsClient 而不走 NetworkInterface::CreateWebSocket(): 后者返回的是
     * esp-ml307 自带的 WebSocket, 帧长度解析有符号位 bug 且无心跳。WsClient 只
     * 需要 NetworkInterface 的 CreateTcp/CreateSsl。 */
    auto network = Board::GetInstance().GetNetwork();
    websocket_ = std::make_unique<WsClient>(network, 1);

    std::string token = token_;
    if (!token.empty()) {
        // If token not has a space, add "Bearer " prefix
        if (token.find(" ") == std::string::npos) {
            token = "Bearer " + token;
        }
        websocket_->SetHeader("Authorization", token.c_str());
    }
    websocket_->SetHeader("Protocol-Version", std::to_string(version_).c_str());
    websocket_->SetHeader("Device-Id", SystemInfo::GetMacAddress().c_str());
    websocket_->SetHeader("Client-Id", Board::GetInstance().GetUuid().c_str());

    websocket_->EnableHeartbeat(WEBSOCKET_PING_INTERVAL_SECONDS, WEBSOCKET_PING_TIMEOUT_SECONDS);

    websocket_->OnData([this](const char* data, size_t len, bool binary) {
        if (binary) {
            if (on_incoming_audio_ != nullptr) {
                if (version_ == 2) {
                    BinaryProtocol2* bp2 = (BinaryProtocol2*)data;
                    bp2->version = ntohs(bp2->version);
                    bp2->type = ntohs(bp2->type);
                    bp2->timestamp = ntohl(bp2->timestamp);
                    bp2->payload_size = ntohl(bp2->payload_size);
                    auto payload = (uint8_t*)bp2->payload;
                    on_incoming_audio_(std::make_unique<AudioStreamPacket>(AudioStreamPacket{
                        .sample_rate = server_sample_rate_,
                        .frame_duration = server_frame_duration_,
                        .timestamp = bp2->timestamp,
                        .payload = std::vector<uint8_t>(payload, payload + bp2->payload_size)
                    }));
                } else if (version_ == 3) {
                    BinaryProtocol3* bp3 = (BinaryProtocol3*)data;
                    bp3->type = bp3->type;
                    bp3->payload_size = ntohs(bp3->payload_size);
                    auto payload = (uint8_t*)bp3->payload;
                    on_incoming_audio_(std::make_unique<AudioStreamPacket>(AudioStreamPacket{
                        .sample_rate = server_sample_rate_,
                        .frame_duration = server_frame_duration_,
                        .timestamp = 0,
                        .payload = std::vector<uint8_t>(payload, payload + bp3->payload_size)
                    }));
                } else {
                    on_incoming_audio_(std::make_unique<AudioStreamPacket>(AudioStreamPacket{
                        .sample_rate = server_sample_rate_,
                        .frame_duration = server_frame_duration_,
                        .timestamp = 0,
                        .payload = std::vector<uint8_t>((uint8_t*)data, (uint8_t*)data + len)
                    }));
                }
            }
        } else {
            // Parse JSON data
            auto root = cJSON_Parse(data);
            auto type = cJSON_GetObjectItem(root, "type");
            if (cJSON_IsString(type)) {
                if (strcmp(type->valuestring, "hello") == 0) {
                    ParseServerHello(root);
                } else {
                    if (on_incoming_json_ != nullptr) {
                        on_incoming_json_(root);
                    }
                }
            } else {
                ESP_LOGE(TAG, "Missing message type, data: %s", data);
            }
            cJSON_Delete(root);
        }
        last_incoming_time_ = std::chrono::steady_clock::now();
    });

    websocket_->OnDisconnected([this](WsCloseReason reason) {
        HandleDisconnected(reason);
    });

    ESP_LOGI(TAG, "Connecting to websocket server: %s with version: %d", url_.c_str(), version_);
    if (!websocket_->Connect(url_.c_str())) {
        ESP_LOGE(TAG, "Failed to connect to websocket server");
        SetError(Lang::Strings::SERVER_NOT_CONNECTED);
        return false;
    }

    xEventGroupClearBits(event_group_handle_, WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT);

    // Send hello message to describe the client
    auto message = GetHelloMessage();
    if (!SendText(message)) {
        return false;
    }

    // Wait for server hello
    EventBits_t bits = xEventGroupWaitBits(event_group_handle_, WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT, pdTRUE, pdFALSE, pdMS_TO_TICKS(10000));
    if (!(bits & WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT)) {
        ESP_LOGE(TAG, "Failed to receive server hello");
        SetError(Lang::Strings::SERVER_TIMEOUT);
        return false;
    }

    last_incoming_time_ = std::chrono::steady_clock::now();
    return true;
}

/* 在 WsClient 的接收/心跳任务上被调用。这里不能销毁 websocket_ (那正是调用者
 * 自己), 所以重连丢给一个一次性任务做。 */
void WebsocketProtocol::HandleDisconnected(WsCloseReason reason) {
    ESP_LOGW(TAG, "Websocket disconnected: %s", WsCloseReasonToString(reason));

    if (closing_) {
        return;  // 本端主动关闭, 调用方自己会处理状态
    }

    /* 对端明确发 close 帧 = 服务器有意结束会话, 重连它大概率还会再关一次,
     * 所以只对"异常"断开做重连。 */
    if (reason == kWsClosePeer || reason == kWsCloseLocal) {
        if (on_audio_channel_closed_ != nullptr) {
            on_audio_channel_closed_();
        }
        return;
    }

    ScheduleReconnect();
}

void WebsocketProtocol::ScheduleReconnect() {
    if (reconnecting_.exchange(true)) {
        return;  // 已经在重连了
    }

    auto task = new std::function<void()>([this]() {
        bool ok = false;
        for (int attempt = 0; attempt < WEBSOCKET_MAX_RECONNECT_ATTEMPTS; attempt++) {
            int delay_ms = 1000 << attempt;   // 1s / 2s / 4s
            vTaskDelay(pdMS_TO_TICKS(delay_ms));

            if (closing_) {
                break;
            }
            ESP_LOGI(TAG, "Reconnecting websocket, attempt %d/%d",
                     attempt + 1, WEBSOCKET_MAX_RECONNECT_ATTEMPTS);

            /* 旧实例已经断开, 这里换新的。上一条连接的接收任务此刻不会再回调,
             * 因为 WsClient 析构会先 Disconnect() 摘掉网桥 link。 */
            websocket_.reset();
            error_occurred_ = false;

            if (EstablishConnection()) {
                ok = true;
                break;
            }
        }

        reconnecting_ = false;

        if (!ok) {
            ESP_LOGE(TAG, "Websocket reconnect failed, closing audio channel");
            websocket_.reset();
            if (on_audio_channel_closed_ != nullptr) {
                on_audio_channel_closed_();
            }
        } else {
            ESP_LOGI(TAG, "Websocket reconnected, session=%s", session_id_.c_str());
        }
    });

    if (xTaskCreate([](void* arg) {
            auto fn = (std::function<void()>*)arg;
            (*fn)();
            delete fn;
            vTaskDelete(NULL);
        }, "ws_reconnect", 4096, task, 4, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create reconnect task");
        delete task;
        reconnecting_ = false;
        if (on_audio_channel_closed_ != nullptr) {
            on_audio_channel_closed_();
        }
    }
}

std::string WebsocketProtocol::GetHelloMessage() {
    // keys: message type, version, audio_params (format, sample_rate, channels)
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "hello");
    cJSON_AddNumberToObject(root, "version", version_);
    cJSON* features = cJSON_CreateObject();
#if CONFIG_USE_SERVER_AEC
    cJSON_AddBoolToObject(features, "aec", true);
#endif
    cJSON_AddBoolToObject(features, "mcp", true);
    cJSON_AddItemToObject(root, "features", features);
    cJSON_AddStringToObject(root, "transport", "websocket");
    cJSON* audio_params = cJSON_CreateObject();
    cJSON_AddStringToObject(audio_params, "format", "opus");
    cJSON_AddNumberToObject(audio_params, "sample_rate", 16000);
    cJSON_AddNumberToObject(audio_params, "channels", 1);
    cJSON_AddNumberToObject(audio_params, "frame_duration", OPUS_FRAME_DURATION_MS);
    cJSON_AddItemToObject(root, "audio_params", audio_params);
    auto json_str = cJSON_PrintUnformatted(root);
    std::string message(json_str);
    cJSON_free(json_str);
    cJSON_Delete(root);
    return message;
}

void WebsocketProtocol::ParseServerHello(const cJSON* root) {
    auto transport = cJSON_GetObjectItem(root, "transport");
    if (transport == nullptr || strcmp(transport->valuestring, "websocket") != 0) {
        ESP_LOGE(TAG, "Unsupported transport: %s", transport->valuestring);
        return;
    }

    auto session_id = cJSON_GetObjectItem(root, "session_id");
    if (cJSON_IsString(session_id)) {
        session_id_ = session_id->valuestring;
        ESP_LOGI(TAG, "Session ID: %s", session_id_.c_str());
    }

    auto audio_params = cJSON_GetObjectItem(root, "audio_params");
    if (cJSON_IsObject(audio_params)) {
        auto sample_rate = cJSON_GetObjectItem(audio_params, "sample_rate");
        if (cJSON_IsNumber(sample_rate)) {
            server_sample_rate_ = sample_rate->valueint;
        }
        auto frame_duration = cJSON_GetObjectItem(audio_params, "frame_duration");
        if (cJSON_IsNumber(frame_duration)) {
            server_frame_duration_ = frame_duration->valueint;
        }
    }

    xEventGroupSetBits(event_group_handle_, WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT);
}
