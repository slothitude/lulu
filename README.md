<div align="center">
  <img src="lulu_logo.jpg" alt="Lulu Logo" width="200">
  <h1>Lulu v5.0 — Autonomous Intelligence Agent</h1>
</div>

Local autonomous agent with a planner/actor/critic intelligence pipeline, embedded property graph database, DLL-loaded tools, persistent tasks, and simultaneous CLI + Telegram channels. Written in C11.

**v5.0** upgrades the agent from a basic LLM loop to a three-stage reasoning pipeline (planner → actor → critic), adds decision learning, HNSW-accelerated semantic memory, SSE streaming with token tracking, TextInput widgets with keyboard navigation, and a full test suite.

## What's New in v5.0

| Phase | Feature | Description |
|-------|---------|-------------|
| **6** | Planner/Actor/Critic Pipeline | Tasks are planned, executed, and evaluated through distinct reasoning stages |
| **7** | Decision Learning | Agent adjusts task scoring weights based on success/failure patterns |
| **8** | SDL3 UI Expansion | TextInput widget, keyboard navigation (Tab/Enter/Space), resizable windows |
| **9** | Memory Performance | HNSW-lite index for fast embedding search, smart memory compaction |
| **10** | Testing & Hardening | 26 automated tests, sandbox hardening (UNC/null bytes/device names), CI workflow |
| **11** | Streaming & Multi-Provider | SSE token streaming, token usage tracking, multi-provider routing |

## Quick Start

```bash
# Build (from bashagent/build/)
cmake -G "MinGW Makefiles" ..
cmake --build . --target agent
cp _deps/kuzu-build/src/libryu_shared.dll .

# Run
./agent.exe                    # Always-on agent — CLI + Telegram
./agent.exe "list the files"   # One-shot prompt
./agent.exe --replay           # Log viewer
```

## Architecture

```
┌─────────────────────────────────────────────────────────┐
│            v5.0 Intelligence Architecture                │
│                                                          │
│  ┌─────────┐     ┌───────┐     ┌────────┐              │
│  │ PLANNER │────▶│ ACTOR │────▶│ CRITIC │              │
│  │ What to │     │ Do it  │     │ Good?  │              │
│  │  do?    │     │        │     │        │              │
│  └─────────┘     └───────┘     └────────┘              │
│       │                            │                     │
│       └──────── retry ◀───────────┘                     │
│                                                          │
│  ┌──────────────────────────────────────────────────┐   │
│  │          RyuGraph Property Graph                  │   │
│  │  Nodes: TASK  MEMORY  GOAL  SESSION  MESSAGE     │   │
│  │         TOOL_CALL  PROMPT_CACHE  FILE  SCRIPT     │   │
│  │         LOG_EVENT                                 │   │
│  │  Rels:  CALLED  USED  RECORDED_IN                 │   │
│  └──────────────────────────────────────────────────┘   │
│                                                          │
│  Channels:  CLI (stdin) + Telegram (TDLib)               │
│  Tools:     7 DLL tools + replay_script pseudo-tool      │
│  LLM:       OpenAI-compatible with SSE streaming         │
│  Memory:    HNSW-lite indexed semantic search            │
└─────────────────────────────────────────────────────────┘
```

### Planner/Actor/Critic Pipeline

Tasks run through three reasoning stages:

1. **Planner** — Breaks the goal into concrete steps using rich context (goal, semantic memories, script matches, recent tasks)
2. **Actor** — Executes tools step-by-step, supports structured `<tool_call name="..." args="{...}"/>` tags
3. **Critic** — Evaluates results with confidence scoring. Can approve or trigger revision (up to 2 retries)

The critic returns structured JSON: `{"status":"done|revise|continue","confidence":0.95,"summary":"..."}`

### Decision Engine — Learning from Experience

`decision_learn()` adjusts scoring weights based on task outcomes:
- Extracts tool sequences from completed tasks
- Stores weights as MEMORY nodes in the graph (category="decision_weights")
- Exponential moving average update (alpha=0.3) with decay
- Weights influence `decision_pick_task()` scoring

### Graph Schema

All state lives in an embedded property graph. No flat JSON files.

| Node Table | Key Fields | Purpose |
|------------|-----------|---------|
| TASK | id, name, prompt, status, priority, plan, state | Autonomous tasks |
| MEMORY | id, category, key, content, embedding | Working memory + learned weights |
| GOAL | id, text, status | Active goals |
| SESSION | id, chat_id, channel | Per-chat sessions |
| MESSAGE | id, session_id, role, content, seq | Chat history |
| TOOL_CALL | id, tool, args_json, result_json, duration_ms | Tool invocations |
| PROMPT_CACHE | hash, response, hit_count | LLM response cache |
| FILE | id, path, hash, size | File tracking |
| SCRIPT | id, name, tool_sequence | Mined workflows |
| LOG_EVENT | id, type, stage, json_data, success | Execution log |

### Semantic Memory with HNSW

Memory embeddings are indexed with an HNSW-lite adjacency list for O(log n) search:
- Index built lazily on first search, invalidated on memory mutations
- Smart compaction merges similar memories (cosine > threshold) before deletion
- Falls back to full-scan if index not yet built

## Directory Structure

