# lulu

Configurable agent runtime with DLL-loaded tools and pipeline-driven execution.

## Architecture

```
agent.json          config (roles, pipeline, tool toggles, behavior)
config.json         LLM credentials (not committed)
run.bat             build + run entrypoint
runtime/
  src/              host source (C11)
    main.c            pipeline executor / orchestrator
    agent_core.c      shared LLM engine (prompt build + call + parse)
    agent_config.c    JSON config loader
    planner.c         goal → steps[] via LLM
    actor.c           step → tool call via LLM → execute
    critic.c          log review → progress evaluation via LLM
    tools.c           DLL tool loader (LoadLibrary registry)
    llm.c             OpenAI-compatible HTTP client (WinHTTP)
    sandbox.c         path traversal protection
    state.c           memory, logging, file I/O
    cJSON.c           vendored JSON parser
    include/          headers
    tools/            tool DLL sources
tools/              built DLLs (created by run.bat)
workspace/          sandboxed file operations root
state/
  info.md           goal description
  log.txt           execution log
  memory.json       persistent working memory
```

## Main Loop

```
for each iteration:
  PLANNER  → break goal into steps[]       (if enabled + needed)
  ACTOR    → execute each step via tool     (if enabled)
  CRITIC   → evaluate progress, detect done (if enabled)
```

All three stages are gated by the `pipeline` config. Stuck detection and stagnation tracking prevent infinite loops.

## Pipeline Configuration

```json
"pipeline": [
  { "role": "planner", "enabled": true },
  { "role": "actor",   "enabled": true },
  { "role": "critic",  "enabled": true }
]
```

Disable stages by setting `enabled: false`:

```json
"pipeline": [
  { "role": "planner", "enabled": true },
  { "role": "actor",   "enabled": true }
]
```

## Tool System

Tools are loaded as DLLs from the `tools/` directory at runtime. No recompilation needed to add or remove tools.

### Tool ABI

Each DLL exports two functions:

```c
// tool_api.h
const ToolInfo *tool_get_info(void);       // name + description
tool_execute_fn tool_get_execute(void);     // cJSON *(*fn)(cJSON *args, const char *workspace, char **err)
```

### Built-in tools

| DLL | Tool | Purpose |
|-----|------|---------|
| `create_file.dll` | `create_file(path, content)` | Write file to workspace |
| `read_file.dll` | `read_file(path)` | Read file from workspace |
| `list_files.dll` | `list_files()` | List workspace contents |
| `run_test.dll` | `run_test()` | Parse test_results.txt |
| `none.dll` | `none(reason)` | Skip step |

### Tool toggles

```json
"tools": {
  "create_file": true,
  "read_file": true,
  "list_files": false,
  "run_test": false,
  "none": true
}
```

### Adding a new tool

1. Create `runtime/tools/my_tool.c`:

```c
#define _CRT_SECURE_NO_WARNINGS
#define TOOL_BUILDING_DLL
#include "tool_api.h"

static cJSON *my_tool(cJSON *args, const char *workspace, char **error) {
    // ... implementation
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "status", "ok");
    return r;
}

static ToolInfo info = { "my_tool", "Does something useful" };

TOOL_EXPORT const ToolInfo *tool_get_info(void) { return &info; }
TOOL_EXPORT tool_execute_fn tool_get_execute(void) { return my_tool; }
```

2. Build:

```bash
gcc -shared -std=c11 -D_CRT_SECURE_NO_WARNINGS -DTOOL_BUILDING_DLL \
    -I runtime/src/include \
    runtime/tools/my_tool.c runtime/src/cJSON.c \
    -o tools/my_tool.dll
```

3. Add to `agent.json` shared.tools_list and tools toggle.

## Build & Run

Requirements: [MSYS2](https://www.msys2.org/) with MinGW64 GCC.

```bash
# From bashagent/

# Create config.json with your LLM credentials
echo '{
  "provider": "openai",
  "model": "glm-5.1",
  "endpoint": "https://your-llm-endpoint/chat/completions",
  "apikey": "your-key",
  "max_iterations": 10,
  "http_timeout_ms": 30000
}' > config.json

# Write your goal
echo "# Goal\nDo something useful." > state/info.md

# Build and run
run.bat
```

`run.bat` compiles all tool DLLs, builds the host, then runs `agent.exe`.

## Security

- All file operations sandboxed to `workspace/`
- Path traversal (`..`, absolute paths, system dirs) blocked
- No arbitrary command execution
- Tool DLLs have no LLM access
- `config.json` excluded from git (contains API key)

## License

MIT
