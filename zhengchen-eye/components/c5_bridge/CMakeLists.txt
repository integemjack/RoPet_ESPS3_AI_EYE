# c5_bridge - S3 侧驱动: 通过 UART 帧协议控制 C5 WiFi 网桥,
# 并把 C5 的 socket 能力桥接为 esp-ml307 3.x 的抽象接口
# (NetworkInterface / Tcp / Udp / Mqtt, 并复用其 HttpClient / WebSocket)。
#
# 另含配网门户 (c5_provision_portal): 用 S3 自带的 2.4G 射频开热点和网页,
# C5 只负责扫描和验证。这样配网期间手机不会掉线, 见该文件头部说明。
idf_component_register(
    SRCS
        "c5_bridge.cc"
        "c5_tcp.cc"
        "c5_udp.cc"
        "c5_mqtt.cc"
        "c5_network.cc"
        "c5_provision_portal.cc"
    INCLUDE_DIRS
        "include"
    # 小智原版配网页, 内容与 78__esp-wifi-connect/assets 下两个文件逐字节一致,
    # 仅改名以避开嵌入符号/构建目标重名 (那个组件在 WifiBoard 模式下仍会用到)。
    # 之前它在 C5 上跑不通不是页面的问题, 而是单射频导致热点掉线;
    # 热点挪到 S3 后前提消失, 原页面按其本意 (同步 /submit -> /done.html) 即可工作。
    EMBED_TXTFILES
        "assets/c5_portal.html"
        "assets/c5_portal_done.html"
    REQUIRES
        driver
        freertos
        esp_timer
        esp_wifi
        esp_netif
        esp_event
        esp_http_server
        nvs_flash
        json
        78__esp-ml307        # 复用其抽象类 (Tcp/Udp/Mqtt/WebSocket/Http/NetworkInterface)
        78__esp-wifi-connect # 复用 DnsServer (captive portal 用)
)
