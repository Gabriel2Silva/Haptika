#include "selector_view.h"
#include <adwaita.h>
#include <glib/gi18n.h>
#include "window.h"
#include "device_manager.h"
#include "battery.h"
#include <string.h>

struct _ConttestSelectorView {
    GtkBox parent_instance;
    ConttestWindow *window;

    GtkWidget *list_box;
    GtkWidget *placeholder;
};

G_DEFINE_TYPE(ConttestSelectorView, conttest_selector_view, GTK_TYPE_BOX)

/* ── UI policy helpers ─────────────────────────────────────────────────── */

/* When a device supports multiple backends, SDL3 is preferred. */
static ConttestBackend preferred_backend(const ConttestDevice *dev) {
    return conttest_device_get_has_sdl(dev) ? CONTTEST_BACKEND_SDL3 : CONTTEST_BACKEND_EVDEV;
}

/* Format a user-facing subtitle describing available backends and
 * InputPlumber status. Caller must g_free() the result. */
static char *format_device_subtitle(const ConttestDevice *dev) {
    const char *backends = "evdev";
    if (conttest_device_get_has_sdl(dev) && conttest_device_get_has_evdev(dev))
        backends = "SDL3, evdev";
    else if (conttest_device_get_has_sdl(dev))
        backends = "SDL3";

    ConttestVirtualKind vkind = conttest_device_get_virtual_kind(dev);
    if (vkind == CONTTEST_VIRTUAL_INPUTPLUMBER) {
        const ConttestInputPlumberInfo *ip = conttest_device_get_inputplumber_info(dev);
        if (ip && ip->composite_name && *ip->composite_name) {
            return g_strdup_printf(_("%s · InputPlumber: %s"), backends, ip->composite_name);
        }
        return g_strdup_printf(_("%s · InputPlumber"), backends);
    }
    return g_strdup(backends);
}

/* ── Row context and icon cache ────────────────────────────────────────── */

/* Attached to each AdwActionRow via g_object_set_data_full("ctx"). */
typedef struct {
    ConttestSelectorView *view;        /* borrowed, outlives row */
    ConttestDevice       *device;      /* owned ref */
    GtkWidget            *icon;        /* owned by the row widget tree */
    const char           *cached_icon_name; /* static string literal */
    char                 *cached_dev_name;
    gboolean              cached_has_sdl;
    SDL_JoystickID        cached_sdl_id;
    SDL_GamepadType       cached_gamepad_type;
    GtkWidget            *battery_box; /* suffix container for battery icon+label, nullable */
    GtkWidget            *battery_icon;
    GtkWidget            *battery_label;
    GtkWidget            *virtual_icon;    /* nullable, InputPlumber indicator */
    GtkWidget            *connection_icon; /* nullable, wireless/wired indicator */
} RowContext;

static void row_context_free(gpointer data) {
    RowContext *ctx = data;
    if (!ctx) return;
    g_clear_object(&ctx->device);
    g_free(ctx->cached_dev_name);
    g_free(ctx);
}

/* ── Signal handlers ─────────────────────────────────────────────────────── */

static void on_row_activated(AdwActionRow *row, gpointer user_data) {
    (void)user_data;
    RowContext *ctx = g_object_get_data(G_OBJECT(row), "ctx");
    g_autoptr(ConttestDevice) dev = g_object_ref(ctx->device);
    conttest_window_show_input_viewer(ctx->view->window, dev, preferred_backend(dev));
}

