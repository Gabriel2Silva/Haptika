#include "battery.h"
#include <glib.h>
#include <glib/gi18n.h>

static const char *const CHARGING_ICONS[11] = {
    "battery-level-0-charging-symbolic",
    "battery-level-10-charging-symbolic",
    "battery-level-20-charging-symbolic",
    "battery-level-30-charging-symbolic",
    "battery-level-40-charging-symbolic",
    "battery-level-50-charging-symbolic",
    "battery-level-60-charging-symbolic",
    "battery-level-70-charging-symbolic",
    "battery-level-80-charging-symbolic",
    "battery-level-90-charging-symbolic",
    "battery-level-100-charged-symbolic",
};

static const char *const ON_BATTERY_ICONS[11] = {
    "battery-level-0-symbolic",
    "battery-level-10-symbolic",
    "battery-level-20-symbolic",
    "battery-level-30-symbolic",
    "battery-level-40-symbolic",
    "battery-level-50-symbolic",
    "battery-level-60-symbolic",
    "battery-level-70-symbolic",
    "battery-level-80-symbolic",
    "battery-level-90-symbolic",
    "battery-level-100-symbolic",
};

static int battery_level_bucket(int percent) {
    int clamped = CLAMP(percent, 0, 100);
    return (clamped + 5) / 10;
}

const char *conttest_battery_icon_name(SDL_PowerState state, int percent) {
    switch (state) {
    case SDL_POWERSTATE_NO_BATTERY:
        return "battery-missing-symbolic";
    case SDL_POWERSTATE_CHARGED:
        return "battery-level-100-charged-symbolic";
    case SDL_POWERSTATE_CHARGING:
        if (percent < 0)
            return "battery-level-50-charging-symbolic";
        /* Adwaita has no battery-level-100-charging-symbolic; only 100% uses charged. */
        if (percent >= 95 && percent < 100)
            return CHARGING_ICONS[9];
        return CHARGING_ICONS[battery_level_bucket(percent)];
    case SDL_POWERSTATE_ON_BATTERY:
        if (percent < 0)
            return "battery-level-50-symbolic";
        return ON_BATTERY_ICONS[battery_level_bucket(percent)];
    default:
        return "battery-missing-symbolic";
    }
}

char *conttest_battery_format_label(SDL_PowerState state, int percent) {
    switch (state) {
    case SDL_POWERSTATE_NO_BATTERY:
        return g_strdup(_("Wired"));
    case SDL_POWERSTATE_CHARGED:
        return g_strdup(_("Charged"));
    case SDL_POWERSTATE_CHARGING:
        if (percent >= 0)
            return g_strdup_printf(_("Charging %d%%"), percent);
        return g_strdup(_("Charging"));
    case SDL_POWERSTATE_ON_BATTERY:
        if (percent >= 0)
            return g_strdup_printf("%d%%", percent);
        return g_strdup(_("On battery"));
    default:
        return g_strdup(_("Unknown"));
    }
}

char *conttest_battery_format_label_compact(SDL_PowerState state, int percent) {
    switch (state) {
    case SDL_POWERSTATE_CHARGED:
        return g_strdup(percent >= 0 ? "100%" : _("Charged"));
    case SDL_POWERSTATE_CHARGING:
    case SDL_POWERSTATE_ON_BATTERY:
        if (percent >= 0)
            return g_strdup_printf("%d%%", percent);
        return g_strdup(state == SDL_POWERSTATE_CHARGING ? _("Charging") : "");
    default:
        return g_strdup("");
    }
}
