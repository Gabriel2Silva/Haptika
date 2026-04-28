#include "window.h"
#include "haptika-config.h"
#include <glib/gi18n.h>
#include "selector_view.h"
#include "input_viewer.h"
#include "device_manager.h"
#include "battery.h"
#include "ds4_info.h"
#include "ds5_info.h"
#include "ds3_info.h"
#include <gio/gio.h>
#include <inttypes.h>

struct _ConttestWindow {
    AdwApplicationWindow parent_instance;

    GtkWidget *toast_overlay;
    GtkWidget *nav_view;
    
    GtkWidget *selector_page;
    GtkWidget *selector_view;
    
    GtkWidget *viewer_page;
    GtkWidget *input_viewer;

    ConttestDeviceManager *device_manager;

    /* Currently visible device in the viewer (borrowed from manager). */
    ConttestDevice *active_device;
    ConttestBackend active_backend;

    /* Header bar battery widgets for the viewer page. */
    GtkWidget *view_battery_box;
    GtkWidget *view_battery_icon;
    GtkWidget *view_battery_label;
    GtkWidget *view_connection_icon;
    GtkWidget *view_controller_info_btn;
    GtkWidget *view_backend_btn;
    /* Theme selector checkbuttons */
    GtkWidget *theme_follow;
    GtkWidget *theme_light;
    GtkWidget *theme_dark;
};

G_DEFINE_TYPE(ConttestWindow, conttest_window, ADW_TYPE_APPLICATION_WINDOW)

static void conttest_window_dispose(GObject *object) {

    ConttestWindow *self = CONTTEST_WINDOW(object);

    /* Stop the viewer and clear the borrowed device pointer before the
     * manager is disposed — the manager owns the ConttestDevice objects. */
    if (self->active_device) {
        conttest_input_viewer_stop(CONTTEST_INPUT_VIEWER(self->input_viewer));
        self->active_device = NULL;
    }

    if (self->device_manager) {
        g_clear_object(&self->device_manager);
    }
    G_OBJECT_CLASS(conttest_window_parent_class)->dispose(object);
}


static void conttest_window_class_init(ConttestWindowClass *klass) {
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->dispose = conttest_window_dispose;
}

static void on_manager_device_updated(ConttestDeviceManager *mgr, ConttestDevice *dev, gpointer user_data);
static void on_manager_device_removed(ConttestDeviceManager *mgr, ConttestDevice *dev, gpointer user_data);


/* ── Theme persistence ─────────────────────────────────────────────────── */

/* App-lifetime GSettings instance. Loaded lazily on first access. */
static GSettings *s_settings = NULL;

static GSettings *app_settings(void) {
    if (!s_settings)
        s_settings = g_settings_new(HAPTIKA_APP_ID);
    return s_settings;
}

static void theme_save(const char *style) {
    g_settings_set_string(app_settings(), "style", style);
}

static const char *theme_load(void) {
    char *val = g_settings_get_string(app_settings(), "style");
    if (!val) return "follow";
    if (g_str_equal(val, "dark"))  { g_free(val); return "dark"; }
    if (g_str_equal(val, "light")) { g_free(val); return "light"; }
    g_free(val);
    return "follow";
}

static void theme_apply(const char *style) {
    AdwStyleManager *mgr = adw_style_manager_get_default();
    if (g_str_equal(style, "dark"))
        adw_style_manager_set_color_scheme(mgr, ADW_COLOR_SCHEME_FORCE_DARK);
    else if (g_str_equal(style, "light"))
        adw_style_manager_set_color_scheme(mgr, ADW_COLOR_SCHEME_FORCE_LIGHT);
    else
        adw_style_manager_set_color_scheme(mgr, ADW_COLOR_SCHEME_DEFAULT);
}

/* ── Theme selector widget ─────────────────────────────────────────────── */

static void on_theme_toggled(GtkCheckButton *btn, gpointer user_data) {
    if (!gtk_check_button_get_active(btn)) return;
    const char *style = (const char *)user_data;
    theme_save(style);
    theme_apply(style);
}

