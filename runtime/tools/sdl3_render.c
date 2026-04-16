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

/* ========================= Widget System ========================= */

#define UI_MAX_NODES     256
#define UI_MAX_CHILDREN  64
#define UI_MAX_SIGNALS   128
#define UI_PI            3.14159265358979323846f

typedef enum {
    UI_W_NONE = 0,
    UI_W_VBOX, UI_W_HBOX, UI_W_MARGIN, UI_W_CENTER, UI_W_GRID,
    UI_W_LABEL, UI_W_COLORRECT, UI_W_HSEPARATOR, UI_W_VSEPARATOR,
    UI_W_BUTTON, UI_W_CHECKBOX, UI_W_PROGRESSBAR, UI_W_OPTIONBUTTON,
    UI_W_SPINBOX,
    UI_W_PANEL, UI_W_PANELCONTAINER, UI_W_SCROLL, UI_W_TABCONTAINER,
    UI_W_COUNT
} UiWidgetType;

typedef struct {
    SDL_FColor bg_color, border_color;
    int border_width, corner_radius, padding;
} StyleBoxFlat;

typedef struct {
    int id, active;
    UiWidgetType type;
    int parent_id;
    int children[UI_MAX_CHILDREN];
    int child_count;
    /* base props */
    float pos_x, pos_y, size_w, size_h, min_w, min_h;
    int flag_h, flag_v; /* FILL=0, EXPAND=1, SHRINK_CENTER=2 */
    int visible, z_index;
    /* widget-specific (flat fields) */
    char text[512]; int align, valign;
    char btn_text[256]; int pressed, disabled, toggle_mode;
    int checked;
    float range_min, range_max, range_value, range_step;
    char opt_items[8][64]; int opt_count, opt_selected;
    int separation;
    int margin_left, margin_right, margin_top, margin_bottom;
    int grid_columns, grid_hsep, grid_vsep;
    int current_tab;
    int h_scroll_mode, v_scroll_mode; float scroll_x, scroll_y;
    StyleBoxFlat style;
} UiNode;

static UiNode g_nodes[UI_MAX_NODES];
static int g_next_id = 1;
static int g_dirty = 1;

typedef struct {
    int node_id;
    char signal[64];
    char callback[128];
    int active;
} UiSignal;

static UiSignal g_signals[UI_MAX_SIGNALS];
static int g_signal_count = 0;

/* ---- Node Pool ---- */

static UiNode *ui_alloc_node(void) {
    for (int i = 0; i < UI_MAX_NODES; i++) {
        if (!g_nodes[i].active) {
            memset(&g_nodes[i], 0, sizeof(UiNode));
            g_nodes[i].id = g_next_id++;
            g_nodes[i].active = 1;
            g_nodes[i].visible = 1;
            g_nodes[i].parent_id = 0;
            return &g_nodes[i];
        }
    }
    return NULL;
}

static UiNode *ui_find_node(int id) {
    if (id <= 0) return NULL;
    for (int i = 0; i < UI_MAX_NODES; i++) {
        if (g_nodes[i].active && g_nodes[i].id == id)
            return &g_nodes[i];
    }
    return NULL;
}

static void ui_remove_child(UiNode *parent, int child_id) {
    if (!parent) return;
    for (int i = 0; i < parent->child_count; i++) {
        if (parent->children[i] == child_id) {
            memmove(&parent->children[i], &parent->children[i+1],
                    (parent->child_count - i - 1) * sizeof(int));
            parent->child_count--;
            return;
        }
    }
}

static void ui_add_child(UiNode *parent, UiNode *child) {
    if (!parent || !child) return;
    if (parent->child_count >= UI_MAX_CHILDREN) return;
    parent->children[parent->child_count++] = child->id;
    child->parent_id = parent->id;
}

static void ui_free_node(UiNode *node) {
    if (!node) return;
    while (node->child_count > 0) {
        int child_id = node->children[0];
        UiNode *child = ui_find_node(child_id);
        ui_remove_child(node, child_id);
        if (child) ui_free_node(child);
    }
    node->active = 0;
}

/* ---- Type & Property Parsing ---- */

static UiWidgetType parse_widget_type(const char *name) {
    if (!name) return UI_W_NONE;
    struct { const char *n; UiWidgetType t; } types[] = {
        {"VBoxContainer",UI_W_VBOX},{"HBoxContainer",UI_W_HBOX},
        {"MarginContainer",UI_W_MARGIN},{"CenterContainer",UI_W_CENTER},
        {"GridContainer",UI_W_GRID},{"Label",UI_W_LABEL},
        {"ColorRect",UI_W_COLORRECT},{"HSeparator",UI_W_HSEPARATOR},
        {"VSeparator",UI_W_VSEPARATOR},{"Button",UI_W_BUTTON},
        {"CheckBox",UI_W_CHECKBOX},{"ProgressBar",UI_W_PROGRESSBAR},
        {"OptionButton",UI_W_OPTIONBUTTON},{"SpinBox",UI_W_SPINBOX},
        {"Panel",UI_W_PANEL},{"PanelContainer",UI_W_PANELCONTAINER},
        {"ScrollContainer",UI_W_SCROLL},{"TabContainer",UI_W_TABCONTAINER},
        {NULL,UI_W_NONE}
    };
    for (int i = 0; types[i].n; i++)
        if (strcmp(name, types[i].n) == 0) return types[i].t;
    return UI_W_NONE;
}

static const char *widget_type_name(UiWidgetType t) {
    switch (t) {
    case UI_W_VBOX: return "VBoxContainer";   case UI_W_HBOX: return "HBoxContainer";
    case UI_W_MARGIN: return "MarginContainer"; case UI_W_CENTER: return "CenterContainer";
    case UI_W_GRID: return "GridContainer";   case UI_W_LABEL: return "Label";
    case UI_W_COLORRECT: return "ColorRect";  case UI_W_HSEPARATOR: return "HSeparator";
    case UI_W_VSEPARATOR: return "VSeparator"; case UI_W_BUTTON: return "Button";
    case UI_W_CHECKBOX: return "CheckBox";    case UI_W_PROGRESSBAR: return "ProgressBar";
    case UI_W_OPTIONBUTTON: return "OptionButton"; case UI_W_SPINBOX: return "SpinBox";
    case UI_W_PANEL: return "Panel";          case UI_W_PANELCONTAINER: return "PanelContainer";
    case UI_W_SCROLL: return "ScrollContainer"; case UI_W_TABCONTAINER: return "TabContainer";
    default: return "Unknown";
    }
}

