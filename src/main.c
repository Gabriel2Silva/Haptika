#include <adwaita.h>
#include <gio/gio.h>
#include <glib/gi18n.h>
#include <SDL3/SDL.h>
#include <stdlib.h>
#include "window.h"
#include "device_manager.h"
#include "haptika-config.h"

typedef struct {
    gboolean   sdl_initialized;
    gboolean   sdl_init_failed;
    char      *sdl_error;
    GSettings *settings;
} AppState;

static void on_about(GSimpleAction *action, GVariant *param, gpointer user_data) {
    (void)action; (void)param;
    GtkApplication *app = GTK_APPLICATION(user_data);
    GtkWindow *window = gtk_application_get_active_window(app);
    if (!window) {
        GList *windows = gtk_application_get_windows(app);
        window = windows ? GTK_WINDOW(windows->data) : NULL;
    }

    AdwDialog *dialog = adw_about_dialog_new_from_appdata(
        "/io/github/gabriel2silva/Haptika/io.github.gabriel2silva.Haptika.metainfo.xml",
        HAPTIKA_VERSION);
    adw_about_dialog_set_developers(ADW_ABOUT_DIALOG(dialog),
                                    (const char *[]){ "Gabriel Limieri", NULL });
    adw_dialog_present(dialog, GTK_WIDGET(window));
}

static void on_style_setting_changed(GSettings *settings, const char *key, gpointer user_data) {
    (void)key; (void)user_data;
    char *style = g_settings_get_string(settings, "style");
    conttest_window_apply_theme(style);
    g_free(style);
}

static void on_startup(GtkApplication *app, gpointer user_data) {
    AppState *state = user_data;

    /* Hints must be set before SDL_Init to take effect. Defer SDL_Init to
     * primary startup so secondary invocations forwarding over D-Bus don't
     * initialize gamepad drivers before exiting. */
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS3, "1");
    if (!SDL_Init(SDL_INIT_GAMEPAD)) {
        state->sdl_init_failed = TRUE;
        state->sdl_error = g_strdup(SDL_GetError());
        g_critical("Failed to initialize SDL3: %s", state->sdl_error);
    } else {
        state->sdl_initialized = TRUE;
        conttest_sdl_configure_events();

        /* Custom mappings for devices SDL doesn't recognize out of the box. */
        SDL_AddGamepadMapping("03000000ff000000cb01000010010000,Sony PlayStation Portable,"
                              "a:b0,b:b1,back:b6,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,dpup:h0.1,"
                              "leftshoulder:b4,leftx:a0,lefty:a1,rightshoulder:b5,start:b7,x:b2,y:b3,"
                              "platform:Linux,");
    }

    /* Register the about action once with the application as user_data so
     * recreating windows cannot leave a dangling pointer. */
    GSimpleAction *about_action = g_simple_action_new("about", NULL);
    g_signal_connect(about_action, "activate", G_CALLBACK(on_about), app);
    g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(about_action));
    g_object_unref(about_action);

    /* Apply saved theme preference before creating any window and keep synced.
     * AdwApplication automatically loads style.css and icons/ from resource_base_path. */
    state->settings = g_settings_new(HAPTIKA_APP_ID);
    g_object_set_data_full(G_OBJECT(app), "haptika-settings",
                           g_object_ref(state->settings), g_object_unref);
    g_signal_connect(state->settings, "changed::style", G_CALLBACK(on_style_setting_changed), NULL);
    on_style_setting_changed(state->settings, "style", NULL);

    /* Keyboard shortcuts */
    gtk_application_set_accels_for_action(app, "app.about",
        (const char *[]){"F1", NULL});
}

static void present_sdl_error_window(GtkApplication *app, const char *error_message) {
    GList *windows = gtk_application_get_windows(app);
    if (windows) {
        gtk_window_present(GTK_WINDOW(windows->data));
        return;
    }

    GtkWidget *win = adw_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(win), "Haptika");
    gtk_window_set_default_size(GTK_WINDOW(win), 460, 320);

    GtkWidget *toolbar = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), adw_header_bar_new());

    GtkWidget *status = adw_status_page_new();
    adw_status_page_set_icon_name(ADW_STATUS_PAGE(status), "dialog-error-symbolic");
    adw_status_page_set_title(ADW_STATUS_PAGE(status),
                              _("Failed to Initialize Controller Backend"));
    const char *message = (error_message && *error_message)
        ? error_message
        : _("SDL3 gamepad initialization failed.");
    /* SDL error strings can contain markup characters; the description is
     * Pango markup. */
    g_autofree char *escaped = g_markup_escape_text(message, -1);
    adw_status_page_set_description(ADW_STATUS_PAGE(status), escaped);

    GtkWidget *quit_btn = gtk_button_new_with_label(_("Quit"));
    gtk_widget_add_css_class(quit_btn, "pill");
    gtk_widget_set_halign(quit_btn, GTK_ALIGN_CENTER);
    g_signal_connect_swapped(quit_btn, "clicked", G_CALLBACK(gtk_window_close), win);
    adw_status_page_set_child(ADW_STATUS_PAGE(status), quit_btn);

    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), status);
    adw_application_window_set_content(ADW_APPLICATION_WINDOW(win), toolbar);
    gtk_window_present(GTK_WINDOW(win));
}

static void on_activate(GtkApplication *app, gpointer user_data) {
    const AppState *state = user_data;
    if (state->sdl_init_failed) {
        present_sdl_error_window(app, state->sdl_error);
        return;
    }

    GList *windows = gtk_application_get_windows(app);
    GtkWindow *window = windows ? GTK_WINDOW(windows->data)
                                : GTK_WINDOW(conttest_window_new(app));
    gtk_window_present(window);
}

int main(int argc, char *argv[]) {
    /* i18n */
    bindtextdomain(GETTEXT_PACKAGE, LOCALEDIR);
    bind_textdomain_codeset(GETTEXT_PACKAGE, "UTF-8");
    textdomain(GETTEXT_PACKAGE);

    AppState state = {0};
    AdwApplication *app = adw_application_new(HAPTIKA_APP_ID,
                                              G_APPLICATION_DEFAULT_FLAGS);
    g_application_set_resource_base_path(G_APPLICATION(app), "/io/github/gabriel2silva/Haptika");
    g_signal_connect(app, "startup", G_CALLBACK(on_startup), &state);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), &state);

    int status = g_application_run(G_APPLICATION(app), argc, argv);

    g_clear_object(&state.settings);
    g_clear_pointer(&state.sdl_error, g_free);
    g_object_unref(app);
    if (state.sdl_initialized) {
        SDL_Quit();
    }
    if (state.sdl_init_failed && status == 0) {
        status = EXIT_FAILURE;
    }
    return status;
}
