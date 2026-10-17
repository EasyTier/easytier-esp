idf_component_register(
    SRCS
        "src/support/random.cpp"
        "src/crypto/siphash.cpp"
        "src/crypto/kdf.cpp"
        "src/crypto/aes_gcm_psa.cpp"
        "src/protocol/wire.cpp"
        "src/protocol/protobuf.cpp"
        "src/protocol/messages.cpp"
        "src/transport/tcp.cpp"
        "src/peer/handshake.cpp"
        "src/peer/keepalive.cpp"
        "src/rpc/fragment.cpp"
        "src/rpc/rpc.cpp"
        "src/route/sync_messages.cpp"
        "src/route/route_info.cpp"
        "src/route/topology.cpp"
        "src/route/table.cpp"
        "src/route/dhcp.cpp"
        "src/route/sync_engine.cpp"
        "src/data/plane.cpp"
        "src/netif/virtual_netif.cpp"
        "src/tasks/data_task.cpp"
        "src/config/config.cpp"
        "src/config/toml.cpp"
        "src/tasks/io_task.cpp"
        "src/tasks/proto_task.cpp"
        "src/context.cpp"
        "src/inst.cpp"
        "third_party/tomlc17/tomlc17.c"
    INCLUDE_DIRS
        "include"
    PRIV_INCLUDE_DIRS
        "src"
        "third_party/tomlc17"
    REQUIRES
        esp_common
    PRIV_REQUIRES
        mbedtls
        esp_hw_support
        esp_timer
        esp_netif
        freertos
        lwip
)

# The public header uses C++20 types; fail loudly instead of compiling with an
# older standard.
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_20)

# Exceptions/RTTI are disabled for the component's own C++ sources only (the
# vendored C parser must not receive C++-only flags).
target_compile_options(${COMPONENT_LIB} PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
    $<$<COMPILE_LANGUAGE:CXX>:-Wall>
    $<$<COMPILE_LANGUAGE:CXX>:-Wextra>
)

# 可选：为组件源码生成 GCC 的 -fstack-usage 数据（每个函数一帧大小），
# 供 tools/check_stack_usage.py 做任务栈预算的静态校验：
#   idf.py -DEASYTIER_STACK_USAGE=ON build && python3 tools/check_stack_usage.py
option(EASYTIER_STACK_USAGE "Emit -fstack-usage data for stack budget analysis" OFF)
if(EASYTIER_STACK_USAGE)
    target_compile_options(${COMPONENT_LIB} PRIVATE
        $<$<COMPILE_LANGUAGE:CXX>:-fstack-usage>
    )
endif()

set_source_files_properties(third_party/tomlc17/tomlc17.c PROPERTIES COMPILE_OPTIONS "-w")