static void ui_apply_default_style(UiNode *node) {
    if (!node) return;
    node->style.bg_color     = (SDL_FColor){0.18f,0.18f,0.22f,1.0f};
    node->style.border_color = (SDL_FColor){0.38f,0.38f,0.45f,1.0f};
    node->style.border_width = 1;
    node->style.corner_radius= 4;
    node->style.padding      = 4;
    switch (node->type) {
    case UI_W_BUTTON:
        node->style.bg_color     = (SDL_FColor){0.25f,0.42f,0.72f,1.0f};
        node->style.border_color = (SDL_FColor){0.35f,0.52f,0.82f,1.0f};
        node->style.corner_radius= 6; node->style.padding = 8; break;
    case UI_W_LABEL:
        node->style.bg_color = (SDL_FColor){0,0,0,0};
        node->style.border_width = 0; break;
    case UI_W_PANEL: case UI_W_PANELCONTAINER:
        node->style.bg_color     = (SDL_FColor){0.24f,0.24f,0.30f,1.0f};
        node->style.border_color = (SDL_FColor){0.42f,0.42f,0.50f,1.0f};
        node->style.padding = 8; break;
    case UI_W_CHECKBOX:
        node->style.bg_color = (SDL_FColor){0,0,0,0};
        node->style.border_width = 0; break;
    case UI_W_PROGRESSBAR:
        node->style.bg_color     = (SDL_FColor){0.16f,0.16f,0.20f,1.0f};
        node->style.border_color = (SDL_FColor){0.38f,0.38f,0.45f,1.0f}; break;
    case UI_W_OPTIONBUTTON:
        node->style.bg_color = (SDL_FColor){0.20f,0.20f,0.25f,1.0f};
        node->style.corner_radius = 4; node->style.padding = 6; break;
    case UI_W_SPINBOX:
        node->style.bg_color = (SDL_FColor){0.20f,0.20f,0.25f,1.0f};
        node->style.corner_radius = 4; node->style.padding = 4; break;
    case UI_W_TABCONTAINER:
        node->style.bg_color     = (SDL_FColor){0.12f,0.12f,0.15f,1.0f};
        node->style.border_color = (SDL_FColor){0.25f,0.25f,0.30f,1.0f}; break;
    default: break;
    }
}

static void ui_parse_props(UiNode *node, cJSON *props) {
    if (!node || !props) return;
    const char *s;
    if ((s = get_str(props,"text","label")))
        { strncpy(node->text,s,sizeof(node->text)-1); node->text[sizeof(node->text)-1]=0; }
    if ((s = get_str(props,"button_text","btn_text")))
        { strncpy(node->btn_text,s,sizeof(node->btn_text)-1); node->btn_text[sizeof(node->btn_text)-1]=0; }
    node->align   = (int)get_num(props,"align","horizontal_alignment",node->align);
    node->valign  = (int)get_num(props,"valign","vertical_alignment",node->valign);
    node->pressed = get_bool(props,"pressed",NULL,node->pressed);
    node->disabled= get_bool(props,"disabled",NULL,node->disabled);
    node->toggle_mode = get_bool(props,"toggle_mode",NULL,node->toggle_mode);
    node->checked = get_bool(props,"checked",NULL,node->checked);
    node->visible = get_bool(props,"visible",NULL,1);
    node->range_min   = (float)get_num(props,"range_min","min_value",node->range_min);
    node->range_max   = (float)get_num(props,"range_max","max_value",node->range_max);
    node->range_value = (float)get_num(props,"range_value","value",node->range_value);
    node->range_step  = (float)get_num(props,"step",NULL,node->range_step);
    node->separation  = (int)get_num(props,"separation","gap",node->separation);
    node->margin_left  = (int)get_num(props,"margin_left","marginLeft",node->margin_left);
    node->margin_right = (int)get_num(props,"margin_right","marginRight",node->margin_right);
    node->margin_top   = (int)get_num(props,"margin_top","marginTop",node->margin_top);
    node->margin_bottom= (int)get_num(props,"margin_bottom","marginBottom",node->margin_bottom);
    node->grid_columns = (int)get_num(props,"columns","grid_columns",node->grid_columns);
    node->grid_hsep    = (int)get_num(props,"h_separation","grid_hsep",node->grid_hsep);
    node->grid_vsep    = (int)get_num(props,"v_separation","grid_vsep",node->grid_vsep);
    node->current_tab  = (int)get_num(props,"current_tab","tab",node->current_tab);
    node->opt_selected = (int)get_num(props,"selected",NULL,node->opt_selected);
    node->size_w = (float)get_num(props,"size_w","width",node->size_w);
    node->size_h = (float)get_num(props,"size_h","height",node->size_h);
    node->min_w  = (float)get_num(props,"min_w","minimum_width",node->min_w);
    node->min_h  = (float)get_num(props,"min_h","minimum_height",node->min_h);
    node->flag_h = (int)get_num(props,"size_flags_horizontal","flag_h",node->flag_h);
    node->flag_v = (int)get_num(props,"size_flags_vertical","flag_v",node->flag_v);
    node->z_index= (int)get_num(props,"z_index",NULL,node->z_index);
    cJSON *opt_arr = cJSON_GetObjectItem(props,"items");
    if (cJSON_IsArray(opt_arr)) {
        node->opt_count = 0;
        int n = cJSON_GetArraySize(opt_arr);
        if (n > 8) n = 8;
        for (int i = 0; i < n; i++) {
            cJSON *it = cJSON_GetArrayItem(opt_arr, i);
            if (cJSON_IsString(it)) {
                strncpy(node->opt_items[node->opt_count], it->valuestring, 63);
                node->opt_items[node->opt_count][63] = 0;
                node->opt_count++;
            }
        }
    }
}

/* ---- Layout Engine ---- */

static float text_width_est(const char *text) {
    if (!text || !text[0]) return 0;
    return (float)strlen(text) * 7.0f;
}

static int text_line_count(const char *text) {
    if (!text || !text[0]) return 0;
    int lines = 1;
    for (const char *p = text; *p; p++) if (*p == '\n') lines++;
    return lines;
}

