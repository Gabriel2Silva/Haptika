#pragma once
#include <SDL3/SDL.h>

/*
 * Shared battery display helpers.
 * Map SDL_PowerState + percent into icon names and human-readable labels.
 */

/* Returns a standard symbolic icon name (e.g. "battery-level-80-symbolic").
 * The returned pointer is a string literal — do not free. */
const char *conttest_battery_icon_name(SDL_PowerState state, int percent);

/* Returns a newly-allocated string like "85%", "Charging 60%", "Wired", "Unknown".
 * Caller must g_free() the result. */
char *conttest_battery_format_label(SDL_PowerState state, int percent);

/* Returns a newly-allocated minimalist label like "85%", "Charging", or "Charged".
 * Caller must g_free() the result. */
char *conttest_battery_format_label_compact(SDL_PowerState state, int percent);
