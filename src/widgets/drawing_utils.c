#include "drawing_utils.h"
#include <adwaita.h>
#include <math.h>

static void on_style_manager_changed(GObject *sm, GParamSpec *pspec, gpointer user_data) {
    (void)sm;
    (void)pspec;
    gtk_widget_queue_draw(GTK_WIDGET(user_data));
}

void conttest_widget_get_accent_color(GdkRGBA *out) {
    g_return_if_fail(out != NULL);

    AdwStyleManager *sm = adw_style_manager_get_default();
    GdkRGBA *accent = sm ? adw_style_manager_get_accent_color_rgba(sm) : NULL;
    if (accent) {
        *out = *accent;
        gdk_rgba_free(accent);
    } else {
        /* Fallback to Adwaita Blue (#3584e4) */
        *out = (GdkRGBA){ 0.2078f, 0.5176f, 0.8941f, 1.0f };
    }
}

gboolean conttest_widget_get_high_contrast(void) {
    AdwStyleManager *sm = adw_style_manager_get_default();
    return sm ? adw_style_manager_get_high_contrast(sm) : FALSE;
}

void conttest_widget_connect_style_redraw(GtkWidget *widget) {
    g_return_if_fail(GTK_IS_WIDGET(widget));

    AdwStyleManager *sm = adw_style_manager_get_default();
    if (!sm)
        return;

    g_signal_connect_object(sm, "notify::accent-color",
                            G_CALLBACK(on_style_manager_changed), widget, G_CONNECT_DEFAULT);
    g_signal_connect_object(sm, "notify::dark",
                            G_CALLBACK(on_style_manager_changed), widget, G_CONNECT_DEFAULT);
    g_signal_connect_object(sm, "notify::high-contrast",
                            G_CALLBACK(on_style_manager_changed), widget, G_CONNECT_DEFAULT);
}

GtkWidget *conttest_captioned_widget_create_label(GtkWidget *parent, const char *caption) {
    g_return_val_if_fail(GTK_IS_WIDGET(parent), NULL);

    GtkWidget *label = gtk_label_new("");
    gtk_widget_set_halign(label, GTK_ALIGN_CENTER);
    gtk_label_set_justify(GTK_LABEL(label), GTK_JUSTIFY_CENTER);
    gtk_widget_add_css_class(label, "caption");
    gtk_widget_set_parent(label, parent);
    conttest_captioned_widget_set_caption(parent, label, caption);
    return label;
}

void conttest_captioned_widget_set_caption(GtkWidget *widget,
                                           GtkWidget *label,
                                           const char *caption) {
    g_return_if_fail(GTK_IS_WIDGET(widget));
    g_return_if_fail(GTK_IS_LABEL(label));

    if (caption && *caption) {
        gtk_label_set_text(GTK_LABEL(label), caption);
        gtk_widget_set_visible(label, TRUE);
        gtk_accessible_update_property(GTK_ACCESSIBLE(widget),
                                       GTK_ACCESSIBLE_PROPERTY_LABEL, caption,
                                       -1);
    } else {
        gtk_label_set_text(GTK_LABEL(label), "");
        gtk_widget_set_visible(label, FALSE);
        gtk_accessible_reset_property(GTK_ACCESSIBLE(widget),
                                      GTK_ACCESSIBLE_PROPERTY_LABEL);
    }
}

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
                                       int *natural_baseline) {
    if (minimum_baseline) *minimum_baseline = -1;
    if (natural_baseline) *natural_baseline = -1;

    gboolean label_visible = label && gtk_widget_get_visible(label);

    if (orientation == GTK_ORIENTATION_HORIZONTAL) {
        int lmin = 0, lnat = 0;
        if (label_visible) {
            gtk_widget_measure(label, GTK_ORIENTATION_HORIZONTAL, -1,
                               &lmin, &lnat, NULL, NULL);
        }
        int min_w = MAX(min_canvas_w, lmin);
        int nat_w = MAX(nat_canvas_w, lnat);
        if (minimum) *minimum = min_w;
        if (natural) *natural = MAX(nat_w, min_w);
    } else {
        int lmin = 0, lnat = 0;
        if (label_visible) {
            int eff_for = -1;
            if (for_size >= 0) {
                int lmw = 0;
                gtk_widget_measure(label, GTK_ORIENTATION_HORIZONTAL, -1,
                                   &lmw, NULL, NULL, NULL);
                eff_for = MAX(for_size, lmw);
            }
            gtk_widget_measure(label, GTK_ORIENTATION_VERTICAL, eff_for,
                               &lmin, &lnat, NULL, NULL);
        }
        int spacing = (label_visible && lnat > 0) ? CONTTEST_WIDGET_CAPTION_SPACING : 0;
        int min_h = min_canvas_h + spacing + lmin;
        int nat_h = nat_canvas_h + spacing + lnat;
        if (minimum) *minimum = min_h;
        if (natural) *natural = MAX(nat_h, min_h);
    }
}