/* Bottom-up: compute minimum size for each node */
static void ui_compute_min_size(UiNode *node) {
    if (!node || !node->active) return;
    float tw = 0, th = 0;
    int i;
    switch (node->type) {
    case UI_W_LABEL:
        tw = text_width_est(node->text) + 4;
        th = (float)text_line_count(node->text) * 14.0f + 4;
        break;
    case UI_W_BUTTON: {
        const char *bt = node->btn_text[0] ? node->btn_text : node->text;
        tw = text_width_est(bt) + (float)(node->style.padding * 2) + 16;
        th = 14.0f + (float)(node->style.padding * 2) + 8;
        break;
    }
    case UI_W_CHECKBOX:
        tw = 16 + text_width_est(node->text) + 4; th = 20; break;
    case UI_W_PROGRESSBAR: tw = 100; th = 20; break;
    case UI_W_COLORRECT:
        tw = node->min_w > 0 ? node->min_w : 10;
        th = node->min_h > 0 ? node->min_h : 10; break;
    case UI_W_HSEPARATOR: tw = 4; th = 4; break;
    case UI_W_VSEPARATOR: tw = 4; th = 4; break;
    case UI_W_OPTIONBUTTON:
        if (node->opt_count > 0 && node->opt_selected >= 0 && node->opt_selected < node->opt_count)
            tw = text_width_est(node->opt_items[node->opt_selected]) + 32;
        else tw = 80;
        th = 28; break;
    case UI_W_SPINBOX: tw = 80; th = 28; break;
    case UI_W_PANEL: tw = 4; th = 4; break;
    case UI_W_VBOX:
        for (i = 0; i < node->child_count; i++)
            ui_compute_min_size(ui_find_node(node->children[i]));
        for (i = 0; i < node->child_count; i++) {
            UiNode *c = ui_find_node(node->children[i]);
            if (!c) continue;
            if (c->min_w > tw) tw = c->min_w;
            th += c->min_h;
        }
        if (node->child_count > 1) th += (float)(node->child_count - 1) * (float)node->separation;
        node->min_w = tw; node->min_h = th; return;
    case UI_W_HBOX:
        for (i = 0; i < node->child_count; i++)
            ui_compute_min_size(ui_find_node(node->children[i]));
        for (i = 0; i < node->child_count; i++) {
            UiNode *c = ui_find_node(node->children[i]);
            if (!c) continue;
            tw += c->min_w;
            if (c->min_h > th) th = c->min_h;
        }
        if (node->child_count > 1) tw += (float)(node->child_count - 1) * (float)node->separation;
        node->min_w = tw; node->min_h = th; return;
    case UI_W_GRID: {
        for (i = 0; i < node->child_count; i++)
            ui_compute_min_size(ui_find_node(node->children[i]));
        int cols = node->grid_columns > 0 ? node->grid_columns : 1;
        int rows = (node->child_count + cols - 1) / cols;
        float mcw = 0, mch = 0;
        for (i = 0; i < node->child_count; i++) {
            UiNode *c = ui_find_node(node->children[i]);
            if (!c) continue;
            if (c->min_w > mcw) mcw = c->min_w;
            if (c->min_h > mch) mch = c->min_h;
        }
        tw = mcw * cols + (float)(cols - 1) * (float)node->grid_hsep;
        th = mch * rows + (float)(rows - 1) * (float)node->grid_vsep;
        break;
    }
    case UI_W_MARGIN:
        for (i = 0; i < node->child_count; i++)
            ui_compute_min_size(ui_find_node(node->children[i]));
        for (i = 0; i < node->child_count; i++) {
            UiNode *c = ui_find_node(node->children[i]);
            if (!c) continue;
            if (c->min_w > tw) tw = c->min_w;
            if (c->min_h > th) th = c->min_h;
        }
        tw += (float)(node->margin_left + node->margin_right);
        th += (float)(node->margin_top + node->margin_bottom);
        node->min_w = tw; node->min_h = th; return;
    case UI_W_CENTER:
        for (i = 0; i < node->child_count; i++)
            ui_compute_min_size(ui_find_node(node->children[i]));
        for (i = 0; i < node->child_count; i++) {
            UiNode *c = ui_find_node(node->children[i]);
            if (!c) continue;
            if (c->min_w > tw) tw = c->min_w;
            if (c->min_h > th) th = c->min_h;
        }
        node->min_w = tw; node->min_h = th; return;
    case UI_W_PANELCONTAINER:
        for (i = 0; i < node->child_count; i++)
            ui_compute_min_size(ui_find_node(node->children[i]));
        for (i = 0; i < node->child_count; i++) {
            UiNode *c = ui_find_node(node->children[i]);
            if (!c) continue;
            if (c->min_w > tw) tw = c->min_w;
            if (c->min_h > th) th = c->min_h;
        }
        tw += (float)(node->style.padding * 2);
        th += (float)(node->style.padding * 2);
        node->min_w = tw; node->min_h = th; return;
    case UI_W_TABCONTAINER:
        for (i = 0; i < node->child_count; i++)
            ui_compute_min_size(ui_find_node(node->children[i]));
        th = 28;
        for (i = 0; i < node->child_count; i++) {
            UiNode *c = ui_find_node(node->children[i]);
            if (!c) continue;
            if (c->min_w > tw) tw = c->min_w;
            th += c->min_h;
        }
        node->min_w = tw; node->min_h = th; return;
    case UI_W_SCROLL:
        for (i = 0; i < node->child_count; i++)
            ui_compute_min_size(ui_find_node(node->children[i]));
        node->min_w = 50; node->min_h = 50; return;
    default:
        tw = node->min_w > 0 ? node->min_w : 10;
        th = node->min_h > 0 ? node->min_h : 10; break;
    }
    if (node->min_w < tw) node->min_w = tw;
    if (node->min_h < th) node->min_h = th;
}

