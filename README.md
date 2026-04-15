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

## Prompt Cache

LLM responses are cached in-memory using FNV-1a 64-bit hash deduplication. Identical prompts skip the HTTP call entirely. Cache persists across runs as a JSON file.

```json
"behavior": {
  "enable_prompt_cache": true,
  "cache_path": "state/prompt_cache.json"
}
```

Features:
- **Zero-dependency FNV-1a hash** — fast, no collisions for typical prompts
- **256-entry in-memory table** — more than enough per session
- **JSON file persistence** — survives restarts, loads on init, saves on exit
- **Observability** — every LLM log entry includes `prompt_hash` and `cache_hit` fields
- **Loop detection** — `--replay` shows hashes; duplicate hashes = agent spinning

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
| `sdl3_render.dll` | `sdl3_render(action, path, ...)` | Render shapes/text to images (SDL3) |
| `none.dll` | `none(reason)` | Skip step |

### SDL3 Render Tool

Renders shapes and text to PNG/BMP images or opens a live window. Built on SDL3 + SDL3_image.

**Actions:**
- `render` (default) — draw to offscreen surface, save to file in workspace
- `window` — open a visible SDL3 window for a timed display
- `info` — return SDL3 version string

**Arguments:**
- `path` — output filename (e.g. `"output.png"` or `"output.bmp"`)
- `action` — `render`, `window`, or `info`
- `width`, `height` — canvas size (default 640x480)
- `bg_color` — background color as hex `#RRGGBB` or named color
- `items` / `shapes` / `elements` — array of draw commands (forgiving aliases)

**Draw commands:**
```json
{"type": "rect",    "x": 10, "y": 10, "w": 100, "h": 50, "color": "red", "fill": true}
{"type": "circle",  "x": 200, "y": 200, "r": 50, "color": "#00FF00", "fill": true}
{"type": "line",    "x1": 0, "y1": 0, "x2": 300, "y2": 300, "color": "white"}
{"type": "point",   "x": 150, "y": 150, "color": "yellow"}
{"type": "text",    "x": 50, "y": 100, "text": "Hello SDL3", "color": "cyan"}
```

**Named colors:** red, green, blue, white, black, yellow, cyan, magenta, orange, purple, pink, gray, darkblue, darkgreen, darkred, lightblue, brown.

**Build:**
```bash
pacman -S mingw-w64-x86_64-sdl3 mingw-w64-x86_64-sdl3-image
gcc -shared -o tools/sdl3_render.dll runtime/tools/sdl3_render.c \
    runtime/src/cJSON.c runtime/src/sandbox.c \
    -I runtime/src/include -lSDL3 -lSDL3_image -lm
```

Requires `SDL3.dll` and `SDL3_image.dll` in the executable's directory (provided in `libs/`).

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
