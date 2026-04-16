# Lulu v3.1 — Always-On Autonomous Agent

Local autonomous agent with DLL-loaded tools, persistent tasks, and simultaneous CLI + Telegram channels. Written in C11.

**v3.1** adds a two-thread architecture, thread-safe shared state, priority aging, session TTL, and tool result truncation.

## Quick Start

```bash
# From bashagent/
run.bat              # Agent starts — CLI + Telegram simultaneously
run.bat --build      # Force rebuild first

# One-shot: run a single prompt and exit
echo "text" | run.bat "summarize this"
run.bat "list all files in workspace"

# Log viewer
run.bat --replay
run.bat --replay --last 20 --stage actor
```

**No flags needed.** No `--chat`, `--listen`, or `--pipeline`. The agent is the default.

## Architecture

```
┌─────────────────────────────────────────────────────┐
│              Two-Thread Architecture                 │
│                                                      │
│  Main Thread (I/O)          Worker Thread            │
│  ┌──────────────┐           ┌──────────────┐        │
│  │ poll channels │           │ agent_think() │        │
│  │  CLI + TG     │           │ next_runnable │        │
│  │ route events  │           │ execute_task() │       │
│  │ → command     │           │ periodic save  │       │
│  │ → chat+tools  │           │ session prune  │       │
│  └──────────────┘           │ Sleep on CV    │       │
│                              └──────────────┘        │
│                                                      │
│  Thread safety: CRITICAL_SECTION on tasks, sessions,  │
│  channel queue. CONDITION_VARIABLE for task wake.    │
│                                                      │
│  Channels:  CLI (stdin) + Telegram (TDLib)           │
│  Tasks:     tasks.json (survives restart)            │
│  Sessions:  per-chat history (linked list, dynamic)  │
│  Tools:     DLL system (runtime-loaded)              │
│  LLM:       OpenAI-compatible (WinHTTP)              │
└─────────────────────────────────────────────────────┘
```

### execute_task() — Structured Autonomy

Tasks don't just "LLM → tool → repeat". They run through internal phases:

1. **Plan** — LLM generates approach using task prompt + previous state
2. **Act** — Tool execution loop (capped at 8 tool steps)
3. **Evaluate** — Check if task is done or needs retry

Each task carries rolling `state`, `last_error`, and `plan` fields so retries aren't blind.

### Priority + Cooldown Scheduler

`tasks_next_runnable()` picks the highest-priority eligible task:
- Pending tasks run immediately
- Failed tasks retry after cooldown (30s × attempts, capped at 300s max)
- Permanently failed (max_attempts reached) are skipped
- Priority aging: tasks waiting > 60s get +1 effective priority (cap 10)

### Robustness Features

- **Session TTL**: idle sessions pruned after 1 hour (`session_prune`)
- **Tool result truncation**: results > 4KB truncated to prevent context explosion
- **Worker status**: `/status` shows `Worker: BUSY` or `Worker: IDLE`
- **Thread safety**: `CRITICAL_SECTION` guards on tasks, sessions, and channel queue; `CONDITION_VARIABLE` wakes worker on new tasks

## Directory Structure

```
bashagent/
├── run.bat                    # Build + run entrypoint
├── config.json                # LLM credentials (not committed)
├── agent.json                 # Agent behavior config
├── runtime/
│   ├── build/agent.exe        # Built binary
│   ├── src/
│   │   ├── main.c             # Core loop, message handling, one-shot
│   │   ├── channel.c          # Unified CLI + Telegram input
│   │   ├── tasks.c            # Persistent task system
│   │   ├── session.c          # Per-chat history management
│   │   ├── llm.c              # OpenAI-compatible HTTP client (WinHTTP)
│   │   ├── tools.c            # DLL tool loader
│   │   ├── sandbox.c          # Path traversal protection
│   │   ├── state.c            # Memory, goals, logging, file I/O
│   │   ├── agent_config.c     # JSON config loader
│   │   ├── event_bus.c        # Synchronous pub/sub
│   │   ├── telegram.c         # TDLib JSON wrapper
│   │   ├── cJSON.c            # Vendored JSON parser
│   │   ├── include/           # Headers
│   │   ├── subscribers/       # Event bus subscribers (log, mem, SDL3, TG)
│   │   └── tools/             # Tool DLL sources
│   └── tools/                 # Built DLLs
├── tools/                     # Runtime-loaded tool DLLs
├── workspace/                 # Sandboxed file operations root
└── state/
    ├── log.jsonl              # Execution log (JSONL)
    ├── memory.json            # Persistent working memory + goals
    ├── tasks.json             # Persistent task queue
    └── prompt_cache.json      # LLM response cache
```