/* Top-down: assign positions and sizes */
static void ui_compute_layout(UiNode *node, float x, float y, float w, float h) {
    if (!node || !node->active) return;
    node->pos_x = x; node->pos_y = y;
    node->size_w = w; node->size_h = h;
    int i, expand_count;
    float remaining, sep_total;

    switch (node->type) {
    case UI_W_VBOX: {
        sep_total = node->child_count > 1
            ? (float)(node->child_count - 1) * (float)node->separation : 0;
        expand_count = 0; float fixed_h = 0;
        for (i = 0; i < node->child_count; i++) {
            UiNode *c = ui_find_node(node->children[i]); if (!c) continue;
            if (c->flag_v == 1) expand_count++; else fixed_h += c->min_h;
        }
        remaining = h - fixed_h - sep_total;
        float expand_h = expand_count > 0 ? remaining / (float)expand_count : 0;
        if (expand_h < 0) expand_h = 0;
        float cy = y;
        for (i = 0; i < node->child_count; i++) {
            UiNode *c = ui_find_node(node->children[i]); if (!c) continue;
            float cw = (c->flag_h == 2) ? c->min_w : w; /* FILL/EXPAND=full width, SHRINK=min */
            float ch = (c->flag_v == 1) ? expand_h : c->min_h;
            float cx = x;
            if (c->flag_h == 2) cx = x + (w - cw) / 2;
            ui_compute_layout(c, cx, cy, cw, ch);
            cy += ch + (float)node->separation;
        }
        break;
    }
    case UI_W_HBOX: {
        sep_total = node->child_count > 1
            ? (float)(node->child_count - 1) * (float)node->separation : 0;
        expand_count = 0; float fixed_w = 0;
        for (i = 0; i < node->child_count; i++) {
            UiNode *c = ui_find_node(node->children[i]); if (!c) continue;
            if (c->flag_h == 1) expand_count++; else fixed_w += c->min_w;
        }
        remaining = w - fixed_w - sep_total;
        float expand_w = expand_count > 0 ? remaining / (float)expand_count : 0;
        if (expand_w < 0) expand_w = 0;
        float cx = x;
        for (i = 0; i < node->child_count; i++) {
            UiNode *c = ui_find_node(node->children[i]); if (!c) continue;
            float cw = (c->flag_h == 1) ? expand_w : c->min_w;
            float ch = (c->flag_v == 2) ? c->min_h : h; /* FILL/EXPAND=full height, SHRINK=min */
            float cy2 = y;
            if (c->flag_v == 2) cy2 = y + (h - ch) / 2;
            ui_compute_layout(c, cx, cy2, cw, ch);
            cx += cw + (float)node->separation;
        }
        break;
    }
    case UI_W_GRID: {
        int cols = node->grid_columns > 0 ? node->grid_columns : 1;
        int rows = (node->child_count + cols - 1) / cols;
        if (rows < 1) rows = 1;
        float cell_w = (w - (float)(cols - 1) * (float)node->grid_hsep) / (float)cols;
        float cell_h = (h - (float)(rows - 1) * (float)node->grid_vsep) / (float)rows;
        if (cell_h < 14) cell_h = 14;
        for (i = 0; i < node->child_count; i++) {
            int col = i % cols, row = i / cols;
            UiNode *c = ui_find_node(node->children[i]); if (!c) continue;
            ui_compute_layout(c,
                x + (float)col * (cell_w + (float)node->grid_hsep),
                y + (float)row * (cell_h + (float)node->grid_vsep),
                cell_w, cell_h);
        }
        break;
    }
    case UI_W_MARGIN: {
        float ix = x + (float)node->margin_left;
        float iy = y + (float)node->margin_top;
        float iw = w - (float)(node->margin_left + node->margin_right);
        float ih = h - (float)(node->margin_top + node->margin_bottom);
        if (iw < 0) iw = 0; if (ih < 0) ih = 0;
        for (i = 0; i < node->child_count; i++)
            ui_compute_layout(ui_find_node(node->children[i]), ix, iy, iw, ih);
        break;
    }
    case UI_W_CENTER:
        for (i = 0; i < node->child_count; i++) {
            UiNode *c = ui_find_node(node->children[i]); if (!c) continue;
            float cw = (c->flag_h == 1) ? w : c->min_w;
            float ch = (c->flag_v == 1) ? h : c->min_h;
            ui_compute_layout(c, x + (w - cw)/2, y + (h - ch)/2, cw, ch);
        }
        break;
    case UI_W_PANELCONTAINER: {
        float pad = (float)node->style.padding;
        for (i = 0; i < node->child_count; i++)
            ui_compute_layout(ui_find_node(node->children[i]),
                             x + pad, y + pad, w - pad*2, h - pad*2);
        break;
    }
    case UI_W_TABCONTAINER: {
        float tab_h = 28;
        for (i = 0; i < node->child_count; i++) {
            UiNode *c = ui_find_node(node->children[i]); if (!c) continue;
            ui_compute_layout(c, x, y + tab_h, w, h - tab_h);
            c->visible = (i == node->current_tab) ? 1 : 0;
        }
        break;
    }
    case UI_W_SCROLL:
        for (i = 0; i < node->child_count; i++) {
            UiNode *c = ui_find_node(node->children[i]); if (!c) continue;
            float cw = c->min_w > w ? c->min_w : w;
            float ch = c->min_h > h ? c->min_h : h;
            ui_compute_layout(c, x - node->scroll_x, y - node->scroll_y, cw, ch);
        }
        break;
    default: break;
    }
}

/* ---- Rendering ---- */

static void set_color(SDL_Renderer *r, SDL_FColor c) {
    SDL_SetRenderDrawColorFloat(r, c.r, c.g, c.b, c.a);
}

static void draw_fill_rect(SDL_Renderer *r, float x, float y, float w, float h) {
    SDL_FRect rect = {x, y, w, h};
    SDL_RenderFillRect(r, &rect);
}

static void draw_rounded_rect(SDL_Renderer *renderer, float x, float y, float w, float h, float r, int fill) {
    if (r <= 0) {
        SDL_FRect rect = {x, y, w, h};
        if (fill) SDL_RenderFillRect(renderer, &rect);
        else SDL_RenderRect(renderer, &rect);
        return;
    }
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    SDL_FColor color;
    SDL_GetRenderDrawColorFloat(renderer, &color.r, &color.g, &color.b, &color.a);

    float corners[4][3] = {
        {x+r, y+r, UI_PI},           {x+w-r, y+r, 3*UI_PI/2},
        {x+w-r, y+h-r, 0},          {x+r, y+h-r, UI_PI/2}
    };
    int segs = (int)(r * 0.5f);
    if (segs < 3) segs = 3;
    if (segs > 12) segs = 12;

    if (fill) {
        SDL_FRect rects[3] = {
            {x+r, y, w-2*r, h}, {x, y+r, r, h-2*r}, {x+w-r, y+r, r, h-2*r}
        };
        for (int i = 0; i < 3; i++) SDL_RenderFillRect(renderer, &rects[i]);
        for (int c = 0; c < 4; c++) {
            for (int s = 0; s < segs; s++) {
                float a1 = corners[c][2] + (float)s * (UI_PI/2) / (float)segs;
                float a2 = corners[c][2] + (float)(s+1) * (UI_PI/2) / (float)segs;
                SDL_Vertex v[3];
                v[0].position = (SDL_FPoint){corners[c][0], corners[c][1]};
                v[1].position = (SDL_FPoint){corners[c][0]+r*cosf(a1), corners[c][1]+r*sinf(a1)};
                v[2].position = (SDL_FPoint){corners[c][0]+r*cosf(a2), corners[c][1]+r*sinf(a2)};
                v[0].color = v[1].color = v[2].color = color;
                v[0].tex_coord = v[1].tex_coord = v[2].tex_coord = (SDL_FPoint){0,0};
                SDL_RenderGeometry(renderer, NULL, v, 3, NULL, 0);
            }
        }
    } else {
        SDL_RenderLine(renderer, x+r, y, x+w-r, y);
        SDL_RenderLine(renderer, x+r, y+h, x+w-r, y+h);
        SDL_RenderLine(renderer, x, y+r, x, y+h-r);
        SDL_RenderLine(renderer, x+w, y+r, x+w, y+h-r);
        for (int c = 0; c < 4; c++) {
            SDL_FPoint pts[14];
            for (int s = 0; s <= segs; s++) {
                float a = corners[c][2] + (float)s * (UI_PI/2) / (float)segs;
                pts[s].x = corners[c][0] + r * cosf(a);
                pts[s].y = corners[c][1] + r * sinf(a);
            }
            SDL_RenderLines(renderer, pts, segs + 1);
        }
    }
}

