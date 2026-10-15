# 把示例配置嵌进固件，示例无需挂载文件系统即可读取：
#   符号 _binary_easytier_toml_start / _binary_easytier_toml_end
#
# main/easytier.toml 含真实的入口地址与网络密钥，已被 .gitignore 忽略，新克隆的
# 仓库里不存在；因此只在文件存在时才嵌入，main.cpp 据 EASYTIER_EXAMPLE_TOML_EMBEDDED
# 决定是启动组网还是只连 WiFi。
set(easytier_embed_args "")
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/easytier.toml")
    set(easytier_embed_args EMBED_TXTFILES "easytier.toml")
endif()

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
    ${easytier_embed_args}
)

if(easytier_embed_args)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE EASYTIER_EXAMPLE_TOML_EMBEDDED=1)
endif()
