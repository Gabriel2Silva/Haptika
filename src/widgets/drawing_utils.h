#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define CONTTEST_WIDGET_CAPTION_SPACING 4

/**
 * conttest_widget_get_accent_color:
 * @out: (out): destination RGBA color
 *
 * Fetches the active Adwaita accent color, falling back to Adwaita Blue when
 * the style manager cannot provide one.
 */
void conttest_widget_get_accent_color(GdkRGBA *out);

/**
 * conttest_widget_get_high_contrast:
 *
 * Returns: %TRUE if high-contrast mode is active in the default AdwStyleManager.
 */
gboolean conttest_widget_get_high_contrast(void);

/**
 * conttest_widget_connect_style_redraw:
 * @widget: widget to queue draws on
 *
 * Redraws @widget whenever accent color, dark preference, or high-contrast
 * mode changes in the default AdwStyleManager.
 */
void conttest_widget_connect_style_redraw(GtkWidget *widget);

/**
 * conttest_captioned_widget_create_label:
 * @parent: custom GtkWidget parent
 * @caption: (nullable): initial caption text
 *
 * Creates and parents a centered `.caption` label for a custom visualizer widget.
 */
GtkWidget *conttest_captioned_widget_create_label(GtkWidget *parent,
                                                  const char *caption);

/**
 * conttest_captioned_widget_set_caption:
 * @widget: custom GtkWidget whose accessible label mirrors @caption
 * @label: caption GtkLabel child
 * @caption: (nullable): new caption text
 *
 * Updates the caption label text, visibility, and widget accessible label.
 */
void conttest_captioned_widget_set_caption(GtkWidget *widget,
                                           GtkWidget *label,
                                           const char *caption);

/**
 * conttest_captioned_widget_measure:
 *
 * Implements standard GtkWidget::measure layout math for a custom widget
 * containing a top canvas above a centered caption label.
 */
void conttest_captioned_widget_measure(GtkWidget *label,
                                       GtkOrientation orientation,
                                       int for_size,
                                       int min_canvas_w,
                                       int min_canvas_h,
                                       int nat_canvas_w,
                                       int nat_canvas_h,
                                       int *minimum,
                                       int *natural,
                                       int *minimum_baseline,
                                       int *natural_baseline);

/**
 * conttest_captioned_widget_allocate:
 *
 * Allocates the bottom caption label within (width, height) and returns the
 * remaining top canvas dimensions in @out_canvas_w and @out_canvas_h.
 */
void conttest_captioned_widget_allocate(GtkWidget *label,
                                        int width,
                                        int height,
                                        int *out_canvas_w,
                                        int *out_canvas_h);

/**
 * conttest_snapshot_circular_indicator:
 *
 * Renders a GPU-accelerated circular crosshair indicator with a tethered,
 * clamped accent dot at normalized position (@x, @y) in [-1, 1].
 */
void conttest_snapshot_circular_indicator(GtkWidget *widget,
                                          GtkSnapshot *snapshot,
                                          float canvas_w,
                                          float canvas_h,
                                          float x,
                                          float y);

G_END_DECLS