static const struct {
    const char *keyword;
    const char *icon_name;
} ICON_LOOKUP[] = {
    { "dualsense",           "ps5-symbolic" },
    { "ps5",                 "ps5-symbolic" },
    { "dualshock 4",         "ps4-symbolic" },
    { "ps4",                 "ps4-symbolic" },
    { "wireless controller", "ps4-symbolic" },
    { "dualshock 3",         "ps3-symbolic" },
    { "ps3",                 "ps3-symbolic" },
    { "dualshock 2",         "ps2-symbolic" },
    { "ps2",                 "ps2-symbolic" },
    { "playstation portable","psp-symbolic" },
    { "psp",                 "psp-symbolic" },
    { "dualshock",           "ps1-symbolic" },
    { "ps1",                 "ps1-symbolic" },
    { "psx",                 "ps1-symbolic" },
    { "playstation",         "ps1-symbolic" },
    { "series x",            "xbox-series-x-symbolic" },
    { "series s",            "xbox-series-x-symbolic" },
    { "xbox one",            "xbox-one-symbolic" },
    { "xbox wireless",       "xbox-one-symbolic" },
    { "xbox 360",            "xbox-360-symbolic" },
    { "xbox controller s",   "xbox-controller-s-symbolic" },
    { "joy-con (l)",         "joy-con-l-symbolic" },
    { "joy-con (r)",         "joy-con-r-symbolic" },
    { "joy-con",             "joy-cons-symbolic" },
    { "switch pro",          "switch-pro-symbolic" },
    { "pro controller",      "switch-pro-symbolic" },
    { "wii u pro",           "wii-u-pro-symbolic" },
    { "wii u",               "wii-u-symbolic" },
    { "wii classic",         "wii-classic-symbolic" },
    { "wiimote",             "wii-symbolic" },
    { "wii remote",          "wii-symbolic" },
    { "gamecube",            "gamecube-symbolic" },
    { "n64",                 "n64-symbolic" },
    { "nintendo 64",         "n64-symbolic" },
    { "snes",                "snes-symbolic" },
    { "super nintendo",      "snes-symbolic" },
    { "nes",                 "nes-symbolic" },
    { "nintendo entertainment system", "nes-symbolic" },
    { "virtual boy",         "virtual-boy-symbolic" },
    { "dreamcast",           "dreamcast-symbolic" },
    { "saturn",              "sega-saturn-symbolic" },
    { "mega drive",          "mega-drive-symbolic" },
    { "genesis",             "mega-drive-symbolic" },
    { "master system",       "master-system-symbolic" },
    { "jaguar",              "atari-jaguar-symbolic" },
    { "atari 2600",          "atari-2600-symbolic" },
    { "2600",                "atari-2600-symbolic" },
    { "stadia",              "stadia-symbolic" },
    { "xbox",                "xbox-symbolic" }
};

/* Case-insensitive substring test used by the device-name icon heuristics. */
static gboolean name_contains_ci(const char *name, const char *keyword) {
    if (!name || !keyword)
        return FALSE;
    char *lower = g_ascii_strdown(name, -1);
    gboolean found = strstr(lower, keyword) != NULL;
    g_free(lower);
    return found;
}

static const char *get_icon_for_name(const char *name) {
    char *lower = g_ascii_strdown(name, -1);
    const char *result = "stadia-symbolic";

    for (size_t i = 0; i < G_N_ELEMENTS(ICON_LOOKUP); i++) {
        if (strstr(lower, ICON_LOOKUP[i].keyword)) {
            result = ICON_LOOKUP[i].icon_name;
            break;
        }
    }

    g_free(lower);
    return result;
}

static const char *get_icon_for_device(const ConttestDevice *dev) {
    /* PSX adapters (e.g. Raphnet PSX-to-USB) have no SDL PS1 type and may be
     * evdev-only or report a generic gamepad type, so the device name takes
     * precedence to ensure every "PSX" device gets the PS1 icon. */
    if (name_contains_ci(conttest_device_get_name(dev), "psx"))
        return "ps1-symbolic";

    if (conttest_device_get_has_sdl(dev)) {
        switch (SDL_GetGamepadTypeForID(conttest_device_get_sdl_id(dev))) {
        case SDL_GAMEPAD_TYPE_PS5:                  return "ps5-symbolic";
        case SDL_GAMEPAD_TYPE_PS4:                  return "ps4-symbolic";
        case SDL_GAMEPAD_TYPE_PS3:                  return "ps3-symbolic";
        case SDL_GAMEPAD_TYPE_XBOX360:              return "xbox-360-symbolic";
        case SDL_GAMEPAD_TYPE_XBOXONE:              return "xbox-one-symbolic";
        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO:  return "switch-pro-symbolic";
        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:  return "joy-con-l-symbolic";
        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT: return "joy-con-r-symbolic";
        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:  return "joy-cons-symbolic";
        default: break;
        }
    }
    return get_icon_for_name(conttest_device_get_name(dev));
}

