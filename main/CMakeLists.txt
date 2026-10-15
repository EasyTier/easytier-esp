idf_component_register(
    SRCS
        "main.cpp"
    INCLUDE_DIRS
        "."
        "include"
    REQUIRES
        easytier-esp
        esp_wifi
        esp_event
        esp_netif
        esp_timer
        nvs_flash
    # 把示例配置嵌进固件，示例无需挂载文件系统即可读取：
    #   符号 _binary_easytier_toml_start / _binary_easytier_toml_end
    EMBED_TXTFILES
        "easytier.toml"
)
