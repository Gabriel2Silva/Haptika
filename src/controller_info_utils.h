#pragma once
#include <gtk/gtk.h>
#include <adwaita.h>

G_BEGIN_DECLS

struct udev;

/* Creates an AdwDialog with a toolbar view and header bar.
 * When `loading` is TRUE, shows a centered AdwSpinner until
 * controller_info_dialog_show_content() replaces it.
 * out_toolbar_view (if non-NULL) receives the toolbar. */
AdwDialog *controller_info_dialog_new(const char *title, int width, int height,
                                      gboolean loading,
                                      GtkWidget **out_toolbar_view);

/* Replaces the toolbar view content with a scrolled clamped vertical box.
 * Moves initial dialog focus to the scrolled container so non-activatable
 * property rows do not show an initial focus outline or pre-select text. */
GtkWidget *controller_info_dialog_show_content(GtkWidget *toolbar_view);

/* Replaces the toolbar view content with an AdwStatusPage error state. */
void controller_info_dialog_show_error(GtkWidget *toolbar_view,
                                       const char *title,
                                       const char *description);

/* Presents a simple alert dialog with an OK response. */
void controller_info_show_error_alert(GtkWidget *parent,
                                      const char *heading,
                                      const char *body);

/* Read a little-endian 16-bit value using unsigned shifts to avoid signed-shift UB. */
static inline guint16 controller_info_read_le16(const guint8 *p) {
    return (guint16)((guint32)p[0] | ((guint32)p[1] << 8));
}

/* Read a little-endian 32-bit value using unsigned shifts to avoid signed-shift UB. */
static inline guint32 controller_info_read_le32(const guint8 *p) {
    return (guint32)p[0] |
           ((guint32)p[1] << 8) |
           ((guint32)p[2] << 16) |
           ((guint32)p[3] << 24);
}

/* Find the hidraw devnode for the device at evdev_syspath.
 * udev must be a valid, caller-owned udev context.
 * Returns a newly-allocated string or NULL. Caller must g_free(). */
char *controller_info_find_hidraw_node(struct udev *udev,
                                       const char *evdev_syspath);

/* Send a HIDIOCGFEATURE ioctl and return a newly-allocated buffer, or NULL.
 * Validates both ioctl success and minimum byte count returned (min_len).
 * If out_len is non-NULL, stores the actual byte count returned by ioctl. */
guint8 *controller_info_hidraw_get_feature(int fd, guint8 report_id,
                                           gsize buf_size, gsize min_len,
                                           gsize *out_len);

/* Sanitize a raw HID string into valid printable UTF-8 with control chars
 * and trailing whitespace removed. Stops at embedded NUL bytes within max_len
 * (pass max_len < 0 for NUL-terminated input). Caller must g_free(). */
char *controller_info_sanitize_hid_string(const char *raw, gssize max_len);

/* Sanitize and copy UTF-8 safely into a fixed-size destination buffer without
 * splitting multi-byte sequences. */
void controller_info_copy_utf8(char *dst, gsize dst_size, const char *src);

/* Format into a fixed-size buffer using g_vsnprintf and log on truncation. */
void controller_info_format(char *dst, gsize dst_size, const char *fmt, ...) G_GNUC_PRINTF(3, 4);

typedef gpointer (*ControllerInfoFetchFunc)(const char *path,
                                            gboolean is_bt,
                                            GCancellable *cancellable);

/* Present a spinner dialog and fetch hardware info on a GTask worker thread.
 * When resolve_hidraw is TRUE, evdev_syspath is resolved off-thread to a hidraw
 * devnode via controller_info_find_hidraw_node() before invoking fetch_func;
 * otherwise evdev_syspath is passed directly to fetch_func.
 * Closing or destroying the dialog cancels the task and ignores late results.
 * done_callback receives a borrowed AdwDialog* as user_data. */
void controller_info_present_async_dialog(GtkWidget *parent,
                                          const char *title,
                                          int width,
                                          int height,
                                          const char *evdev_syspath,
                                          gboolean resolve_hidraw,
                                          gboolean is_bt,
                                          ControllerInfoFetchFunc fetch_func,
                                          GDestroyNotify result_free_func,
                                          GAsyncReadyCallback done_callback);

typedef enum {
    CONTROLLER_INFO_STATUS_NONE = 0,
    CONTROLLER_INFO_STATUS_SUCCESS,
    CONTROLLER_INFO_STATUS_ERROR
} ControllerInfoStatus;

/* Adds a selectable .property AdwActionRow to group with title and value
 * (no-op if value is NULL or empty). */
void controller_info_add_row(AdwPreferencesGroup *group,
                              const char *title, const char *value);

/* Adds a selectable .property AdwActionRow with optional semantic status styling
 * and suffix icon (no-op if value is NULL or empty). */
void controller_info_add_status_row(AdwPreferencesGroup *group,
                                     const char *title, const char *value,
                                     ControllerInfoStatus status);

G_END_DECLS
