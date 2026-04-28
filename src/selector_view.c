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

/* --- UI policy helpers --- */

/* When a device supports multiple backends, SDL3 is preferred. */
static ConttestBackend preferred_backend(const ConttestDevice *dev) {
    return conttest_device_get_has_sdl(dev) ? CONTTEST_BACKEND_SDL3 : CONTTEST_BACKEND_EVDEV;
}

/* Format a user-facing subtitle describing available backends. */
static const char *format_backend_subtitle(const ConttestDevice *dev) {
    if (conttest_device_get_has_sdl(dev) && conttest_device_get_has_evdev(dev)) return "SDL3, evdev";
    if (conttest_device_get_has_sdl(dev))  return "SDL3";
    return "evdev";
}

/* --- Typed context structs for row / menu wiring --- */

/* Attached to each AdwActionRow via g_object_set_data_full("ctx"). */
typedef struct {
    ConttestSelectorView *view;        /* borrowed, outlives row */
    ConttestDevice       *device;      /* borrowed, manager-owned */
    GtkWidget            *icon;        /* owned by the row widget tree */
    GtkWidget            *battery_box; /* suffix container for battery icon+label, nullable */
    GtkWidget            *battery_icon;
    GtkWidget            *battery_label;
    GtkWidget            *connection_icon; /* nullable, wireless/wired indicator */
} RowContext;

/* --- Signal handlers --- */

static void on_row_activated(AdwActionRow *row, gpointer user_data) {
    (void)user_data;
    RowContext *ctx = g_object_get_data(G_OBJECT(row), "ctx");
    conttest_window_show_input_viewer(ctx->view->window, ctx->device, preferred_backend(ctx->device));
}

static const struct {
    const char *keyword;
    const char *icon_path;
} ICON_LOOKUP[] = {
    { "dualsense",           "/io/github/gabriel2silva/Haptika/icons/ps5.svg" },
    { "ps5",                 "/io/github/gabriel2silva/Haptika/icons/ps5.svg" },
    { "dualshock 4",         "/io/github/gabriel2silva/Haptika/icons/ps4.svg" },
    { "ps4",                 "/io/github/gabriel2silva/Haptika/icons/ps4.svg" },
    { "wireless controller", "/io/github/gabriel2silva/Haptika/icons/ps4.svg" },
    { "dualshock 3",         "/io/github/gabriel2silva/Haptika/icons/ps3.svg" },
    { "ps3",                 "/io/github/gabriel2silva/Haptika/icons/ps3.svg" },
    { "dualshock 2",         "/io/github/gabriel2silva/Haptika/icons/ps2.svg" },
    { "ps2",                 "/io/github/gabriel2silva/Haptika/icons/ps2.svg" },
    { "playstation portable","/io/github/gabriel2silva/Haptika/icons/psp.svg" },
    { "psp",                 "/io/github/gabriel2silva/Haptika/icons/psp.svg" },
    { "dualshock",           "/io/github/gabriel2silva/Haptika/icons/ps1.svg" },
    { "ps1",                 "/io/github/gabriel2silva/Haptika/icons/ps1.svg" },
    { "playstation",         "/io/github/gabriel2silva/Haptika/icons/ps1.svg" },
    { "series x",            "/io/github/gabriel2silva/Haptika/icons/xbox-series-x.svg" },
    { "series s",            "/io/github/gabriel2silva/Haptika/icons/xbox-series-x.svg" },
    { "xbox one",            "/io/github/gabriel2silva/Haptika/icons/xbox-one.svg" },
    { "xbox wireless",       "/io/github/gabriel2silva/Haptika/icons/xbox-one.svg" },
    { "xbox 360",            "/io/github/gabriel2silva/Haptika/icons/xbox-360.svg" },
    { "xbox controller s",   "/io/github/gabriel2silva/Haptika/icons/xbox-controller-s.svg" },
    { "joy-con (l)",         "/io/github/gabriel2silva/Haptika/icons/joy-con-l.svg" },
    { "joy-con (r)",         "/io/github/gabriel2silva/Haptika/icons/joy-con-r.svg" },
    { "joy-con",             "/io/github/gabriel2silva/Haptika/icons/joy-cons.svg" },
    { "switch pro",          "/io/github/gabriel2silva/Haptika/icons/switch-pro.svg" },
    { "pro controller",      "/io/github/gabriel2silva/Haptika/icons/switch-pro.svg" },
    { "wii u pro",           "/io/github/gabriel2silva/Haptika/icons/wii-u-pro.svg" },
    { "wii u",               "/io/github/gabriel2silva/Haptika/icons/wii-u.svg" },
    { "wii classic",         "/io/github/gabriel2silva/Haptika/icons/wii-classic.svg" },
    { "wiimote",             "/io/github/gabriel2silva/Haptika/icons/wii.svg" },
    { "wii remote",          "/io/github/gabriel2silva/Haptika/icons/wii.svg" },
    { "gamecube",            "/io/github/gabriel2silva/Haptika/icons/gamecube.svg" },
    { "n64",                 "/io/github/gabriel2silva/Haptika/icons/n64.svg" },
    { "nintendo 64",         "/io/github/gabriel2silva/Haptika/icons/n64.svg" },
    { "snes",                "/io/github/gabriel2silva/Haptika/icons/snes.svg" },
    { "super nintendo",      "/io/github/gabriel2silva/Haptika/icons/snes.svg" },
    { "nes",                 "/io/github/gabriel2silva/Haptika/icons/nes.svg" },
    { "nintendo entertainment system", "/io/github/gabriel2silva/Haptika/icons/nes.svg" },
    { "virtual boy",         "/io/github/gabriel2silva/Haptika/icons/virtual-boy.svg" },
    { "dreamcast",           "/io/github/gabriel2silva/Haptika/icons/dreamcast.svg" },
    { "saturn",              "/io/github/gabriel2silva/Haptika/icons/sega-saturn.svg" },
    { "mega drive",          "/io/github/gabriel2silva/Haptika/icons/mega-drive.svg" },
    { "genesis",             "/io/github/gabriel2silva/Haptika/icons/mega-drive.svg" },
    { "master system",       "/io/github/gabriel2silva/Haptika/icons/master-system.svg" },
    { "jaguar",              "/io/github/gabriel2silva/Haptika/icons/atari-jaguar.svg" },
    { "atari 2600",          "/io/github/gabriel2silva/Haptika/icons/atari-2600.svg" },
    { "2600",                "/io/github/gabriel2silva/Haptika/icons/atari-2600.svg" },
    { "stadia",              "/io/github/gabriel2silva/Haptika/icons/stadia.svg" }
};

