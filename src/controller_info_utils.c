#include "controller_info_utils.h"
#include <glib/gi18n.h>
#include <libudev.h>
#include <linux/hidraw.h>
#include <stdarg.h>
#include <string.h>
#include <sys/ioctl.h>

static void weak_ref_free(gpointer p) {
    GWeakRef *ref = p;
    g_weak_ref_clear(ref);
    g_free(ref);
}

static void set_toolbar_dialog_focus(GtkWidget *toolbar_view, GtkWidget *target) {
    GtkWidget *dialog = gtk_widget_get_ancestor(toolbar_view, ADW_TYPE_DIALOG);
    if (dialog) {
        adw_dialog_set_focus(ADW_DIALOG(dialog), target);
        return;
    }
    GWeakRef *ref = g_object_get_data(G_OBJECT(toolbar_view), "haptika-dialog");
    AdwDialog *weak_dialog = ref ? g_weak_ref_get(ref) : NULL;
    if (weak_dialog) {
        adw_dialog_set_focus(weak_dialog, target);
        g_object_unref(weak_dialog);
    }
}

AdwDialog *controller_info_dialog_new(const char *title, int width, int height,
                                      gboolean loading,
                                      GtkWidget **out_toolbar_view) {
    AdwDialog *dialog = ADW_DIALOG(adw_dialog_new());
    adw_dialog_set_title(dialog, title);
    if (width > 0) adw_dialog_set_content_width(dialog, width);
    if (height > 0) adw_dialog_set_content_height(dialog, height);

    GtkWidget *toolbar_view = adw_toolbar_view_new();
    adw_dialog_set_child(dialog, toolbar_view);

    /* AdwDialog only parents its child widget once presented; keep a weak ref
     * so synchronous callers can configure initial dialog focus beforehand. */
    GWeakRef *dialog_ref = g_new(GWeakRef, 1);
    g_weak_ref_init(dialog_ref, dialog);
    g_object_set_data_full(G_OBJECT(toolbar_view), "haptika-dialog",
                           dialog_ref, weak_ref_free);

    GtkWidget *header = adw_header_bar_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar_view), header);

    if (loading) {
        GtkWidget *spinner = adw_spinner_new();
        gtk_widget_set_valign(spinner, GTK_ALIGN_CENTER);
        gtk_widget_set_halign(spinner, GTK_ALIGN_CENTER);
        adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar_view), spinner);
    }

    if (out_toolbar_view)
        *out_toolbar_view = toolbar_view;

    return dialog;
}

GtkWidget *controller_info_dialog_show_content(GtkWidget *toolbar_view) {
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar_view), scroll);

    GtkWidget *clamp = adw_clamp_new();
    adw_clamp_set_maximum_size(ADW_CLAMP(clamp), 480);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), clamp);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 24);
    gtk_widget_set_margin_top(box, 24);
    gtk_widget_set_margin_bottom(box, 24);
    gtk_widget_set_margin_start(box, 12);
    gtk_widget_set_margin_end(box, 12);
    adw_clamp_set_child(ADW_CLAMP(clamp), box);

    set_toolbar_dialog_focus(toolbar_view, scroll);

    return box;
}

void controller_info_dialog_show_error(GtkWidget *toolbar_view,
                                       const char *title,
                                       const char *description) {
    GtkWidget *status = adw_status_page_new();
    adw_status_page_set_icon_name(ADW_STATUS_PAGE(status), "dialog-error-symbolic");
    adw_status_page_set_title(ADW_STATUS_PAGE(status), title);
    if (description && *description) {
        /* Driver and device error text is untrusted; the description is Pango
         * markup, so escape it to render literally. */
        g_autofree char *escaped = g_markup_escape_text(description, -1);
        adw_status_page_set_description(ADW_STATUS_PAGE(status), escaped);
    }
    gtk_widget_add_css_class(status, "compact");
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar_view), status);

    set_toolbar_dialog_focus(toolbar_view, status);
}

