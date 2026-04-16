#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sdl3_debugger.h"
#include "event_bus.h"

/* Only compile SDL3 code if the headers are available */
#ifdef ENABLE_SDL3_DEBUG

#include <SDL3/SDL.h>

/* ---- State ---- */
static SDL_Window   *g_win = NULL;
static SDL_Renderer *g_ren = NULL;
static int g_initialized = 0;

/* Latest state snapshot (updated by events) */
static struct {
    char  phase[32];
    int   iteration;
    int   step_id;
    char  last_tool[64];
    int   last_success;
    int   tool_calls;
    int   errors;
    char  files_info[256];
    double progress;
} g_state;

/* ---- Rendering ---- */
static void render_frame(void) {
    if (!g_ren) return;

    /* Background */
    SDL_SetRenderDrawColorFloat(g_ren, 0.1f, 0.1f, 0.12f, 1.0f);
    SDL_RenderClear(g_ren);

    /* Title bar */
    SDL_SetRenderDrawColorFloat(g_ren, 0.2f, 0.6f, 1.0f, 1.0f);
    SDL_RenderDebugText(g_ren, 8, 4, "BashAgent Debugger");

    /* Phase + iteration */
    char line[256];
    snprintf(line, sizeof(line), "Phase: %-10s  Iter: %d  Steps: %d",
             g_state.phase, g_state.iteration, g_state.tool_calls);
    SDL_SetRenderDrawColorFloat(g_ren, 0.9f, 0.9f, 0.9f, 1.0f);
    SDL_RenderDebugText(g_ren, 8, 22, line);

    /* Progress bar */
    float bar_x = 8, bar_y = 42, bar_w = 620, bar_h = 14;
    SDL_FRect bg_rect = { bar_x, bar_y, bar_w, bar_h };
    SDL_SetRenderDrawColorFloat(g_ren, 0.3f, 0.3f, 0.3f, 1.0f);
    SDL_RenderFillRect(g_ren, &bg_rect);

    float fill_w = bar_w * (float)g_state.progress;
    if (fill_w > 0) {
        SDL_FRect fill_rect = { bar_x, bar_y, fill_w, bar_h };
        float pr = g_state.progress > 0.8f ? 0.2f : (g_state.progress > 0.5f ? 0.8f : 1.0f);
        float pg = g_state.progress > 0.5f ? 0.8f : 0.6f;
        SDL_SetRenderDrawColorFloat(g_ren, pr, pg, 0.2f, 1.0f);
        SDL_RenderFillRect(g_ren, &fill_rect);
    }

    snprintf(line, sizeof(line), "%.0f%%", g_state.progress * 100);
    SDL_SetRenderDrawColorFloat(g_ren, 1.0f, 1.0f, 1.0f, 1.0f);
    SDL_RenderDebugText(g_ren, bar_x + bar_w / 2 - 12, bar_y, line);

    /* Last tool call */
    snprintf(line, sizeof(line), "Last: %s %s  Errors: %d",
             g_state.last_tool, g_state.last_success ? "OK" : "FAIL",
             g_state.errors);
    SDL_SetRenderDrawColorFloat(g_ren, g_state.last_success ? 0.4f : 1.0f,
                                g_state.last_success ? 1.0f : 0.3f,
                                0.3f, 1.0f);
    SDL_RenderDebugText(g_ren, 8, 64, line);

    /* Files info */
    if (g_state.files_info[0]) {
        SDL_SetRenderDrawColorFloat(g_ren, 0.6f, 0.8f, 0.6f, 1.0f);
        SDL_RenderDebugText(g_ren, 8, 82, g_state.files_info);
    }

    SDL_RenderPresent(g_ren);
}

/* ---- Event callbacks ---- */

static int on_step_created(const Event *ev, void *ud) {
    (void)ud;
    snprintf(g_state.phase, sizeof(g_state.phase), "planning");
    g_state.step_id = ev->step_created.step_id;
    render_frame();
    return 0;
}

static int on_tool_result(const Event *ev, void *ud) {
    (void)ud;
    snprintf(g_state.phase, sizeof(g_state.phase), "acting");
    g_state.tool_calls++;
    strncpy(g_state.last_tool, ev->tool_result.tool, sizeof(g_state.last_tool) - 1);
    g_state.last_success = ev->tool_result.success;
    if (!ev->tool_result.success) g_state.errors++;
    g_state.step_id = ev->tool_result.step_id;
    render_frame();
    return 0;
}

static int on_state_update(const Event *ev, void *ud) {
    (void)ud;
    if (ev->state_update.stage)
        strncpy(g_state.phase, ev->state_update.stage, sizeof(g_state.phase) - 1);
    g_state.progress = ev->state_update.progress;
    render_frame();
    return 0;
}

static int on_done(const Event *ev, void *ud) {
    (void)ud;
    snprintf(g_state.phase, sizeof(g_state.phase), "DONE");
    g_state.progress = 1.0;
    render_frame();
    return 0;
}

/* ---- Public API ---- */

void sdl3_debugger_init(void) {
    if (g_initialized) return;

    memset(&g_state, 0, sizeof(g_state));
    snprintf(g_state.phase, sizeof(g_state.phase), "init");

    /* Check if SDL is already initialized (by sdl3_render tool) */
    if (!SDL_WasInit(SDL_INIT_VIDEO)) {
        if (!SDL_Init(SDL_INIT_VIDEO)) {
            fprintf(stderr, "[DEBUG] SDL3 init failed: %s\n", SDL_GetError());
            return;
        }
    }

    if (!SDL_CreateWindowAndRenderer("BashAgent Debugger", 640, 200,
                                      SDL_WINDOW_RESIZABLE, &g_win, &g_ren)) {
        fprintf(stderr, "[DEBUG] SDL3 window failed: %s\n", SDL_GetError());
        return;
    }

    g_initialized = 1;
    render_frame();

    /* Subscribe to events */
    event_bus_subscribe(EVENT_STEP_CREATED,  on_step_created, NULL);
    event_bus_subscribe(EVENT_TOOL_RESULT,   on_tool_result,  NULL);
    event_bus_subscribe(EVENT_STATE_UPDATE,  on_state_update, NULL);
    event_bus_subscribe(EVENT_DONE,          on_done,         NULL);

    fprintf(stderr, "[DEBUG] SDL3 debugger window opened\n");
}

void sdl3_debugger_shutdown(void) {
    if (!g_initialized) return;
    if (g_ren) SDL_DestroyRenderer(g_ren);
    if (g_win) SDL_DestroyWindow(g_win);
    g_ren = NULL;
    g_win = NULL;
    g_initialized = 0;
}

#else /* !ENABLE_SDL3_DEBUG */

/* Stubs when SDL3 debug is disabled */
void sdl3_debugger_init(void) {}
void sdl3_debugger_shutdown(void) {}

#endif
