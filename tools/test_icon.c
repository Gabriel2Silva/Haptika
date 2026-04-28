/*
 * test_icon — verify that the Haptika app icon is discoverable by GTK.
 *
 * Usage: test_icon [icon-search-dir]
 *   If no argument is given, only the default theme paths are checked.
 *   Pass the project's data/icons directory to test an uninstalled build.
 */
#include <adwaita.h>
#include <stdio.h>

int main(int argc, char *argv[]) {
    gtk_init();
    GdkDisplay *display = gdk_display_get_default();
    GtkIconTheme *theme = gtk_icon_theme_get_for_display(display);

    if (argc > 1) {
        gtk_icon_theme_add_search_path(theme, argv[1]);
        printf("Added search path: %s\n", argv[1]);
    }

    char **paths = gtk_icon_theme_get_search_path(theme);
    printf("Search paths:\n");
    for (int i = 0; paths[i]; i++)
        printf("  %s\n", paths[i]);
    g_strfreev(paths);

    const char *name = "io.github.gabriel2silva.Haptika";
    gboolean has = gtk_icon_theme_has_icon(theme, name);
    printf("\nHas icon '%s': %s\n", name, has ? "YES" : "NO");

    if (has) {
        GtkIconPaintable *icon = gtk_icon_theme_lookup_icon(
            theme, name, NULL, 128, 1, GTK_TEXT_DIR_NONE, 0);
        if (icon) {
            GFile *file = gtk_icon_paintable_get_file(icon);
            if (file) {
                char *path = g_file_get_path(file);
                printf("Icon file: %s\n", path ? path : "(resource)");
                g_free(path);
                g_object_unref(file);
            }
            g_object_unref(icon);
        }
    }

    return has ? 0 : 1;
}