```
bashagent/
├── CMakeLists.txt              # CMake build (FetchContent for RyuGraph)
├── agent.json                  # Agent behavior config + role prompts
├── runtime/
│   ├── src/
│   │   ├── main.c             # Core loop, planner/actor/critic pipeline
│   │   ├── agent_core.c       # Prompt building utilities
│   │   ├── agent_db.c         # Graph storage + HNSW index + smart compact
│   │   ├── llm.c              # LLM client + streaming + token tracking
│   │   ├── decision_engine.c  # Scored scheduler + learning weights
│   │   ├── channel.c          # Unified CLI + Telegram input
│   │   ├── tasks.c            # Task system (delegates to agent_db)
│   │   ├── session.c          # Per-chat history (graph-backed)
│   │   ├── tools.c            # DLL tool loader
│   │   ├── sandbox.c          # Path traversal + security protection
│   │   ├── state.c            # Memory, goals, logging (graph-backed)
│   │   ├── agent_config.c     # JSON config + role prompt loader
│   │   ├── telegram.c         # TDLib JSON wrapper
│   │   └── include/           # Headers
│   ├── tools/                 # Tool DLL sources + built DLLs
│   └── tests/                 # Test suite (26 tests)
├── .github/workflows/
│   ├── release.yml            # Build + release on tag push
│   └── ci.yml                 # CI: build + test on push
├── build/                      # CMake build directory
├── workspace/                 # Sandboxed file operations root
└── state/
    ├── graph.kuzu             # Property graph database
    └── log.jsonl              # Execution log (JSONL)
```

## Commands

| Command | Action |
|---------|--------|
| `/stop` | Shutdown agent |
| `/clear` | Clear this chat's history |
| `/status` | Show uptime, tasks, graph stats, token usage |
| `/files` | List workspace files |
| `/tasks` | Show all tasks with status |
| `/decide` | Show last decision engine debug (includes learned weights) |
| `/goal <text>` | Create a high-priority autonomous task |
| `/graph <cypher>` | Run read-only Cypher query on the graph |
| `/scripts` | List mined workflow scripts |
| `/update` | Check GitHub for new version |

## Configuration

### agent.json — Pipeline & Behavior

```json
{
  "roles": {
    "planner": {
      "system": "You are a task planner...",
      "output_format": "OUTPUT FORMAT...",
      "rules": ["Each step must use exactly one tool", "Keep steps small"]
    },
    "actor": {
      "system": "You are an AI agent executing a specific task step...",
      "output_format": "OUTPUT FORMAT..."
    },
    "critic": {
      "system": "You are a critical reviewer...",
      "output_format": "OUTPUT FORMAT..."
    }
  },
  "pipeline": [
    { "role": "planner", "enabled": true },
    { "role": "actor",   "enabled": true },
    { "role": "critic",  "enabled": true }
  ],
  "behavior": {
    "max_iterations": 10,
    "enable_prompt_cache": true,
    "enable_telegram": true
  }
}
```

## Tool System

### Built-in Tools

| DLL | Tool | Purpose |
|-----|------|---------|
| `create_file.dll` | `create_file(path, content)` | Write file to workspace |
| `read_file.dll` | `read_file(path)` | Read file from workspace |
| `list_files.dll` | `list_files()` | List workspace contents |
| `run_test.dll` | `run_test()` | Parse test results |
| `sdl3_render.dll` | `sdl3_render(action, ...)` | Widget UI system + PNG export |
| `telegram_send.dll` | `telegram_send(text)` | Send via Telegram |
| `none.dll` | `none(reason)` | No-op |

Plus the built-in `replay_script` pseudo-tool for re-executing mined workflows.

### SDL3 Widget System (19 Widgets)

**19 widgets**: VBox, HBox, Margin, Center, Grid, Label, ColorRect, HSeparator, VSeparator, Button, CheckBox, ProgressBar, OptionButton, SpinBox, Panel, PanelContainer, ScrollContainer, TabContainer, **TextInput**.

**Interactive window features**:
- Click hit-testing and signal routing to agent
- Tab/Shift+Tab keyboard focus cycling
- Enter triggers Button/TextInput, Space toggles CheckBox
- Resizable windows with dynamic layout recalculation
- Blinking cursor on focused TextInput

## Testing

```bash
cd runtime/tests
gcc -std=c11 -I../src/include -o test_runner.exe \
    test_runner.c test_sandbox.c test_tools.c test_agent_db.c \
    test_channel.c test_tasks.c ../src/sandbox.c ../src/cJSON.c
./test_runner.exe
```

26 tests covering sandbox security, cJSON parsing, and test infrastructure.

## Security

- All file operations sandboxed to `workspace/`
- Path traversal (`..`, absolute paths, system dirs) blocked
- UNC path escape (`\\?\`, `\\server`) blocked
- Null byte injection in filenames blocked
- Windows device names (CON, PRN, AUX, NUL) blocked
- Case-insensitive workspace prefix check on Windows
- No arbitrary command execution
- Tool DLLs have no LLM access
- `/graph` restricted to read-only MATCH/RETURN queries

## Build Requirements

- [MSYS2](https://www.msys2.org/) with MinGW64 GCC 15+
- CMake 3.20+
- SDL3 + SDL3_image (for sdl3_render tool)
- TDLib (for Telegram integration)
- WinHTTP (system library)

## License

MIT
