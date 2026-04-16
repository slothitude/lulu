#define _CRT_SECURE_NO_WARNINGS
#include <stdlib.h>
#include <string.h>
#include "event_bus.h"

typedef struct {
    EventCallback cb;
    void         *user_data;
} Sub;

static Sub  g_subs[EVENT_TYPE_COUNT][EVENT_MAX_SUBS];
static int  g_seq = 0;

void event_bus_init(void) {
    memset(g_subs, 0, sizeof(g_subs));
    g_seq = 0;
}

void event_bus_shutdown(void) {
    memset(g_subs, 0, sizeof(g_subs));
}

int event_bus_subscribe(EventType type, EventCallback cb, void *user_data) {
    if ((int)type < 0 || type >= EVENT_TYPE_COUNT) return -1;
    for (int i = 0; i < EVENT_MAX_SUBS; i++) {
        if (!g_subs[type][i].cb) {
            g_subs[type][i].cb = cb;
            g_subs[type][i].user_data = user_data;
            return i;
        }
    }
    return -1;
}

int event_bus_publish(EventType type, const Event *ev) {
    if ((int)type < 0 || type >= EVENT_TYPE_COUNT) return 0;

    /* Copy and stamp seq */
    Event copy = *ev;
    copy.type = type;
    copy.seq  = g_seq++;

    int dispatched = 0;
    for (int i = 0; i < EVENT_MAX_SUBS; i++) {
        if (g_subs[type][i].cb) {
            g_subs[type][i].cb(&copy, g_subs[type][i].user_data);
            dispatched++;
        }
    }
    return dispatched;
}