static void update_row_icon(RowContext *ctx, const ConttestDevice *dev) {
    if (!ctx->icon) return;

    gboolean has_sdl = conttest_device_get_has_sdl(dev);
    SDL_JoystickID sdl_id = has_sdl ? conttest_device_get_sdl_id(dev) : 0;
    SDL_GamepadType gtype =
        has_sdl ? SDL_GetGamepadTypeForID(sdl_id) : SDL_GAMEPAD_TYPE_UNKNOWN;
    const char *name = conttest_device_get_name(dev);

    if (ctx->cached_icon_name &&
        ctx->cached_has_sdl == has_sdl &&
        ctx->cached_sdl_id == sdl_id &&
        ctx->cached_gamepad_type == gtype &&
        g_strcmp0(ctx->cached_dev_name, name) == 0) {
        return;
    }

    const char *icon_name = get_icon_for_device(dev);
    g_free(ctx->cached_dev_name);
    ctx->cached_dev_name = g_strdup(name);
    ctx->cached_has_sdl = has_sdl;
    ctx->cached_sdl_id = sdl_id;
    ctx->cached_gamepad_type = gtype;

    if (g_strcmp0(ctx->cached_icon_name, icon_name) != 0) {
        ctx->cached_icon_name = icon_name;
        gtk_image_set_from_icon_name(GTK_IMAGE(ctx->icon), icon_name);
    }
}

static void update_row_battery(RowContext *ctx, const ConttestDevice *device) {
    SDL_PowerState state = conttest_device_get_battery_state(device);
    int percent = conttest_device_get_battery_percent(device);

    gboolean show = conttest_device_get_has_sdl(device) &&
                    state != SDL_POWERSTATE_UNKNOWN;

    if (show) {
        char *label = conttest_battery_format_label(state, percent);
        gtk_label_set_text(GTK_LABEL(ctx->battery_label), label);
        g_free(label);
        gtk_image_set_from_icon_name(GTK_IMAGE(ctx->battery_icon), conttest_battery_icon_name(state, percent));
        gtk_widget_set_visible(ctx->battery_box, TRUE);
    } else {
        gtk_widget_set_visible(ctx->battery_box, FALSE);
    }

    if (ctx->virtual_icon) {
        if (conttest_device_get_virtual_kind(device) == CONTTEST_VIRTUAL_INPUTPLUMBER) {
            const ConttestInputPlumberInfo *ip = conttest_device_get_inputplumber_info(device);
            if (ip && ip->composite_name && *ip->composite_name) {
                char *tip = g_strdup_printf(_("InputPlumber virtual device (%s)"), ip->composite_name);
                gtk_widget_set_tooltip_text(ctx->virtual_icon, tip);
                g_free(tip);
            } else {
                gtk_widget_set_tooltip_text(ctx->virtual_icon, _("InputPlumber virtual device"));
            }
            gtk_widget_set_visible(ctx->virtual_icon, TRUE);
        } else {
            gtk_widget_set_visible(ctx->virtual_icon, FALSE);
        }
    }

    if (ctx->connection_icon) {
        SDL_JoystickConnectionState conn = conttest_device_get_connection_state(device);
        if (conn != SDL_JOYSTICK_CONNECTION_UNKNOWN) {
            const char *conn_icon = (conn == SDL_JOYSTICK_CONNECTION_WIRELESS)
                ? "haptika-bluetooth-symbolic"
                : "haptika-usb-symbolic";
            gtk_image_set_from_icon_name(GTK_IMAGE(ctx->connection_icon), conn_icon);
            gtk_widget_set_visible(ctx->connection_icon, TRUE);
        } else {
            gtk_widget_set_visible(ctx->connection_icon, FALSE);
        }
    }
}

