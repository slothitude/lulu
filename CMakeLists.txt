cmake_minimum_required(VERSION 3.20)
project(lulu_v4 C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# ---- RyuGraph (Kuzu fork) ----
include(FetchContent)
FetchContent_Declare(kuzu
    GIT_REPOSITORY https://github.com/predictable-labs/ryugraph.git
    GIT_TAG        v25.9.2
)
FetchContent_MakeAvailable(kuzu)

# ---- Agent executable ----
set(SOURCES
    runtime/src/main.c
    runtime/src/agent_db.c
    runtime/src/tasks.c
    runtime/src/channel.c
    runtime/src/session.c
    runtime/src/llm.c
    runtime/src/tools.c
    runtime/src/sandbox.c
    runtime/src/state.c
    runtime/src/agent_config.c
    runtime/src/event_bus.c
    runtime/src/telegram.c
    runtime/src/cJSON.c
    runtime/src/decision_engine.c
    runtime/src/subscribers/log_subscriber.c
    runtime/src/subscribers/mem_subscriber.c
    runtime/src/subscribers/sdl3_debugger.c
    runtime/src/subscribers/tg_subscriber.c
)

add_executable(agent ${SOURCES})

target_include_directories(agent PRIVATE
    ${CMAKE_SOURCE_DIR}/runtime/src/include
    ${kuzu_SOURCE_DIR}/src/include/c_api
    ${kuzu_SOURCE_DIR}/src/include/main
)

# Link RyuGraph + system libs
target_link_libraries(agent PRIVATE
    ryu_shared
    winhttp
    ws2_32
    psapi
    iphlpapi
    bcrypt
    crypt32
    userenv
    ncrypt
    ssl
    crypto
    z
)

# TDLib static libs (if present)
if(EXISTS "${CMAKE_SOURCE_DIR}/libs")
    target_link_directories(agent PRIVATE ${CMAKE_SOURCE_DIR}/libs)
    target_link_libraries(agent PRIVATE
        tdjson_static tdjson_private tdclient tdcore
        tde2e tdmtproto tddb tdnet tdactor
        tdutils tdsqlite tdtl tdapi
    )
endif()

# SDL3 (if present)
target_link_libraries(agent PRIVATE SDL3 SDL3_image)

# Force C++ linker for TDLib
set_target_properties(agent PROPERTIES LINKER_LANGUAGE CXX)