/* Main recursive draw function */
static void ui_draw_node(SDL_Renderer *renderer, UiNode *node) {
    if (!node || !node->active || !node->visible) return;
    SDL_FColor text_col = {0.93f,0.93f,0.95f,1.0f};
    SDL_FColor dim_text = {0.60f,0.60f,0.65f,1.0f};
    int i;
    switch (node->type) {
    case UI_W_LABEL: {
        set_color(renderer, text_col);
        float tw = text_width_est(node->text);
        float tx = node->pos_x, ty = node->pos_y;
        if (node->align == 1) tx = node->pos_x + (node->size_w - tw) / 2;
        else if (node->align == 2) tx = node->pos_x + node->size_w - tw - 2;
        if (node->valign == 1) ty = node->pos_y + (node->size_h - 14.0f) / 2;
        else if (node->valign == 2) ty = node->pos_y + node->size_h - 16;
        SDL_RenderDebugText(renderer, tx, ty, node->text);
        break;
    }
    case UI_W_BUTTON: {
        set_color(renderer, node->style.bg_color);
        draw_rounded_rect(renderer, node->pos_x, node->pos_y, node->size_w, node->size_h,
                          (float)node->style.corner_radius, 1);
        if (node->style.border_width > 0) {
            set_color(renderer, node->style.border_color);
            draw_rounded_rect(renderer, node->pos_x, node->pos_y, node->size_w, node->size_h,
                              (float)node->style.corner_radius, 0);
        }
        const char *bt = node->btn_text[0] ? node->btn_text : node->text;
        set_color(renderer, text_col);
        SDL_RenderDebugText(renderer,
            node->pos_x + (node->size_w - text_width_est(bt)) / 2,
            node->pos_y + (node->size_h - 14) / 2, bt);
        break;
    }
    case UI_W_CHECKBOX: {
        float bx = node->pos_x, by = node->pos_y + (node->size_h - 14) / 2;
        SDL_FRect box = {bx, by, 14, 14};
        set_color(renderer, (SDL_FColor){0.35f,0.35f,0.4f,1.0f});
        SDL_RenderFillRect(renderer, &box);
        set_color(renderer, (SDL_FColor){0.5f,0.5f,0.55f,1.0f});
        SDL_RenderRect(renderer, &box);
        if (node->checked) {
            set_color(renderer, (SDL_FColor){0.4f,0.75f,0.4f,1.0f});
            SDL_RenderLine(renderer, bx+3,by+7, bx+6,by+11);
            SDL_RenderLine(renderer, bx+6,by+11, bx+11,by+3);
        }
        set_color(renderer, text_col);
        SDL_RenderDebugText(renderer, bx+20, by, node->text);
        break;
    }
    case UI_W_PROGRESSBAR: {
        set_color(renderer, node->style.bg_color);
        draw_fill_rect(renderer, node->pos_x, node->pos_y, node->size_w, node->size_h);
        float range = node->range_max - node->range_min;
        float ratio = range > 0 ? (node->range_value - node->range_min) / range : 0;
        if (ratio < 0) ratio = 0; if (ratio > 1) ratio = 1;
        set_color(renderer, (SDL_FColor){0.25f,0.60f,0.35f,1.0f});
        draw_fill_rect(renderer, node->pos_x, node->pos_y, node->size_w * ratio, node->size_h);
        set_color(renderer, node->style.border_color);
        { SDL_FRect br = {node->pos_x, node->pos_y, node->size_w, node->size_h};
          SDL_RenderRect(renderer, &br); }
        char vbuf[32]; snprintf(vbuf, sizeof(vbuf), "%.0f%%", ratio * 100);
        set_color(renderer, text_col);
        SDL_RenderDebugText(renderer, node->pos_x + (node->size_w - text_width_est(vbuf))/2,
                           node->pos_y + (node->size_h - 14)/2, vbuf);
        break;
    }
    case UI_W_COLORRECT:
        set_color(renderer, node->style.bg_color);
        draw_fill_rect(renderer, node->pos_x, node->pos_y, node->size_w, node->size_h);
        break;
    case UI_W_HSEPARATOR:
        set_color(renderer, (SDL_FColor){0.45f,0.45f,0.52f,1.0f});
        SDL_RenderLine(renderer, node->pos_x, node->pos_y + node->size_h/2,
                       node->pos_x + node->size_w, node->pos_y + node->size_h/2);
        break;
    case UI_W_VSEPARATOR:
        set_color(renderer, (SDL_FColor){0.45f,0.45f,0.52f,1.0f});
        SDL_RenderLine(renderer, node->pos_x + node->size_w/2, node->pos_y,
                       node->pos_x + node->size_w/2, node->pos_y + node->size_h);
        break;
    case UI_W_PANEL:
        set_color(renderer, node->style.bg_color);
        draw_fill_rect(renderer, node->pos_x, node->pos_y, node->size_w, node->size_h);
        if (node->style.border_width > 0) {
            set_color(renderer, node->style.border_color);
            { SDL_FRect pr = {node->pos_x, node->pos_y, node->size_w, node->size_h};
              SDL_RenderRect(renderer, &pr); }
        }
        break;
    case UI_W_PANELCONTAINER:
        set_color(renderer, node->style.bg_color);
        draw_fill_rect(renderer, node->pos_x, node->pos_y, node->size_w, node->size_h);
        if (node->style.border_width > 0) {
            set_color(renderer, node->style.border_color);
            draw_rounded_rect(renderer, node->pos_x, node->pos_y,
                              node->size_w, node->size_h,
                              (float)node->style.corner_radius, 0);
        }
        for (i = 0; i < node->child_count; i++)
            ui_draw_node(renderer, ui_find_node(node->children[i]));
        return;
    case UI_W_OPTIONBUTTON: {
        set_color(renderer, node->style.bg_color);
        draw_rounded_rect(renderer, node->pos_x, node->pos_y, node->size_w, node->size_h,
                          (float)node->style.corner_radius, 1);
        set_color(renderer, node->style.border_color);
        draw_rounded_rect(renderer, node->pos_x, node->pos_y, node->size_w, node->size_h,
                          (float)node->style.corner_radius, 0);
        const char *sel = (node->opt_count > 0 && node->opt_selected >= 0 &&
                          node->opt_selected < node->opt_count)
                          ? node->opt_items[node->opt_selected] : "";
        set_color(renderer, text_col);
        SDL_RenderDebugText(renderer, node->pos_x + 8,
                           node->pos_y + (node->size_h - 14)/2, sel);
        float ax = node->pos_x + node->size_w - 16, ay = node->pos_y + node->size_h/2;
        set_color(renderer, dim_text);
        SDL_RenderLine(renderer, ax-4,ay-3, ax,ay+3);
        SDL_RenderLine(renderer, ax,ay+3, ax+4,ay-3);
        break;
    }
    case UI_W_SPINBOX: {
        set_color(renderer, node->style.bg_color);
        draw_fill_rect(renderer, node->pos_x, node->pos_y, node->size_w, node->size_h);
        set_color(renderer, node->style.border_color);
        { SDL_FRect sr = {node->pos_x, node->pos_y, node->size_w, node->size_h};
          SDL_RenderRect(renderer, &sr); }
        char vbuf2[32];
        if (node->range_step >= 1) snprintf(vbuf2, sizeof(vbuf2), "%.0f", node->range_value);
        else snprintf(vbuf2, sizeof(vbuf2), "%.1f", node->range_value);
        set_color(renderer, text_col);
        SDL_RenderDebugText(renderer,
            node->pos_x + (node->size_w - text_width_est(vbuf2))/2,
            node->pos_y + (node->size_h - 14)/2, vbuf2);
        float bw = 20;
        set_color(renderer, (SDL_FColor){0.28f,0.28f,0.33f,1.0f});
        draw_fill_rect(renderer, node->pos_x, node->pos_y, bw, node->size_h);
        draw_fill_rect(renderer, node->pos_x+node->size_w-bw, node->pos_y, bw, node->size_h);
        set_color(renderer, text_col);
        SDL_RenderDebugText(renderer, node->pos_x+6, node->pos_y+(node->size_h-14)/2, "-");
        SDL_RenderDebugText(renderer, node->pos_x+node->size_w-14, node->pos_y+(node->size_h-14)/2, "+");
        break;
    }
    case UI_W_TABCONTAINER: {
        float tab_h = 28;
        set_color(renderer, (SDL_FColor){0.10f,0.10f,0.13f,1.0f});
        draw_fill_rect(renderer, node->pos_x, node->pos_y, node->size_w, tab_h);
        float tab_w = node->size_w / (node->child_count > 0 ? (float)node->child_count : 1);
        for (i = 0; i < node->child_count; i++) {
            float tx = node->pos_x + (float)i * tab_w;
            if (i == node->current_tab) {
                set_color(renderer, node->style.bg_color);
                draw_fill_rect(renderer, tx, node->pos_y, tab_w, tab_h);
                set_color(renderer, text_col);
            } else {
                set_color(renderer, dim_text);
            }
            UiNode *child = ui_find_node(node->children[i]);
            char tab_label[32];
            if (child && child->text[0]) snprintf(tab_label, sizeof(tab_label), "%s", child->text);
            else snprintf(tab_label, sizeof(tab_label), "Tab %d", i+1);
            SDL_RenderDebugText(renderer, tx + 8, node->pos_y + 7, tab_label);
        }
        set_color(renderer, node->style.bg_color);
        draw_fill_rect(renderer, node->pos_x, node->pos_y + tab_h, node->size_w, node->size_h - tab_h);
        for (i = 0; i < node->child_count; i++) {
            UiNode *c = ui_find_node(node->children[i]);
            if (c && c->visible) ui_draw_node(renderer, c);
        }
        return;
    }
    case UI_W_SCROLL: {
        SDL_Rect clip = {(int)node->pos_x, (int)node->pos_y, (int)node->size_w, (int)node->size_h};
        SDL_SetRenderClipRect(renderer, &clip);
        for (i = 0; i < node->child_count; i++)
            ui_draw_node(renderer, ui_find_node(node->children[i]));
        SDL_SetRenderClipRect(renderer, NULL);
        return;
    }
    default: break;
    }
    /* Default: draw children for layout containers with no visual */
    for (i = 0; i < node->child_count; i++)
        ui_draw_node(renderer, ui_find_node(node->children[i]));
}