void controller_info_show_error_alert(GtkWidget *parent,
                                      const char *heading,
                                      const char *body) {
    AdwDialog *d = ADW_DIALOG(adw_alert_dialog_new(heading, NULL));
    adw_alert_dialog_set_body(ADW_ALERT_DIALOG(d), body);
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(d), "ok", _("OK"));
    adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(d), "ok");
    adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(d), "ok");
    adw_dialog_present(d, parent);
}

char *controller_info_find_hidraw_node(struct udev *udev,
                                       const char *evdev_syspath) {
    if (!udev || !evdev_syspath) return NULL;
    char *result = NULL;

    struct udev_device *evdev = udev_device_new_from_syspath(udev, evdev_syspath);
    if (!evdev) return NULL;

    struct udev_device *hid = udev_device_get_parent_with_subsystem_devtype(evdev, "hid", NULL);
    if (!hid) goto out_evdev;

    struct udev_enumerate *en = udev_enumerate_new(udev);
    if (!en) goto out_evdev;
    udev_enumerate_add_match_subsystem(en, "hidraw");
    udev_enumerate_add_match_parent(en, hid);
    udev_enumerate_scan_devices(en);

    struct udev_list_entry *entry = udev_enumerate_get_list_entry(en);
    if (entry) {
        const char *path = udev_list_entry_get_name(entry);
        struct udev_device *hr = udev_device_new_from_syspath(udev, path);
        if (hr) {
            const char *node = udev_device_get_devnode(hr);
            if (node) result = g_strdup(node);
            udev_device_unref(hr);
        }
    }
    udev_enumerate_unref(en);

out_evdev:
    udev_device_unref(evdev);
    return result;
}

guint8 *controller_info_hidraw_get_feature(int fd, guint8 report_id,
                                           gsize buf_size, gsize min_len,
                                           gsize *out_len) {
    if (out_len)
        *out_len = 0;
    if (fd < 0 || buf_size == 0 || buf_size > 4096 || min_len > buf_size)
        return NULL;

    guint8 *buf = g_malloc0(buf_size);
    buf[0] = report_id;
    int ret = ioctl(fd, HIDIOCGFEATURE(buf_size), buf);
    if (ret < 0 || (gsize)ret < min_len) {
        g_free(buf);
        return NULL;
    }
    if (out_len)
        *out_len = (gsize)ret;
    return buf;
}

char *controller_info_sanitize_hid_string(const char *raw, gssize max_len) {
    if (!raw)
        return NULL;

    gsize len;
    if (max_len < 0) {
        len = strlen(raw);
    } else {
        const void *nul = memchr(raw, '\0', (gsize)max_len);
        len = nul ? (gsize)((const char *)nul - raw) : (gsize)max_len;
    }

    char *valid = g_utf8_validate(raw, (gssize)len, NULL)
        ? g_strndup(raw, len)
        : g_utf8_make_valid(raw, (gssize)len);

    GString *clean = g_string_sized_new(strlen(valid));
    for (const char *p = valid; *p; p = g_utf8_next_char(p)) {
        gunichar ch = g_utf8_get_char(p);
        if (!g_unichar_iscntrl(ch))
            g_string_append_unichar(clean, ch);
    }
    g_free(valid);
    g_strchomp(clean->str);
    return g_string_free(clean, FALSE);
}

void controller_info_copy_utf8(char *dst, gsize dst_size, const char *src) {
    if (!dst || dst_size == 0)
        return;
    if (!src) {
        dst[0] = '\0';
        return;
    }

    char *sanitized = controller_info_sanitize_hid_string(src, -1);
    gsize max_bytes = dst_size - 1;
    gsize slen = strlen(sanitized);
    if (slen > max_bytes) {
        /* Step back before any UTF-8 continuation bytes so truncation stays valid UTF-8. */
        const char *end = sanitized + max_bytes;
        while (end > sanitized && (((guint8)*end & 0xc0u) == 0x80u))
            end--;
        slen = (gsize)(end - sanitized);
    }
    memcpy(dst, sanitized, slen);
    dst[slen] = '\0';
    g_free(sanitized);
}