static GtkWidget *build_theme_selector(ConttestWindow *self) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_add_css_class(box, "themeselector");
    gtk_widget_set_hexpand(box, TRUE);

    self->theme_follow = gtk_check_button_new();
    gtk_widget_add_css_class(self->theme_follow, "theme-selector");
    gtk_widget_add_css_class(self->theme_follow, "follow");
    gtk_widget_set_hexpand(self->theme_follow, TRUE);
    gtk_widget_set_halign(self->theme_follow, GTK_ALIGN_CENTER);
    gtk_widget_set_focus_on_click(self->theme_follow, FALSE);
    gtk_widget_set_tooltip_text(self->theme_follow, _("Follow System Style"));

    self->theme_light = gtk_check_button_new();
    gtk_widget_add_css_class(self->theme_light, "theme-selector");
    gtk_widget_add_css_class(self->theme_light, "light");
    gtk_check_button_set_group(GTK_CHECK_BUTTON(self->theme_light), GTK_CHECK_BUTTON(self->theme_follow));
    gtk_widget_set_hexpand(self->theme_light, TRUE);
    gtk_widget_set_halign(self->theme_light, GTK_ALIGN_CENTER);
    gtk_widget_set_focus_on_click(self->theme_light, FALSE);
    gtk_widget_set_tooltip_text(self->theme_light, _("Light Style"));

    self->theme_dark = gtk_check_button_new();
    gtk_widget_add_css_class(self->theme_dark, "theme-selector");
    gtk_widget_add_css_class(self->theme_dark, "dark");
    gtk_check_button_set_group(GTK_CHECK_BUTTON(self->theme_dark), GTK_CHECK_BUTTON(self->theme_follow));
    gtk_widget_set_hexpand(self->theme_dark, TRUE);
    gtk_widget_set_halign(self->theme_dark, GTK_ALIGN_CENTER);
    gtk_widget_set_focus_on_click(self->theme_dark, FALSE);
    gtk_widget_set_tooltip_text(self->theme_dark, _("Dark Style"));

    gtk_box_append(GTK_BOX(box), self->theme_follow);
    gtk_box_append(GTK_BOX(box), self->theme_light);
    gtk_box_append(GTK_BOX(box), self->theme_dark);

    /* Set initial state from saved preference */
    const char *style = theme_load();
    if (g_str_equal(style, "light"))
        gtk_check_button_set_active(GTK_CHECK_BUTTON(self->theme_light), TRUE);
    else if (g_str_equal(style, "dark"))
        gtk_check_button_set_active(GTK_CHECK_BUTTON(self->theme_dark), TRUE);
    else
        gtk_check_button_set_active(GTK_CHECK_BUTTON(self->theme_follow), TRUE);

    static const char style_follow[] = "follow";
    static const char style_light[]  = "light";
    static const char style_dark[]   = "dark";

    g_signal_connect(self->theme_follow, "toggled", G_CALLBACK(on_theme_toggled), (gpointer)style_follow);
    g_signal_connect(self->theme_light,  "toggled", G_CALLBACK(on_theme_toggled), (gpointer)style_light);
    g_signal_connect(self->theme_dark,   "toggled", G_CALLBACK(on_theme_toggled), (gpointer)style_dark);

    return box;
}