/* ---- Action Handlers ---- */

static cJSON *ui_node_to_json(UiNode *node) {
    if (!node) return cJSON_CreateNull();
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddNumberToObject(obj, "id", node->id);
    cJSON_AddStringToObject(obj, "type", widget_type_name(node->type));
    if (node->parent_id > 0) cJSON_AddNumberToObject(obj, "parent_id", node->parent_id);
    cJSON_AddBoolToObject(obj, "visible", node->visible);
    cJSON_AddNumberToObject(obj, "x", (double)node->pos_x);
    cJSON_AddNumberToObject(obj, "y", (double)node->pos_y);
    cJSON_AddNumberToObject(obj, "w", (double)node->size_w);
    cJSON_AddNumberToObject(obj, "h", (double)node->size_h);
    if (node->text[0]) cJSON_AddStringToObject(obj, "text", node->text);
    if (node->child_count > 0) {
        cJSON *children = cJSON_CreateArray();
        for (int i = 0; i < node->child_count; i++) {
            UiNode *c = ui_find_node(node->children[i]);
            if (c) cJSON_AddItemToArray(children, ui_node_to_json(c));
        }
        cJSON_AddItemToObject(obj, "children", children);
    }
    return obj;
}

static cJSON *handle_ui_create(cJSON *args, const char *workspace, char **error) {
    const char *type_str = get_str(args, "type", "widget");
    if (!type_str) TOOL_ERROR("ui_create requires 'type' argument");
    UiWidgetType type = parse_widget_type(type_str);
    if (type == UI_W_NONE) {
        char buf[256]; snprintf(buf, sizeof(buf), "unknown widget type '%s'", type_str);
        TOOL_ERROR(buf);
    }
    UiNode *node = ui_alloc_node();
    if (!node) TOOL_ERROR("node pool full (max 256)");
    node->type = type;
    ui_apply_default_style(node);
    int parent_id = (int)get_num(args, "parent_id", "parent", 0);
    if (parent_id > 0) {
        UiNode *parent = ui_find_node(parent_id);
        if (!parent) { ui_free_node(node); TOOL_ERROR("parent node not found"); }
        ui_add_child(parent, node);
    }
    cJSON *props = cJSON_GetObjectItem(args, "props");
    if (!props) props = args;
    ui_parse_props(node, props);
    if (type == UI_W_GRID && node->grid_columns <= 0) node->grid_columns = 2;
    if (type == UI_W_PROGRESSBAR && node->range_max == 0)
        { node->range_min = 0; node->range_max = 100; node->range_value = 0; }
    g_dirty = 1;
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "status", "ok");
    cJSON_AddNumberToObject(r, "node_id", node->id);
    return r;
}