void controller_info_format(char *dst, gsize dst_size, const char *fmt, ...) {
    if (!dst || dst_size == 0)
        return;
    va_list args;
    va_start(args, fmt);
    int n = g_vsnprintf(dst, (gulong)dst_size, fmt, args);
    va_end(args);
    if (n < 0 || (gsize)n >= dst_size)
        g_debug("Formatted controller info field truncated (%zu bytes)", dst_size);
}

typedef struct {
    char *evdev_syspath;
    gboolean resolve_hidraw;
    gboolean is_bt;
    ControllerInfoFetchFunc fetch_func;
    GDestroyNotify result_free_func;
} AsyncFetchData;

typedef struct {
    AdwDialog *dialog;
    GCancellable *cancellable;
    GAsyncReadyCallback user_callback;
    guint timeout_id;
    gboolean timed_out;
} AsyncUiContext;

/* HIDIOCGFEATURE/HIDIOCSFEATURE can block in the kernel driver and cannot be
 * interrupted. Bound how long the dialog waits (and holds its reference) even
 * though the worker thread may remain blocked until the driver returns. */
#define CONTROLLER_INFO_FETCH_TIMEOUT_SEC 5

static void cancel_and_unref(gpointer p) {
    GCancellable *c = G_CANCELLABLE(p);
    g_cancellable_cancel(c);
    g_object_unref(c);
}

static void async_fetch_data_free(gpointer p) {
    AsyncFetchData *d = p;
    g_free(d->evdev_syspath);
    g_free(d);
}

static gboolean on_fetch_timeout(gpointer user_data) {
    AsyncUiContext *ui = user_data;
    ui->timeout_id = 0;
    ui->timed_out = TRUE;
    g_cancellable_cancel(ui->cancellable);
    if (ui->dialog) {
        controller_info_dialog_show_error(
            adw_dialog_get_child(ui->dialog),
            _("Request Timed Out"),
            _("The controller did not answer the hardware query in time."));
        /* Release the dialog now; a worker stuck in the driver must not keep it
         * alive after the user closes it. */
        g_clear_object(&ui->dialog);
    }
    return G_SOURCE_REMOVE;
}

static void async_fetch_thread(GTask *task, gpointer source,
                               gpointer task_data, GCancellable *cancellable) {
    (void)source;
    AsyncFetchData *d = task_data;
    if (g_cancellable_is_cancelled(cancellable)) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Cancelled");
        return;
    }

    char *resolved_hidraw = NULL;
    const char *path_arg = d->evdev_syspath;
    if (d->resolve_hidraw) {
        struct udev *udev = udev_new();
        resolved_hidraw = (udev && d->evdev_syspath)
            ? controller_info_find_hidraw_node(udev, d->evdev_syspath)
            : NULL;
        if (udev) udev_unref(udev);

        if (g_cancellable_is_cancelled(cancellable)) {
            g_free(resolved_hidraw);
            g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Cancelled");
            return;
        }
        if (!resolved_hidraw) {
            g_task_return_new_error(
                task, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "%s",
                _("Could not find hidraw device. You may need to add a udev rule to allow access."));
            return;
        }
        path_arg = resolved_hidraw;
    }

    gpointer res = d->fetch_func(path_arg, d->is_bt, cancellable);
    g_free(resolved_hidraw);
    g_task_return_pointer(task, res, d->result_free_func);
}

static void on_async_task_done(GObject *source, GAsyncResult *result, gpointer user_data) {
    AsyncUiContext *ui = (AsyncUiContext *)user_data;
    if (ui->timeout_id) {
        g_source_remove(ui->timeout_id);
        ui->timeout_id = 0;
    }
    if (!ui->timed_out && ui->dialog &&
        !g_cancellable_is_cancelled(ui->cancellable) && ui->user_callback)
        ui->user_callback(source, result, ui->dialog);

    g_clear_object(&ui->dialog);
    g_object_unref(ui->cancellable);
    g_free(ui);
}

