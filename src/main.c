#include <adwaita.h>
#include <glib/gi18n.h>
#include <SDL3/SDL.h>
#include "window.h"
#include "haptika-config.h"

static void on_about(GSimpleAction *action, GVariant *param, gpointer user_data) {
    (void)action; (void)param;
    GtkWindow *window = GTK_WINDOW(user_data);

    AdwDialog *dialog = adw_about_dialog_new();
    adw_about_dialog_set_application_name(ADW_ABOUT_DIALOG(dialog), "Haptika");
    adw_about_dialog_set_application_icon(ADW_ABOUT_DIALOG(dialog), HAPTIKA_APP_ID);
    adw_about_dialog_set_version(ADW_ABOUT_DIALOG(dialog), HAPTIKA_VERSION);
    adw_about_dialog_set_developer_name(ADW_ABOUT_DIALOG(dialog), "Gabriel Limieri");
    adw_about_dialog_set_developers(ADW_ABOUT_DIALOG(dialog), (const char *[]){ "Gabriel Limieri", NULL });
    adw_about_dialog_set_license_type(ADW_ABOUT_DIALOG(dialog), GTK_LICENSE_GPL_3_0);
    adw_about_dialog_set_issue_url(ADW_ABOUT_DIALOG(dialog), "https://github.com/Gabriel2Silva/Haptika/issues");
    adw_dialog_present(dialog, GTK_WIDGET(window));
}

static void on_startup(GtkApplication *app, gpointer user_data) {
    (void)user_data;

    // Register the about action once.
    GSimpleAction *about_action = g_simple_action_new("about", NULL);
    g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(about_action));
    g_object_unref(about_action);

    // Load custom CSS
    GtkCssProvider *css_provider = gtk_css_provider_new();
    gtk_css_provider_load_from_resource(css_provider, "/io/github/gabriel2silva/Haptika/style.css");
    gtk_style_context_add_provider_for_display(gdk_display_get_default(),
                                               GTK_STYLE_PROVIDER(css_provider),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css_provider);

    /* Ensure bundled icons are found on all desktops (not just GNOME) */
    GtkIconTheme *icon_theme = gtk_icon_theme_get_for_display(gdk_display_get_default());
    gtk_icon_theme_add_resource_path(icon_theme, "/io/github/gabriel2silva/Haptika/icons");

    /* Keyboard shortcuts */
    gtk_application_set_accels_for_action(app, "app.about",
        (const char *[]){"F1", NULL});
}

static void on_activate(GtkApplication *app, gpointer user_data) {
    (void)user_data;

    // Create the main window
    GtkWidget *window = conttest_window_new(app);

    // Point the about action at the current window. Disconnect any previous
    // handler first so a second activate (e.g. raising an existing instance)
    // doesn't accumulate stale connections.
    GAction *about_action = g_action_map_lookup_action(G_ACTION_MAP(app), "about");
    g_signal_handlers_disconnect_by_func(about_action, on_about, NULL);
    g_signal_connect(about_action, "activate", G_CALLBACK(on_about), window);

    gtk_window_present(GTK_WINDOW(window));
}

int main(int argc, char *argv[]) {
    /* i18n */
    bindtextdomain(GETTEXT_PACKAGE, LOCALEDIR);
    bind_textdomain_codeset(GETTEXT_PACKAGE, "UTF-8");
    textdomain(GETTEXT_PACKAGE);

    /* Hints must be set before SDL_Init to take effect. */
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS3, "1");

    if (!SDL_Init(SDL_INIT_GAMEPAD)) {
        g_error("Failed to initialize SDL3: %s", SDL_GetError());
    }

    /* Custom mappings for devices SDL doesn't recognize out of the box. */
    SDL_AddGamepadMapping("03000000ff000000cb01000010010000,Sony PlayStation Portable,"
                          "a:b0,b:b1,back:b6,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,dpup:h0.1,"
                          "leftshoulder:b4,leftx:a0,lefty:a1,rightshoulder:b5,start:b7,x:b2,y:b3,"
                          "platform:Linux,");

    AdwApplication *app = adw_application_new("io.github.gabriel2silva.Haptika",
                                              G_APPLICATION_DEFAULT_FLAGS);
    g_application_set_resource_base_path(G_APPLICATION(app), "/io/github/gabriel2silva/Haptika");
    g_signal_connect(app, "startup", G_CALLBACK(on_startup), NULL);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);


    int status = g_application_run(G_APPLICATION(app), argc, argv);

    g_object_unref(app);
    SDL_Quit();
    return status;
}