static cJSON *handle_ui_set_prop(cJSON *args, const char *workspace, char **error) {
    int node_id = (int)get_num(args, "node_id", "id", 0);
    if (node_id <= 0) TOOL_ERROR("ui_set_prop requires 'node_id'");
    UiNode *node = ui_find_node(node_id);
    if (!node) TOOL_ERROR("node not found");
    ui_parse_props(node, args);
    g_dirty = 1;
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "status", "ok");
    return r;
}

static cJSON *handle_ui_connect(cJSON *args, const char *workspace, char **error) {
    int node_id = (int)get_num(args, "node_id", "id", 0);
    const char *signal = get_str(args, "signal", "event");
    const char *callback = get_str(args, "callback", "handler");
    if (node_id <= 0 || !signal || !callback)
        TOOL_ERROR("ui_connect requires 'node_id', 'signal', 'callback'");
    if (!ui_find_node(node_id)) TOOL_ERROR("node not found");
    if (g_signal_count >= UI_MAX_SIGNALS) TOOL_ERROR("signal pool full");
    UiSignal *s = &g_signals[g_signal_count++];
    s->node_id = node_id; s->active = 1;
    strncpy(s->signal, signal, sizeof(s->signal) - 1);
    strncpy(s->callback, callback, sizeof(s->callback) - 1);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "status", "ok");
    return r;
}

static cJSON *handle_ui_destroy(cJSON *args, const char *workspace, char **error) {
    int node_id = (int)get_num(args, "node_id", "id", 0);
    if (node_id <= 0) TOOL_ERROR("ui_destroy requires 'node_id'");
    UiNode *node = ui_find_node(node_id);
    if (!node) TOOL_ERROR("node not found");
    if (node->parent_id > 0) {
        UiNode *parent = ui_find_node(node->parent_id);
        ui_remove_child(parent, node_id);
    }
    ui_free_node(node);
    g_dirty = 1;
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "status", "ok");
    return r;
}

static cJSON *handle_ui_clear(cJSON *args, const char *workspace, char **error) {
    memset(g_nodes, 0, sizeof(g_nodes));
    g_next_id = 1; g_dirty = 1;
    memset(g_signals, 0, sizeof(g_signals));
    g_signal_count = 0;
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "status", "ok");
    return r;
}

static cJSON *handle_ui_get_tree(cJSON *args, const char *workspace, char **error) {
    cJSON *r = cJSON_CreateObject();
    cJSON *roots = cJSON_CreateArray();
    int node_count = 0;
    for (int i = 0; i < UI_MAX_NODES; i++) {
        if (g_nodes[i].active) {
            node_count++;
            if (g_nodes[i].parent_id == 0)
                cJSON_AddItemToArray(roots, ui_node_to_json(&g_nodes[i]));
        }
    }
    cJSON_AddItemToObject(r, "tree", roots);
    cJSON_AddNumberToObject(r, "total_nodes", node_count);
    cJSON_AddNumberToObject(r, "signal_count", g_signal_count);
    return r;
}