void controller_info_present_async_dialog(GtkWidget *parent,
                                          const char *title,
                                          int width,
                                          int height,
                                          const char *evdev_syspath,
                                          gboolean resolve_hidraw,
                                          gboolean is_bt,
                                          ControllerInfoFetchFunc fetch_func,
                                          GDestroyNotify result_free_func,
                                          GAsyncReadyCallback done_callback) {
    AdwDialog *dialog = controller_info_dialog_new(title, width, height, TRUE, NULL);
    GCancellable *cancellable = g_cancellable_new();
    g_signal_connect_object(dialog, "closed", G_CALLBACK(g_cancellable_cancel),
                            cancellable, G_CONNECT_SWAPPED);
    g_object_set_data_full(G_OBJECT(dialog), "conttest-async-cancellable",
                           g_object_ref(cancellable), cancel_and_unref);

    adw_dialog_present(dialog, parent);

    AsyncFetchData *td = g_new(AsyncFetchData, 1);
    td->evdev_syspath = g_strdup(evdev_syspath);
    td->resolve_hidraw = resolve_hidraw;
    td->is_bt = is_bt;
    td->fetch_func = fetch_func;
    td->result_free_func = result_free_func;

    AsyncUiContext *ui = g_new(AsyncUiContext, 1);
    ui->dialog = g_object_ref(dialog);
    ui->cancellable = g_object_ref(cancellable);
    ui->user_callback = done_callback;
    ui->timed_out = FALSE;
    ui->timeout_id = g_timeout_add_seconds_full(G_PRIORITY_DEFAULT,
                                                CONTROLLER_INFO_FETCH_TIMEOUT_SEC,
                                                on_fetch_timeout, ui, NULL);

    GTask *task = g_task_new(NULL, cancellable, on_async_task_done, ui);
    g_task_set_task_data(task, td, async_fetch_data_free);
    g_task_run_in_thread(task, async_fetch_thread);
    g_object_unref(task);
    g_object_unref(cancellable);
}

void controller_info_add_row(AdwPreferencesGroup *group,
                              const char *title, const char *value) {
    controller_info_add_status_row(group, title, value,
                                   CONTROLLER_INFO_STATUS_NONE);
}

void controller_info_add_status_row(AdwPreferencesGroup *group,
                                     const char *title, const char *value,
                                     ControllerInfoStatus status) {
    if (!value || !*value)
        return;

    AdwActionRow *row = ADW_ACTION_ROW(adw_action_row_new());
    adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    adw_action_row_set_subtitle(row, value);
    adw_action_row_set_subtitle_selectable(row, TRUE);
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
    gtk_widget_set_focusable(GTK_WIDGET(row), FALSE);
    gtk_widget_add_css_class(GTK_WIDGET(row), "property");

    const char *icon_name = NULL;
    const char *css_class = NULL;
    switch (status) {
        case CONTROLLER_INFO_STATUS_SUCCESS:
            icon_name = "object-select-symbolic";
            css_class = "success";
            break;
        case CONTROLLER_INFO_STATUS_ERROR:
            icon_name = "dialog-warning-symbolic";
            css_class = "error";
            break;
        case CONTROLLER_INFO_STATUS_NONE:
        default:
            break;
    }

    if (icon_name && css_class) {
        gtk_widget_add_css_class(GTK_WIDGET(row), css_class);
        GtkWidget *icon = gtk_image_new_from_icon_name(icon_name);
        gtk_widget_set_valign(icon, GTK_ALIGN_CENTER);
        gtk_widget_add_css_class(icon, css_class);
        gtk_accessible_update_state(GTK_ACCESSIBLE(icon),
                                    GTK_ACCESSIBLE_STATE_HIDDEN, TRUE,
                                    -1);
        adw_action_row_add_suffix(row, icon);
    }

    adw_preferences_group_add(group, GTK_WIDGET(row));
}
