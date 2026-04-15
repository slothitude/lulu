#define _CRT_SECURE_NO_WARNINGS
#define TOOL_BUILDING_DLL
#define SDL_MAIN_USE_NOOP
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "tool_api.h"
#include "tool_helpers.h"
#include "sandbox.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3_image/SDL_image.h>

/* ========================= Helpers ========================= */

typedef struct { const char *name; unsigned char r, g, b; } NamedColor;
static const NamedColor NAMED_COLORS[] = {
    {"red",     255,0,0},     {"green",   0,128,0},   {"blue",    0,0,255},
    {"white",   255,255,255}, {"black",   0,0,0},     {"yellow",  255,255,0},
    {"cyan",    0,255,255},   {"magenta", 255,0,255},  {"orange",  255,165,0},
    {"purple",  128,0,128},   {"pink",    255,192,203},{"gray",    128,128,128},
    {"grey",    128,128,128}, {"darkblue",0,0,139},    {"darkgreen",0,100,0},
    {"darkred", 139,0,0},     {"lightblue",173,216,230},{"brown", 139,69,19},
    {NULL, 0,0,0}
};

static int parse_color(const char *color, SDL_FColor *out) {
    if (!color) { out->r=1; out->g=1; out->b=1; out->a=1; return 0; }

    /* Hex format: #RRGGBB */
    if (color[0] == '#' && strlen(color) >= 7) {
        unsigned int r, g, b;
        if (sscanf(color + 1, "%2x%2x%2x", &r, &g, &b) == 3) {
            out->r = r / 255.0f; out->g = g / 255.0f; out->b = b / 255.0f;
            out->a = 1.0f; return 0;
        }
    }

    /* Named color */
    for (const NamedColor *nc = NAMED_COLORS; nc->name; nc++) {
        if (strcmp(color, nc->name) == 0) {
            out->r = nc->r / 255.0f; out->g = nc->g / 255.0f; out->b = nc->b / 255.0f;
            out->a = 1.0f; return 0;
        }
    }

    out->r=1; out->g=1; out->b=1; out->a=1;
    return -1;
}

/* Get a number from JSON, trying multiple key names */
static double get_num(cJSON *obj, const char *key1, const char *key2, double def) {
    cJSON *item = cJSON_GetObjectItem(obj, key1);
    if (!item) item = cJSON_GetObjectItem(obj, key2);
    if (cJSON_IsNumber(item)) return item->valuedouble;
    return def;
}