## Commands (work from any channel)

| Command | Action |
|---------|--------|
| `/stop` | Shutdown agent |
| `/clear` | Clear this chat's history |
| `/status` | Show uptime, steps, messages, tasks |
| `/files` | List workspace files |
| `/tasks` | Show all tasks with status |
| `/goal <text>` | Create a high-priority task |

## Configuration

### config.json — LLM Credentials

```json
{
  "provider": "openai",
  "model": "glm-5.1",
  "endpoint": "https://your-llm-endpoint/chat/completions",
  "apikey": "your-key",
  "max_iterations": 10,
  "http_timeout_ms": 30000
}
```

### agent.json — Agent Behavior

```json
{
  "shared": {
    "tools_list": "auto-populated from loaded DLLs"
  },
  "tools": [
    { "name": "create_file", "enabled": true },
    { "name": "read_file",   "enabled": true },
    { "name": "list_files",  "enabled": true }
  ],
  "behavior": {
    "max_iterations": 10,
    "enable_prompt_cache": true,
    "cache_path": "state/prompt_cache.json",
    "enable_telegram": true,
    "telegram_bot_token": "your-bot-token",
    "telegram_chat_id": "123456789"
  }
}
```

## Tool System

Tools are loaded as DLLs from the `tools/` directory at runtime. No recompilation needed to add or remove tools.

### Tool ABI

Each DLL exports two functions:

```c
const ToolInfo *tool_get_info(void);       // name + description
tool_execute_fn tool_get_execute(void);     // cJSON *(*fn)(cJSON *args, const char *workspace, char **err)
```

### Built-in Tools

| DLL | Tool | Purpose |
|-----|------|---------|
| `create_file.dll` | `create_file(path, content)` | Write file to workspace |
| `read_file.dll` | `read_file(path)` | Read file from workspace |
| `list_files.dll` | `list_files()` | List workspace contents |
| `run_test.dll` | `run_test()` | Parse test_results.txt |
| `sdl3_render.dll` | `sdl3_render(action, path, ...)` | Render shapes/text to images |
| `none.dll` | `none(reason)` | No-op |
| `telegram_send.dll` | `telegram_send(text)` | Send via Telegram |

### Adding a New Tool

1. Create `runtime/tools/my_tool.c`:

```c
#define _CRT_SECURE_NO_WARNINGS
#define TOOL_BUILDING_DLL
#include "tool_api.h"

static cJSON *my_tool(cJSON *args, const char *workspace, char **error) {
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "status", "ok");
    return r;
}

static ToolInfo info = { .api_version = 1, .struct_size = sizeof(ToolInfo),
    .name = "my_tool", .description = "Does something useful" };

TOOL_EXPORT const ToolInfo *tool_get_info(void) { return &info; }
TOOL_EXPORT tool_execute_fn tool_get_execute(void) { return my_tool; }
```

2. It auto-compiles with `run.bat --build`.

## Prompt Cache

LLM responses are cached using FNV-1a 64-bit hash deduplication. Identical prompts skip the HTTP call. Cache persists across runs as JSON.

## Build Requirements

- [MSYS2](https://www.msys2.org/) with MinGW64 GCC 15+
- SDL3 + SDL3_image (for sdl3_render tool)
- TDLib (for Telegram integration)
- WinHTTP (system library)

## Security

- All file operations sandboxed to `workspace/`
- Path traversal (`..`, absolute paths, system dirs) blocked
- No arbitrary command execution
- Tool DLLs have no LLM access
- `config.json` excluded from git (contains API key)

## License

MIT
