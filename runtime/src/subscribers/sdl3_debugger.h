#pragma once

/* Initialize the SDL3 debugger view. Opens a 640x200 window.
   No-op if SDL3 is not available or debug is disabled. */
void sdl3_debugger_init(void);
void sdl3_debugger_shutdown(void);