static GtkWidget *create_row_for_device(ConttestSelectorView *self, ConttestDevice *device) {
    GtkWidget *row = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), conttest_device_get_name(device));
    char *subtitle = format_device_subtitle(device);
    adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);
    g_free(subtitle);

    GtkWidget *icon = gtk_image_new();
    gtk_image_set_pixel_size(GTK_IMAGE(icon), 48);
    adw_action_row_add_prefix(ADW_ACTION_ROW(row), icon);
    
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), TRUE);
    g_signal_connect(row, "activated", G_CALLBACK(on_row_activated), NULL);

    RowContext *ctx = g_new0(RowContext, 1);
    ctx->view   = self;
    ctx->device = g_object_ref(device);
    ctx->icon   = icon;
    update_row_icon(ctx, device);

    /* Always pre-create battery widgets to ensure stable suffix order (Battery | Menu). */
    ctx->battery_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_valign(ctx->battery_box, GTK_ALIGN_CENTER);
    ctx->battery_label = gtk_label_new(NULL);
    gtk_widget_add_css_class(ctx->battery_label, "dimmed");
    gtk_box_append(GTK_BOX(ctx->battery_box), ctx->battery_label);
    ctx->battery_icon = gtk_image_new();
    gtk_image_set_pixel_size(GTK_IMAGE(ctx->battery_icon), 16);
    gtk_box_append(GTK_BOX(ctx->battery_box), ctx->battery_icon);
    
    adw_action_row_add_suffix(ADW_ACTION_ROW(row), ctx->battery_box);

    ctx->virtual_icon = gtk_image_new_from_icon_name("haptika-inputplumber-symbolic");
    gtk_image_set_pixel_size(GTK_IMAGE(ctx->virtual_icon), 16);
    gtk_widget_set_valign(ctx->virtual_icon, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_end(ctx->virtual_icon, 4);
    adw_action_row_add_suffix(ADW_ACTION_ROW(row), ctx->virtual_icon);

    ctx->connection_icon = gtk_image_new();
    gtk_image_set_pixel_size(GTK_IMAGE(ctx->connection_icon), 16);
    gtk_widget_set_valign(ctx->connection_icon, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_end(ctx->connection_icon, 4);
    adw_action_row_add_suffix(ADW_ACTION_ROW(row), ctx->connection_icon);

    update_row_battery(ctx, device);

    g_object_set_data_full(G_OBJECT(row), "ctx", ctx, row_context_free);
    return row;
}

/* ── List management ─────────────────────────────────────────────────────── */

static void update_placeholder(ConttestSelectorView *self) {
    GtkWidget *child = gtk_widget_get_first_child(self->list_box);
    if (child) {
        gtk_widget_set_visible(self->list_box, TRUE);
        gtk_widget_set_visible(self->placeholder, FALSE);
    } else {
        gtk_widget_set_visible(self->list_box, FALSE);
        gtk_widget_set_visible(self->placeholder, TRUE);
    }
}

static RowContext *find_row_ctx_for_device(ConttestSelectorView *self, ConttestDevice *dev, GtkWidget **out_child) {
    for (GtkWidget *child = gtk_widget_get_first_child(self->list_box); child; child = gtk_widget_get_next_sibling(child)) {
        RowContext *ctx = g_object_get_data(G_OBJECT(child), "ctx");
        if (ctx && ctx->device == dev) {
            if (out_child) *out_child = child;
            return ctx;
        }
    }
    if (out_child) *out_child = NULL;
    return NULL;
}

static void on_device_added(ConttestDeviceManager *mgr, ConttestDevice *dev, gpointer user_data) {
    (void)mgr;
    ConttestSelectorView *self = CONTTEST_SELECTOR_VIEW(user_data);
    GtkWidget *row = create_row_for_device(self, dev);
    gtk_list_box_append(GTK_LIST_BOX(self->list_box), row);
    update_placeholder(self);
}

static void on_device_removed(ConttestDeviceManager *mgr, ConttestDevice *dev, gpointer user_data) {
    (void)mgr;
    ConttestSelectorView *self = CONTTEST_SELECTOR_VIEW(user_data);
    GtkWidget *child = NULL;
    if (find_row_ctx_for_device(self, dev, &child) && child) {
        gtk_list_box_remove(GTK_LIST_BOX(self->list_box), child);
    }
    update_placeholder(self);
}

static void on_device_updated(ConttestDeviceManager *mgr, ConttestDevice *dev, gpointer user_data) {
    (void)mgr;
    ConttestSelectorView *self = CONTTEST_SELECTOR_VIEW(user_data);
    GtkWidget *child = NULL;
    RowContext *ctx = find_row_ctx_for_device(self, dev, &child);
    if (!ctx || !child) return;

    AdwActionRow *row = ADW_ACTION_ROW(child);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), conttest_device_get_name(dev));
    char *subtitle = format_device_subtitle(dev);
    adw_action_row_set_subtitle(row, subtitle);
    g_free(subtitle);

    update_row_icon(ctx, dev);

    update_row_battery(ctx, dev);
}