/* Get a string from JSON, trying multiple key names */
static const char *get_str(cJSON *obj, const char *key1, const char *key2) {
    cJSON *item = cJSON_GetObjectItem(obj, key1);
    if (!item) item = cJSON_GetObjectItem(obj, key2);
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

/* Get bool from JSON, trying multiple key names */
static int get_bool(cJSON *obj, const char *key1, const char *key2, int def) {
    cJSON *item = cJSON_GetObjectItem(obj, key1);
    if (!item) item = cJSON_GetObjectItem(obj, key2);
    if (cJSON_IsBool(item)) return cJSON_IsTrue(item);
    return def;
}

static void draw_circle(SDL_Renderer *renderer, float cx, float cy, float r, int fill) {
    int segments = (int)(r * 2);
    if (segments < 16) segments = 16;
    if (segments > 128) segments = 128;

    float *pts = (float *)malloc(sizeof(float) * segments * 2);
    if (!pts) return;

    for (int i = 0; i < segments; i++) {
        float angle = (2.0f * 3.14159265f * i) / segments;
        pts[i * 2]     = cx + r * cosf(angle);
        pts[i * 2 + 1] = cy + r * sinf(angle);
    }

    if (fill) {
        SDL_FColor cur_color;
        SDL_GetRenderDrawColorFloat(renderer, &cur_color.r, &cur_color.g, &cur_color.b, &cur_color.a);
        for (int i = 0; i < segments; i++) {
            int next = (i + 1) % segments;
            SDL_Vertex verts[3];
            verts[0].position.x = cx;     verts[0].position.y = cy;
            verts[1].position.x = pts[i * 2];      verts[1].position.y = pts[i * 2 + 1];
            verts[2].position.x = pts[next * 2];    verts[2].position.y = pts[next * 2 + 1];
            verts[0].color = verts[1].color = verts[2].color = cur_color;
            verts[0].tex_coord = verts[1].tex_coord = verts[2].tex_coord = (SDL_FPoint){0,0};
            SDL_RenderGeometry(renderer, NULL, verts, 3, NULL, 0);
        }
    } else {
        SDL_RenderLines(renderer, (SDL_FPoint*)pts, segments);
    }
    free(pts);
}

static int render_items(SDL_Renderer *renderer, cJSON *items) {
    if (!cJSON_IsArray(items)) return 0;
    int n = cJSON_GetArraySize(items);
    for (int i = 0; i < n; i++) {
        cJSON *item = cJSON_GetArrayItem(items, i);
        const char *type = get_str(item, "type", NULL);
        if (!type) continue;

        /* Map type aliases: "rectangle" -> "rect", "background" -> skip */
        int skip = 0;
        const char *real_type = type;
        if (strcmp(type, "rectangle") == 0) real_type = "rect";
        if (strcmp(type, "background") == 0) skip = 1;

        if (skip) continue;

        const char *color_str = get_str(item, "color", NULL);
        SDL_FColor color;
        parse_color(color_str, &color);
        SDL_SetRenderDrawColorFloat(renderer, color.r, color.g, color.b, color.a);

        if (strcmp(real_type, "rect") == 0) {
            SDL_FRect rect;
            rect.x = (float)get_num(item, "x", "cx", 0);
            rect.y = (float)get_num(item, "y", "cy", 0);
            rect.w = (float)get_num(item, "w", "width", 50);
            rect.h = (float)get_num(item, "h", "height", 50);
            int fill = get_bool(item, "fill", "filled", 0);
            if (fill)
                SDL_RenderFillRect(renderer, &rect);
            else
                SDL_RenderRect(renderer, &rect);

        } else if (strcmp(real_type, "line") == 0) {
            SDL_RenderLine(renderer,
                (float)get_num(item, "x1", "x", 0),
                (float)get_num(item, "y1", "y", 0),
                (float)get_num(item, "x2", "x2", 100),
                (float)get_num(item, "y2", "y2", 100));

        } else if (strcmp(real_type, "point") == 0) {
            SDL_RenderPoint(renderer,
                (float)get_num(item, "x", "cx", 0),
                (float)get_num(item, "y", "cy", 0));

        } else if (strcmp(real_type, "circle") == 0) {
            float radius = (float)get_num(item, "r", "radius", 50);
            int fill = get_bool(item, "fill", "filled", 0);
            draw_circle(renderer,
                (float)get_num(item, "x", "cx", 0),
                (float)get_num(item, "y", "cy", 0),
                radius, fill);

        } else if (strcmp(real_type, "text") == 0) {
            const char *text = get_str(item, "text", "content");
            if (text) {
                SDL_RenderDebugText(renderer,
                    (float)get_num(item, "x", "cx", 0),
                    (float)get_num(item, "y", "cy", 0),
                    text);
            }
        }
    }
    return n;
}

static int ends_with(const char *s, const char *suffix) {
    size_t slen = strlen(s), suflen = strlen(suffix);
    if (suflen > slen) return 0;
    return strcmp(s + slen - suflen, suffix) == 0;
}

/* Try to get the items array from multiple common key names */
static cJSON *find_items(cJSON *args) {
    const char *keys[] = {"items", "shapes", "elements", "objects", "drawings", NULL};
    for (const char **k = keys; *k; k++) {
        cJSON *arr = cJSON_GetObjectItem(args, *k);
        if (cJSON_IsArray(arr)) return arr;
    }
    return NULL;
}

/* Try to get the path string from multiple common key names */
static const char *find_path(cJSON *args) {
    const char *keys[] = {"path", "file", "output_path", "output", "filename", "dest", NULL};
    for (const char **k = keys; k && *k; k++) {
        const char *val = get_str(args, *k, NULL);
        if (val) return val;
    }
    return NULL;
}

/* ========================= Tool entry ========================= */

static cJSON *sdl3_render(cJSON *args, const char *workspace, char **error) {
    const char *action = get_str(args, "action", "mode");
    if (!action) action = "render";

    /* ---- info action ---- */
    if (strcmp(action, "info") == 0 || strcmp(action, "get_info") == 0
        || strcmp(action, "version") == 0) {
        int ver = SDL_GetVersion();
        cJSON *r = cJSON_CreateObject();
        char ver_str[32];
        snprintf(ver_str, sizeof(ver_str), "%d.%d.%d",
                 SDL_VERSIONNUM_MAJOR(ver),
                 SDL_VERSIONNUM_MINOR(ver),
                 SDL_VERSIONNUM_MICRO(ver));
        cJSON_AddStringToObject(r, "version", ver_str);
        cJSON_AddStringToObject(r, "status", "ok");
        return r;
    }

    /* Resolve items and background (shared between render & window) */
    cJSON *items = find_items(args);
    const char *bg_str = get_str(args, "bg_color", "background");
    SDL_FColor bg;
    parse_color(bg_str ? bg_str : "#000000", &bg);
    int width = (int)get_num(args, "width", "w", 640);
    int height = (int)get_num(args, "height", "h", 480);

    /* ---- window action ---- */
    if (strcmp(action, "window") == 0 || strcmp(action, "show") == 0
        || strcmp(action, "display") == 0) {
        int timeout_ms = (int)get_num(args, "timeout_ms", "timeout", 5000);
        const char *title = get_str(args, "title", "name");
        if (!title) title = "BashAgent SDL3";

        SDL_Window *window;
        SDL_Renderer *renderer;
        if (!SDL_Init(SDL_INIT_VIDEO)) {
            char buf[256];
            snprintf(buf, sizeof(buf), "SDL3 init error: %s", SDL_GetError());
            *error = _strdup(buf);
            return NULL;
        }
        if (!SDL_CreateWindowAndRenderer(title, width, height, 0, &window, &renderer)) {
            char buf[256];
            snprintf(buf, sizeof(buf), "SDL3 window error: %s", SDL_GetError());
            *error = _strdup(buf);
            return NULL;
        }

        SDL_SetRenderDrawColorFloat(renderer, bg.r, bg.g, bg.b, bg.a);
        SDL_RenderClear(renderer);
        if (items) render_items(renderer, items);
        SDL_RenderPresent(renderer);

        Uint64 start = SDL_GetTicks();
        int running = 1;
        while (running) {
            SDL_Event e;
            while (SDL_PollEvent(&e)) {
                if (e.type == SDL_EVENT_QUIT) running = 0;
                if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_ESCAPE) running = 0;
            }
            if ((int)(SDL_GetTicks() - start) >= timeout_ms) running = 0;
            SDL_Delay(16);
        }

        Uint64 duration = SDL_GetTicks() - start;
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();

        cJSON *r = cJSON_CreateObject();
        cJSON_AddStringToObject(r, "status", "shown");
        cJSON_AddNumberToObject(r, "duration_ms", (double)duration);
        return r;
    }

    /* ---- render action (offscreen → file) ---- */
    if (strcmp(action, "render") == 0 || strcmp(action, "save") == 0
        || strcmp(action, "export") == 0 || strcmp(action, "create") == 0) {
        const char *path_str = find_path(args);
        if (!path_str) TOOL_ERROR("sdl3_render requires 'path' (or 'file') argument for output filename");

        char path_buf[512];
        strncpy(path_buf, path_str, sizeof(path_buf) - 1);
        path_buf[sizeof(path_buf) - 1] = 0;
        tool_normalize_path(path_buf);

        char *full_path = sandbox_resolve_path(path_buf, workspace);
        if (!full_path) TOOL_ERROR("path traversal blocked or invalid path");

        /* Initialize SDL video subsystem */
        if (!SDL_Init(SDL_INIT_VIDEO)) {
            char buf[256];
            snprintf(buf, sizeof(buf), "SDL3 init error: %s", SDL_GetError());
            free(full_path);
            *error = _strdup(buf);
            return NULL;
        }

        /* Use software renderer with surface for reliable offscreen rendering */
        SDL_Surface *surface = SDL_CreateSurface(width, height, SDL_PIXELFORMAT_RGBA8888);
        if (!surface) {
            char buf[256];
            snprintf(buf, sizeof(buf), "SDL3 surface error: %s", SDL_GetError());
            SDL_Quit();
            free(full_path);
            *error = _strdup(buf);
            return NULL;
        }

        SDL_Renderer *renderer = SDL_CreateSoftwareRenderer(surface);
        if (!renderer) {
            char buf[256];
            snprintf(buf, sizeof(buf), "SDL3 renderer error: %s", SDL_GetError());
            SDL_DestroySurface(surface);
            SDL_Quit();
            free(full_path);
            *error = _strdup(buf);
            return NULL;
        }

        SDL_SetRenderDrawColorFloat(renderer, bg.r, bg.g, bg.b, bg.a);
        SDL_RenderClear(renderer);
        int item_count = 0;
        if (items) item_count = render_items(renderer, items);
        SDL_RenderPresent(renderer);

        /* Save surface to file */
        int is_png = ends_with(full_path, ".png");
        int save_ok = 0;
        const char *sdl_err = "";

        /* Remove existing file first (SDL won't overwrite) */
        remove(full_path);

        if (is_png) {
            save_ok = IMG_SavePNG(surface, full_path);
        } else {
            save_ok = SDL_SaveBMP(surface, full_path);
        }
        if (!save_ok) sdl_err = SDL_GetError();

        SDL_DestroyRenderer(renderer);
        SDL_DestroySurface(surface);
        SDL_Quit();

        if (!save_ok) {
            char buf[512];
            snprintf(buf, sizeof(buf), "failed to save image to '%s': %s", full_path, sdl_err);
            free(full_path);
            *error = _strdup(buf);
            return NULL;
        }

        cJSON *r = cJSON_CreateObject();
        cJSON_AddStringToObject(r, "status", "saved");
        cJSON_AddStringToObject(r, "path", path_buf);
        cJSON_AddNumberToObject(r, "width", width);
        cJSON_AddNumberToObject(r, "height", height);
        cJSON_AddNumberToObject(r, "items", item_count);
        cJSON_AddStringToObject(r, "format", is_png ? "png" : "bmp");
        free(full_path);
        return r;
    }

    TOOL_ERROR("unknown action '%s', expected: render, window, info");
}

static ToolInfo info = {
    TOOL_API_VERSION_MAX,
    sizeof(ToolInfo),
    "sdl3_render",
    "Render shapes/text to BMP/PNG image or display in a window using SDL3",
    1,  /* requires_workspace */
    1,  /* is_idempotent */
    1   /* has_side_effects */
};

TOOL_EXPORT const ToolInfo *tool_get_info(void) { return &info; }
TOOL_EXPORT tool_execute_fn tool_get_execute(void) { return sdl3_render; }

int SDL_main(int argc, char *argv[]) { (void)argc; (void)argv; return 0; }
