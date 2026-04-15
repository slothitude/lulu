cmake_minimum_required(VERSION 3.10)
project(BashAgent C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_executable(agent
    src/main.c
    src/sandbox.c
    src/tools.c
    src/llm.c
    src/state.c
    src/agent_config.c
    src/agent_core.c
    src/planner.c
    src/actor.c
    src/critic.c
    src/cJSON.c
)

target_include_directories(agent PRIVATE src/include)
target_link_libraries(agent winhttp)

if(WIN32)
    target_compile_definitions(agent PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
