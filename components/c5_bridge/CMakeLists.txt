# c5_bridge - S3 侧驱动: 通过 UART 帧协议控制 C5 WiFi 网桥,
# 并把 C5 的 socket 能力桥接为 ML307 组件的抽象接口 (Tcp/Ssl/Udp/Mqtt/WebSocket/Http)。
idf_component_register(
    SRCS
        "c5_bridge.cc"
        "c5_transport.cc"
        "c5_udp.cc"
        "c5_mqtt.cc"
        "c5_http.cc"
    INCLUDE_DIRS
        "include"
    REQUIRES
        driver
        freertos
        esp_timer
        78__esp-ml307   # 复用其抽象类 (Tcp/Udp/Mqtt/WebSocket/Http/NetworkInterface)
)
