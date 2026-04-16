#pragma once
#include "state.h"

/* Register the memory subscriber. Needs a pointer to the live WorkingMemory
   and the path where memory.json is saved after each update. */
void mem_subscriber_init(WorkingMemory *mem, const char *memory_path);