static GtkWidget *build_menu_button(ConttestWindow *self) {
    GMenu *menu = g_menu_new();

    GMenu *theme_section = g_menu_new();
    g_menu_append_item(theme_section, g_menu_item_new_section(NULL, G_MENU_MODEL(g_menu_new())));
    GMenuItem *theme_item = g_menu_item_new(NULL, NULL);
    g_menu_item_set_attribute(theme_item, "custom", "s", "theme");
    g_menu_append_item(theme_section, theme_item);
    g_object_unref(theme_item);
    g_menu_append_section(menu, NULL, G_MENU_MODEL(theme_section));
    g_object_unref(theme_section);

    GMenu *app_section = g_menu_new();
    g_menu_append(app_section, _("About Haptika"), "app.about");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(app_section));
    g_object_unref(app_section);

    GtkWidget *btn = gtk_menu_button_new();
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(btn), "open-menu-symbolic");
    gtk_widget_set_tooltip_text(btn, _("Main Menu"));
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(btn), G_MENU_MODEL(menu));
    g_object_unref(menu);

    GtkWidget *selector = build_theme_selector(self);
    GtkPopoverMenu *popover = GTK_POPOVER_MENU(gtk_menu_button_get_popover(GTK_MENU_BUTTON(btn)));
    gtk_popover_menu_add_child(popover, selector, "theme");

    return btn;
}



static void on_switch_backend_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    ConttestWindow *self = CONTTEST_WINDOW(user_data);
    if (!self->active_device) return;
    ConttestBackend other = (self->active_backend == CONTTEST_BACKEND_SDL3)
        ? CONTTEST_BACKEND_EVDEV : CONTTEST_BACKEND_SDL3;
    conttest_window_show_input_viewer(self, self->active_device, other);
}

static void on_controller_info_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    ConttestWindow *self = CONTTEST_WINDOW(user_data);
    if (!self->active_device) return;
    if (conttest_device_is_ds5(self->active_device))
        ds5_info_show(GTK_WIDGET(self), self->active_device);
    else if (conttest_device_is_ds3(self->active_device))
        ds3_info_show(GTK_WIDGET(self), self->active_device);
    else
        ds4_info_show(GTK_WIDGET(self), self->active_device);
}