void conttest_captioned_widget_allocate(GtkWidget *label,
                                        int width,
                                        int height,
                                        int *out_canvas_w,
                                        int *out_canvas_h) {
    int label_h = 0;
    int spacing = 0;
    gboolean label_visible = label && gtk_widget_get_visible(label);

    if (label_visible) {
        int lmw = 0, lmin = 0, lnat = 0;
        gtk_widget_measure(label, GTK_ORIENTATION_HORIZONTAL, -1,
                           &lmw, NULL, NULL, NULL);
        gtk_widget_measure(label, GTK_ORIENTATION_VERTICAL, MAX(width, lmw),
                           &lmin, &lnat, NULL, NULL);
        label_h = CLAMP(lnat, 0, height);
        if (label_h < lmin && height >= lmin)
            label_h = lmin;
        if (label_h > 0 && height > label_h)
            spacing = MIN(CONTTEST_WIDGET_CAPTION_SPACING, height - label_h);
    }

    int canvas_h = MAX(0, height - label_h - spacing);
    if (out_canvas_w) *out_canvas_w = width;
    if (out_canvas_h) *out_canvas_h = canvas_h;

    if (label_visible) {
        int alloc_w = MAX(0, width);
        GskTransform *tr = gsk_transform_translate(
            NULL, &GRAPHENE_POINT_INIT(0.0f, (float)(canvas_h + spacing)));
        gtk_widget_allocate(label, alloc_w, label_h, -1, tr);
    }
}

void conttest_snapshot_circular_indicator(GtkWidget *widget,
                                          GtkSnapshot *snapshot,
                                          float canvas_w,
                                          float canvas_h,
                                          float x,
                                          float y) {
    g_return_if_fail(GTK_IS_WIDGET(widget));
    g_return_if_fail(snapshot != NULL);

    float cx = canvas_w * 0.5f;
    float cy = canvas_h * 0.5f;
    float radius = MIN(cx, cy) - 2.0f;
    if (radius <= 4.0f)
        return;

    GdkRGBA fg;
    gtk_widget_get_color(widget, &fg);

    GdkRGBA accent;
    conttest_widget_get_accent_color(&accent);
    gboolean hc = conttest_widget_get_high_contrast();

    /* Trough disk and outer rim */
    g_autoptr(GskPathBuilder) ring_builder = gsk_path_builder_new();
    gsk_path_builder_add_circle(ring_builder, &GRAPHENE_POINT_INIT(cx, cy), radius);
    g_autoptr(GskPath) ring_path = gsk_path_builder_free_to_path(g_steal_pointer(&ring_builder));

    GdkRGBA bg_col = fg;
    bg_col.alpha *= hc ? 0.14f : 0.06f;
    gtk_snapshot_append_fill(snapshot, ring_path, GSK_FILL_RULE_WINDING, &bg_col);

    GdkRGBA rim_col = fg;
    rim_col.alpha *= hc ? 0.55f : 0.30f;
    g_autoptr(GskStroke) rim_stroke = gsk_stroke_new(1.5f);
    gtk_snapshot_append_stroke(snapshot, ring_path, rim_stroke, &rim_col);

    /* Crosshair axes */
    g_autoptr(GskPathBuilder) cross_builder = gsk_path_builder_new();
    gsk_path_builder_move_to(cross_builder, cx, cy - radius);
    gsk_path_builder_line_to(cross_builder, cx, cy + radius);
    gsk_path_builder_move_to(cross_builder, cx - radius, cy);
    gsk_path_builder_line_to(cross_builder, cx + radius, cy);
    g_autoptr(GskPath) cross_path = gsk_path_builder_free_to_path(g_steal_pointer(&cross_builder));

    GdkRGBA cross_col = fg;
    cross_col.alpha *= hc ? 0.32f : 0.14f;
    g_autoptr(GskStroke) cross_stroke = gsk_stroke_new(1.0f);
    gtk_snapshot_append_stroke(snapshot, cross_path, cross_stroke, &cross_col);

    /* Moving dot clamped cleanly within the boundary rim */
    float dot_r = radius * 0.15f;
    float travel = radius - dot_r;
    float nx = isfinite(x) ? x : 0.0f;
    float ny = isfinite(y) ? y : 0.0f;
    float dx = nx * travel;
    float dy = ny * travel;
    float dist = hypotf(dx, dy);
    if (!isfinite(dist)) {
        float n_dist = hypotf(nx, ny);
        if (isfinite(n_dist) && n_dist > 0.0f) {
            dx = (nx / n_dist) * travel;
            dy = (ny / n_dist) * travel;
            dist = travel;
        } else {
            dx = 0.0f;
            dy = 0.0f;
            dist = 0.0f;
        }
    } else if (dist > travel && dist > 0.0f) {
        dx *= travel / dist;
        dy *= travel / dist;
    }

    if (dist > 1.0f) {
        g_autoptr(GskPathBuilder) tether_builder = gsk_path_builder_new();
        gsk_path_builder_move_to(tether_builder, cx, cy);
        gsk_path_builder_line_to(tether_builder, cx + dx, cy + dy);
        g_autoptr(GskPath) tether_path =
            gsk_path_builder_free_to_path(g_steal_pointer(&tether_builder));

        GdkRGBA tether_col = accent;
        tether_col.alpha *= 0.45f;
        g_autoptr(GskStroke) tether_stroke = gsk_stroke_new(1.5f);
        gtk_snapshot_append_stroke(snapshot, tether_path, tether_stroke, &tether_col);
    }

    g_autoptr(GskPathBuilder) dot_builder = gsk_path_builder_new();
    gsk_path_builder_add_circle(dot_builder,
                                &GRAPHENE_POINT_INIT(cx + dx, cy + dy),
                                dot_r);
    g_autoptr(GskPath) dot_path = gsk_path_builder_free_to_path(g_steal_pointer(&dot_builder));

    GdkRGBA dot_col = accent;
    dot_col.alpha *= 0.90f;
    gtk_snapshot_append_fill(snapshot, dot_path, GSK_FILL_RULE_WINDING, &dot_col);

    if (hc) {
        g_autoptr(GskStroke) dot_rim = gsk_stroke_new(1.0f);
        gtk_snapshot_append_stroke(snapshot, dot_path, dot_rim, &fg);
    }
}
