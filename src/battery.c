#include "battery.h"
#include <glib.h>
#include <glib/gi18n.h>

const char *conttest_battery_icon_name(SDL_PowerState state, int percent) {
    switch (state) {
    case SDL_POWERSTATE_NO_BATTERY:
        return "battery-missing-symbolic";
    case SDL_POWERSTATE_CHARGED:
        return "battery-full-symbolic";
    case SDL_POWERSTATE_CHARGING:
        if (percent < 0) return "battery-good-charging-symbolic";
        if (percent <= 20) return "battery-caution-charging-symbolic";
        if (percent <= 40) return "battery-low-charging-symbolic";
        if (percent <= 70) return "battery-good-charging-symbolic";
        return "battery-full-charging-symbolic";
    case SDL_POWERSTATE_ON_BATTERY:
        if (percent < 0) return "battery-good-symbolic";
        if (percent <= 20) return "battery-caution-symbolic";
        if (percent <= 40) return "battery-low-symbolic";
        if (percent <= 70) return "battery-good-symbolic";
        return "battery-full-symbolic";
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