static cJSON *handle_ui_render_frame(cJSON *args, const char *workspace, char **error) {
    const char *path_str = find_path(args);
    if (!path_str) TOOL_ERROR("ui_render_frame requires 'path' argument");
    int width = (int)get_num(args, "width", "w", 640);
    int height = (int)get_num(args, "height", "h", 480);

    char path_buf[512];
    strncpy(path_buf, path_str, sizeof(path_buf)-1); path_buf[sizeof(path_buf)-1] = 0;
    tool_normalize_path(path_buf);
    char *full_path = sandbox_resolve_path(path_buf, workspace);
    if (!full_path) TOOL_ERROR("path traversal blocked or invalid path");

    /* Find root nodes */
    int root_count = 0; UiNode *roots[16];
    for (int i = 0; i < UI_MAX_NODES && root_count < 16; i++) {
        if (g_nodes[i].active && g_nodes[i].parent_id == 0)
            roots[root_count++] = &g_nodes[i];
    }
    if (root_count == 0) { free(full_path); TOOL_ERROR("no widgets in tree — use ui_create first"); }

    /* Layout pass */
    float cw = (float)width, ch = (float)height;
    if (root_count == 1) {
        ui_compute_min_size(roots[0]);
        ui_compute_layout(roots[0], 0, 0, cw, ch);
    } else {
        for (int i = 0; i < root_count; i++) ui_compute_min_size(roots[i]);
        float total_h = 0;
        for (int i = 0; i < root_count; i++) total_h += roots[i]->min_h;
        float scale = total_h > ch ? ch / total_h : 1.0f;
        float cy = 0;
        for (int i = 0; i < root_count; i++) {
            float rh = roots[i]->min_h * scale;
            ui_compute_layout(roots[i], 0, cy, cw, rh);
            cy += rh;
        }
    }

    /* Offscreen render */
    int was_init = SDL_WasInit(SDL_INIT_VIDEO);
    if (!was_init) {
        if (!SDL_Init(SDL_INIT_VIDEO)) {
            char buf[256]; snprintf(buf, sizeof(buf), "SDL3 init error: %s", SDL_GetError());
            free(full_path); *error = _strdup(buf); return NULL;
        }
    }
    SDL_Surface *surface = SDL_CreateSurface(width, height, SDL_PIXELFORMAT_RGBA8888);
    if (!surface) {
        char buf[256]; snprintf(buf, sizeof(buf), "SDL3 surface error: %s", SDL_GetError());
        if (!was_init) SDL_Quit(); free(full_path); *error = _strdup(buf); return NULL;
    }
    SDL_Renderer *renderer = SDL_CreateSoftwareRenderer(surface);
    if (!renderer) {
        char buf[256]; snprintf(buf, sizeof(buf), "SDL3 renderer error: %s", SDL_GetError());
        SDL_DestroySurface(surface); if (!was_init) SDL_Quit();
        free(full_path); *error = _strdup(buf); return NULL;
    }

    SDL_SetRenderDrawColorFloat(renderer, 0.10f, 0.10f, 0.12f, 1.0f);
    SDL_RenderClear(renderer);
    int total_nodes = 0;
    for (int i = 0; i < UI_MAX_NODES; i++) if (g_nodes[i].active) total_nodes++;
    for (int i = 0; i < root_count; i++) ui_draw_node(renderer, roots[i]);
    SDL_RenderPresent(renderer);

    remove(full_path);
    int is_png = ends_with(full_path, ".png");
    int save_ok = is_png ? IMG_SavePNG(surface, full_path) : SDL_SaveBMP(surface, full_path);
    const char *sdl_err = save_ok ? "" : SDL_GetError();
    SDL_DestroyRenderer(renderer); SDL_DestroySurface(surface);
    if (!was_init) SDL_Quit();

    if (!save_ok) {
        char buf[512]; snprintf(buf, sizeof(buf), "failed to save image: %s", sdl_err);
        free(full_path); *error = _strdup(buf); return NULL;
    }
    g_dirty = 0;
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "status", "rendered");
    cJSON_AddStringToObject(r, "path", path_buf);
    cJSON_AddNumberToObject(r, "nodes", total_nodes);
    cJSON_AddNumberToObject(r, "width", width);
    cJSON_AddNumberToObject(r, "height", height);
    free(full_path);
    return r;
}

static cJSON *handle_ui_theme_set(cJSON *args, const char *workspace, char **error) {
    int node_id = (int)get_num(args, "node_id", "id", 0);
    if (node_id <= 0) TOOL_ERROR("ui_theme_set requires 'node_id'");
    UiNode *node = ui_find_node(node_id);
    if (!node) TOOL_ERROR("node not found");
    const char *bg_str = get_str(args, "bg_color", "bg");
    if (bg_str) parse_color(bg_str, &node->style.bg_color);
    const char *border_str = get_str(args, "border_color", "border");
    if (border_str) parse_color(border_str, &node->style.border_color);
    cJSON *bw = cJSON_GetObjectItem(args, "border_width");
    if (cJSON_IsNumber(bw)) node->style.border_width = (int)bw->valuedouble;
    cJSON *cr = cJSON_GetObjectItem(args, "corner_radius");
    if (cJSON_IsNumber(cr)) node->style.corner_radius = (int)cr->valuedouble;
    cJSON *pd = cJSON_GetObjectItem(args, "padding");
    if (cJSON_IsNumber(pd)) node->style.padding = (int)pd->valuedouble;
    g_dirty = 1;
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "status", "ok");
    return r;
}

/* ========================= Tool entry ========================= */

static cJSON *sdl3_render(cJSON *args, const char *workspace, char **error) {
    const char *action = get_str(args, "action", "mode");
    if (!action) action = "render";

    /* ---- UI widget actions (checked first) ---- */
    if (strcmp(action, "ui_create") == 0)       return handle_ui_create(args, workspace, error);
    if (strcmp(action, "ui_set_prop") == 0)     return handle_ui_set_prop(args, workspace, error);
    if (strcmp(action, "ui_connect") == 0)      return handle_ui_connect(args, workspace, error);
    if (strcmp(action, "ui_destroy") == 0)      return handle_ui_destroy(args, workspace, error);
    if (strcmp(action, "ui_render_frame") == 0) return handle_ui_render_frame(args, workspace, error);
    if (strcmp(action, "ui_get_tree") == 0)     return handle_ui_get_tree(args, workspace, error);
    if (strcmp(action, "ui_clear") == 0)        return handle_ui_clear(args, workspace, error);
    if (strcmp(action, "ui_theme_set") == 0)    return handle_ui_theme_set(args, workspace, error);

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
        int was_init = SDL_WasInit(SDL_INIT_VIDEO);
        if (!was_init) {
            if (!SDL_Init(SDL_INIT_VIDEO)) {
                char buf[256];
                snprintf(buf, sizeof(buf), "SDL3 init error: %s", SDL_GetError());
                *error = _strdup(buf);
                return NULL;
            }
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
        if (!was_init) SDL_Quit();

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

        /* Initialize SDL video subsystem (guard for debugger coexistence) */
        int render_was_init = SDL_WasInit(SDL_INIT_VIDEO);
        if (!render_was_init) {
            if (!SDL_Init(SDL_INIT_VIDEO)) {
                char buf[256];
                snprintf(buf, sizeof(buf), "SDL3 init error: %s", SDL_GetError());
                free(full_path);
                *error = _strdup(buf);
                return NULL;
            }
        }

        /* Use software renderer with surface for reliable offscreen rendering */
        SDL_Surface *surface = SDL_CreateSurface(width, height, SDL_PIXELFORMAT_RGBA8888);
        if (!surface) {
            char buf[256];
            snprintf(buf, sizeof(buf), "SDL3 surface error: %s", SDL_GetError());
            if (!render_was_init) SDL_Quit();
            free(full_path);
            *error = _strdup(buf);
            return NULL;
        }

        SDL_Renderer *renderer = SDL_CreateSoftwareRenderer(surface);
        if (!renderer) {
            char buf[256];
            snprintf(buf, sizeof(buf), "SDL3 renderer error: %s", SDL_GetError());
            SDL_DestroySurface(surface);
            if (!render_was_init) SDL_Quit();
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
        if (!render_was_init) SDL_Quit();

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

    TOOL_ERROR("unknown action '%s', expected: render, window, info, ui_create, ui_set_prop, ui_connect, ui_destroy, ui_render_frame, ui_get_tree, ui_clear, ui_theme_set");
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