static void conttest_window_init(ConttestWindow *self) {
    gtk_window_set_title(GTK_WINDOW(self), "Haptika");
    gtk_window_set_icon_name(GTK_WINDOW(self), "io.github.gabriel2silva.Haptika");
    gtk_window_set_default_size(GTK_WINDOW(self), 1100, 800);

    self->device_manager = conttest_device_manager_new();

    self->toast_overlay = adw_toast_overlay_new();
    adw_application_window_set_content(ADW_APPLICATION_WINDOW(self), self->toast_overlay);

    self->nav_view = adw_navigation_view_new();
    adw_toast_overlay_set_child(ADW_TOAST_OVERLAY(self->toast_overlay), self->nav_view);

    // Selector Page
    self->selector_view = conttest_selector_view_new(self);
    GtkWidget *sel_clamp = adw_clamp_new();
    adw_clamp_set_child(ADW_CLAMP(sel_clamp), self->selector_view);

    GtkWidget *sel_toolbar = adw_toolbar_view_new();
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(sel_toolbar), sel_clamp);
    GtkWidget *sel_header = adw_header_bar_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(sel_toolbar), sel_header);
    adw_header_bar_pack_end(ADW_HEADER_BAR(sel_header), build_menu_button(self));

    self->selector_page = GTK_WIDGET(adw_navigation_page_new(sel_toolbar, "selector"));
    adw_navigation_page_set_title(ADW_NAVIGATION_PAGE(self->selector_page), _("Controllers"));
    adw_navigation_view_add(ADW_NAVIGATION_VIEW(self->nav_view), ADW_NAVIGATION_PAGE(self->selector_page));

    // Input Viewer Page
    self->input_viewer = conttest_input_viewer_new(self);

    GtkWidget *view_toolbar = adw_toolbar_view_new();
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(view_toolbar), self->input_viewer);
    GtkWidget *view_header = adw_header_bar_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view_toolbar), view_header);

    self->viewer_page = GTK_WIDGET(adw_navigation_page_new(view_toolbar, "viewer"));
    adw_navigation_view_add(ADW_NAVIGATION_VIEW(self->nav_view), ADW_NAVIGATION_PAGE(self->viewer_page));

    /* Viewer header battery widgets */
    self->view_battery_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_valign(self->view_battery_box, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_end(self->view_battery_box, 6);
    gtk_widget_set_visible(self->view_battery_box, FALSE);

    self->view_battery_label = gtk_label_new(NULL);
    gtk_widget_add_css_class(self->view_battery_label, "dim-label");
    gtk_box_append(GTK_BOX(self->view_battery_box), self->view_battery_label);

    self->view_battery_icon = gtk_image_new();
    gtk_image_set_pixel_size(GTK_IMAGE(self->view_battery_icon), 16);
    gtk_box_append(GTK_BOX(self->view_battery_box), self->view_battery_icon);

    adw_header_bar_pack_end(ADW_HEADER_BAR(view_header), self->view_battery_box);

    self->view_connection_icon = gtk_image_new();
    gtk_image_set_pixel_size(GTK_IMAGE(self->view_connection_icon), 16);
    gtk_widget_set_valign(self->view_connection_icon, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_end(self->view_connection_icon, 6);
    gtk_widget_set_visible(self->view_connection_icon, FALSE);
    adw_header_bar_pack_end(ADW_HEADER_BAR(view_header), self->view_connection_icon);

    self->view_controller_info_btn = gtk_button_new_from_icon_name("haptika-help-about-symbolic");
    gtk_widget_set_tooltip_text(self->view_controller_info_btn, _("Controller Info"));
    gtk_widget_add_css_class(self->view_controller_info_btn, "flat");
    gtk_widget_set_visible(self->view_controller_info_btn, FALSE);
    g_signal_connect(self->view_controller_info_btn, "clicked", G_CALLBACK(on_controller_info_clicked), self);
    adw_header_bar_pack_end(ADW_HEADER_BAR(view_header), self->view_controller_info_btn);

    self->view_backend_btn = gtk_button_new_from_icon_name("haptika-backend-symbolic");
    gtk_widget_add_css_class(self->view_backend_btn, "flat");
    gtk_widget_set_visible(self->view_backend_btn, FALSE);
    g_signal_connect(self->view_backend_btn, "clicked", G_CALLBACK(on_switch_backend_clicked), self);
    adw_header_bar_pack_end(ADW_HEADER_BAR(view_header), self->view_backend_btn);

    /* Apply saved theme preference */
    theme_apply(theme_load());

    /* Manager signals */
    g_signal_connect_object(self->device_manager, "device-updated", G_CALLBACK(on_manager_device_updated), self, 0);
    g_signal_connect_object(self->device_manager, "device-removed", G_CALLBACK(on_manager_device_removed), self, 0);
}

GtkWidget *conttest_window_new(GtkApplication *app) {
    return g_object_new(CONTTEST_TYPE_WINDOW, "application", app, NULL);
}

static void update_header_battery(ConttestWindow *self) {
    if (!self->active_device) {
        gtk_widget_set_visible(self->view_battery_box, FALSE);
        gtk_widget_set_visible(self->view_connection_icon, FALSE);
        return;
    }

    SDL_PowerState state = conttest_device_get_battery_state(self->active_device);
    int percent = conttest_device_get_battery_percent(self->active_device);

    if (conttest_device_get_has_sdl(self->active_device) && state != SDL_POWERSTATE_UNKNOWN) {
        char *label = conttest_battery_format_label_compact(state, percent);
        gtk_label_set_text(GTK_LABEL(self->view_battery_label), label);
        g_free(label);
        gtk_image_set_from_icon_name(GTK_IMAGE(self->view_battery_icon), conttest_battery_icon_name(state, percent));
        gtk_widget_set_visible(self->view_battery_box, TRUE);
    } else {
        gtk_widget_set_visible(self->view_battery_box, FALSE);
    }

    SDL_JoystickConnectionState conn = conttest_device_get_connection_state(self->active_device);
    if (conn != SDL_JOYSTICK_CONNECTION_UNKNOWN) {
        const char *conn_icon = (conn == SDL_JOYSTICK_CONNECTION_WIRELESS)
            ? "haptika-bluetooth-symbolic"
            : "haptika-usb-symbolic";
        gtk_image_set_from_icon_name(GTK_IMAGE(self->view_connection_icon), conn_icon);
        gtk_widget_set_visible(self->view_connection_icon, TRUE);
    } else {
        gtk_widget_set_visible(self->view_connection_icon, FALSE);
    }
}

static void on_manager_device_updated(ConttestDeviceManager *mgr, ConttestDevice *dev, gpointer user_data) {
    (void)mgr;
    ConttestWindow *self = CONTTEST_WINDOW(user_data);
    if (dev == self->active_device) {
        update_header_battery(self);
    }
}

static void on_manager_device_removed(ConttestDeviceManager *mgr, ConttestDevice *dev, gpointer user_data) {
    (void)mgr;
    ConttestWindow *self = CONTTEST_WINDOW(user_data);
    if (dev == self->active_device) {
        /* Device currently being viewed was removed. Cleanly return to selector. */
        conttest_window_show_selector(self);
        conttest_window_show_toast(self, _("Controller disconnected."));
    }
}

void conttest_window_show_selector(ConttestWindow *self) {
    self->active_device = NULL;
    gtk_widget_set_visible(self->view_battery_box, FALSE);
    gtk_widget_set_visible(self->view_connection_icon, FALSE);
    gtk_widget_set_visible(self->view_controller_info_btn, FALSE);
    gtk_widget_set_visible(self->view_backend_btn, FALSE);

    conttest_input_viewer_stop(CONTTEST_INPUT_VIEWER(self->input_viewer));
    adw_navigation_view_pop_to_page(ADW_NAVIGATION_VIEW(self->nav_view), ADW_NAVIGATION_PAGE(self->selector_page));
}

void conttest_window_show_input_viewer(ConttestWindow *self, ConttestDevice *device, ConttestBackend backend) {
    const char *path_or_id = NULL;
    char id_str[32];

    if (backend == CONTTEST_BACKEND_SDL3) {
        snprintf(id_str, sizeof(id_str), "%" PRIu32, (uint32_t)conttest_device_get_sdl_id(device));
        path_or_id = id_str;
    } else {
        path_or_id = conttest_device_get_evdev_syspath(device);
    }

    if (conttest_input_viewer_start(CONTTEST_INPUT_VIEWER(self->input_viewer), backend, path_or_id, device)) {
        self->active_device  = device;
        self->active_backend = backend;
        update_header_battery(self);
        gtk_widget_set_visible(self->view_controller_info_btn, conttest_device_is_ds4(device) || conttest_device_is_ds5(device) || conttest_device_is_ds3(device));

        gboolean has_both = conttest_device_get_has_sdl(device) && conttest_device_get_has_evdev(device);
        if (has_both) {
            const char *tooltip = (backend == CONTTEST_BACKEND_SDL3)
                ? _("Switch to evdev") : _("Switch to SDL3");
            gtk_widget_set_tooltip_text(self->view_backend_btn, tooltip);
            gtk_widget_set_visible(self->view_backend_btn, TRUE);
        } else {
            gtk_widget_set_visible(self->view_backend_btn, FALSE);
        }

        adw_navigation_page_set_title(ADW_NAVIGATION_PAGE(self->viewer_page), conttest_device_get_name(device));
        adw_navigation_view_push(ADW_NAVIGATION_VIEW(self->nav_view), ADW_NAVIGATION_PAGE(self->viewer_page));
    } else {
        conttest_window_show_toast(self, _("Failed to connect to device. It might be busy or requested API is blocked."));
    }
}

void conttest_window_show_toast(ConttestWindow *self, const char *message) {
    AdwToast *toast = adw_toast_new(message);
    adw_toast_overlay_add_toast(ADW_TOAST_OVERLAY(self->toast_overlay), toast);
}

ConttestDeviceManager *conttest_window_get_device_manager(ConttestWindow *self) {
    return self->device_manager;
}
