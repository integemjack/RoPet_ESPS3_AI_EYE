# [本地修改] 配网热点已搬到 S3 的 2.4G 射频上 (C5 单射频, 自己开热点会在 STA
# 验证时被拖去换信道, 手机掉线导致配网页的 JS 被系统冻结、拿不到验证结果)。
# C5 这边只保留 "存凭据 + 连 WiFi" 两块, 不再需要 SoftAP / captive portal /
# DNS server / 网页资源 —— 一并从构建里摘掉, 省下 flash 和内存。
idf_component_register(
    SRCS
        "wifi_station.cc"
        "ssid_manager.cc"
    INCLUDE_DIRS
        "include"
    REQUIRES
        "esp_timer"
        "esp_wifi"
        "nvs_flash"
)