/* ── GObject boilerplate ─────────────────────────────────────────────────── */

static void conttest_selector_view_init(ConttestSelectorView *self) {
    self->list_box = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(self->list_box), GTK_SELECTION_NONE);
    gtk_widget_add_css_class(self->list_box, "boxed-list");
    gtk_widget_set_margin_top(self->list_box, 32);
    gtk_widget_set_margin_bottom(self->list_box, 32);
    gtk_widget_set_visible(self->list_box, FALSE);
    gtk_box_append(GTK_BOX(self), self->list_box);

    self->placeholder = adw_status_page_new();
    adw_status_page_set_title(ADW_STATUS_PAGE(self->placeholder), _("No controllers detected"));
    adw_status_page_set_description(ADW_STATUS_PAGE(self->placeholder), _("Plug in a controller to get started."));
    adw_status_page_set_icon_name(ADW_STATUS_PAGE(self->placeholder), "io.github.gabriel2silva.Haptika");
    gtk_widget_set_vexpand(self->placeholder, TRUE);
    gtk_box_append(GTK_BOX(self), self->placeholder);
}

static void conttest_selector_view_class_init(ConttestSelectorViewClass *klass) {
    (void)klass;
}

static void add_existing_device(ConttestDevice *dev, gpointer user_data) {
    ConttestSelectorView *self = CONTTEST_SELECTOR_VIEW(user_data);
    GtkWidget *row = create_row_for_device(self, dev);
    gtk_list_box_append(GTK_LIST_BOX(self->list_box), row);
}

GtkWidget *conttest_selector_view_new(ConttestWindow *window) {
    ConttestSelectorView *self = g_object_new(CONTTEST_TYPE_SELECTOR_VIEW, "orientation", GTK_ORIENTATION_VERTICAL, NULL);
    self->window = window;

    ConttestDeviceManager *mgr = conttest_window_get_device_manager(self->window);
    conttest_device_manager_foreach(mgr, add_existing_device, self);
    update_placeholder(self);

    g_signal_connect_object(mgr, "device-added",   G_CALLBACK(on_device_added),   self, 0);
    g_signal_connect_object(mgr, "device-removed",  G_CALLBACK(on_device_removed),  self, 0);
    g_signal_connect_object(mgr, "device-updated",  G_CALLBACK(on_device_updated),  self, 0);

    return GTK_WIDGET(self);
}