static const char *get_icon_for_name(const char *name) {
    char *lower = g_ascii_strdown(name, -1);
    const char *result = "/io/github/gabriel2silva/Haptika/icons/stadia.svg";

    for (size_t i = 0; i < G_N_ELEMENTS(ICON_LOOKUP); i++) {
        if (strstr(lower, ICON_LOOKUP[i].keyword)) {
            result = ICON_LOOKUP[i].icon_path;
            break;
        }
    }

    g_free(lower);
    return result;
}

static const char *get_icon_for_device(const ConttestDevice *dev) {
    if (conttest_device_get_has_sdl(dev)) {
        switch (SDL_GetGamepadTypeForID(conttest_device_get_sdl_id(dev))) {
        case SDL_GAMEPAD_TYPE_PS5:                  return "/io/github/gabriel2silva/Haptika/icons/ps5.svg";
        case SDL_GAMEPAD_TYPE_PS4:                  return "/io/github/gabriel2silva/Haptika/icons/ps4.svg";
        case SDL_GAMEPAD_TYPE_PS3:                  return "/io/github/gabriel2silva/Haptika/icons/ps3.svg";
        case SDL_GAMEPAD_TYPE_XBOX360:              return "/io/github/gabriel2silva/Haptika/icons/xbox-360.svg";
        case SDL_GAMEPAD_TYPE_XBOXONE:              return "/io/github/gabriel2silva/Haptika/icons/xbox-one.svg";
        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO:  return "/io/github/gabriel2silva/Haptika/icons/switch-pro.svg";
        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:  return "/io/github/gabriel2silva/Haptika/icons/joy-con-l.svg";
        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT: return "/io/github/gabriel2silva/Haptika/icons/joy-con-r.svg";
        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:  return "/io/github/gabriel2silva/Haptika/icons/joy-cons.svg";
        default: break;
        }
    }
    return get_icon_for_name(conttest_device_get_name(dev));
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
    adw_action_row_set_subtitle(ADW_ACTION_ROW(row), format_backend_subtitle(device));

    const char *icon_path = get_icon_for_device(device);
    GtkWidget *icon = gtk_image_new_from_resource(icon_path);
    gtk_image_set_pixel_size(GTK_IMAGE(icon), 48);
    adw_action_row_add_prefix(ADW_ACTION_ROW(row), icon);
    
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), TRUE);
    g_signal_connect(row, "activated", G_CALLBACK(on_row_activated), NULL);

    RowContext *ctx = g_new0(RowContext, 1);
    ctx->view   = self;
    ctx->device = device;
    ctx->icon   = icon;

    /* Always pre-create battery widgets to ensure stable suffix order (Battery | Menu). */
    ctx->battery_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_valign(ctx->battery_box, GTK_ALIGN_CENTER);
    ctx->battery_label = gtk_label_new(NULL);
    gtk_widget_add_css_class(ctx->battery_label, "dim-label");
    gtk_box_append(GTK_BOX(ctx->battery_box), ctx->battery_label);
    ctx->battery_icon = gtk_image_new();
    gtk_image_set_pixel_size(GTK_IMAGE(ctx->battery_icon), 16);
    gtk_box_append(GTK_BOX(ctx->battery_box), ctx->battery_icon);
    
    adw_action_row_add_suffix(ADW_ACTION_ROW(row), ctx->battery_box);

    ctx->connection_icon = gtk_image_new();
    gtk_image_set_pixel_size(GTK_IMAGE(ctx->connection_icon), 16);
    gtk_widget_set_valign(ctx->connection_icon, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_end(ctx->connection_icon, 4);
    adw_action_row_add_suffix(ADW_ACTION_ROW(row), ctx->connection_icon);

    update_row_battery(ctx, device);

    g_object_set_data_full(G_OBJECT(row), "ctx", ctx, g_free);
    return row;
}

/* --- List management --- */

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
    adw_action_row_set_subtitle(row, format_backend_subtitle(dev));

    if (ctx->icon) {
        gtk_image_set_from_resource(GTK_IMAGE(ctx->icon), get_icon_for_device(dev));
    }

    update_row_battery(ctx, dev);
}

/* --- GObject boilerplate --- */

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
