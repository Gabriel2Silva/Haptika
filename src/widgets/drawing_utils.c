#include "drawing_utils.h"
#include <adwaita.h>
#include <math.h>

static gboolean style_cache_connected = FALSE;
static gboolean style_cache_dirty     = TRUE;
static GdkRGBA  cached_accent_color   = { 0.2078f, 0.5176f, 0.8941f, 1.0f };
static gboolean cached_high_contrast  = FALSE;

static void on_style_cache_invalidated(GObject *sm, GParamSpec *pspec, gpointer user_data) {
    (void)sm;
    (void)pspec;
    (void)user_data;
    style_cache_dirty = TRUE;
}

static void ensure_style_cache(void) {
    AdwStyleManager *sm = adw_style_manager_get_default();
    if (!sm) {
        cached_accent_color  = (GdkRGBA){ 0.2078f, 0.5176f, 0.8941f, 1.0f };
        cached_high_contrast = FALSE;
        return;
    }

    if (!style_cache_connected) {
        style_cache_connected = TRUE;
        style_cache_dirty     = TRUE;
        g_signal_connect(sm, "notify::accent-color",
                         G_CALLBACK(on_style_cache_invalidated), NULL);
        g_signal_connect(sm, "notify::dark",
                         G_CALLBACK(on_style_cache_invalidated), NULL);
        g_signal_connect(sm, "notify::high-contrast",
                         G_CALLBACK(on_style_cache_invalidated), NULL);
    }

    if (style_cache_dirty) {
        GdkRGBA *accent = adw_style_manager_get_accent_color_rgba(sm);
        if (accent) {
            cached_accent_color = *accent;
            gdk_rgba_free(accent);
        } else {
            /* Fallback to Adwaita Blue (#3584e4) */
            cached_accent_color = (GdkRGBA){ 0.2078f, 0.5176f, 0.8941f, 1.0f };
        }
        cached_high_contrast = adw_style_manager_get_high_contrast(sm);
        style_cache_dirty    = FALSE;
    }
}

static void on_style_manager_changed(GObject *sm, GParamSpec *pspec, gpointer user_data) {
    (void)sm;
    (void)pspec;
    style_cache_dirty = TRUE;
    gtk_widget_queue_draw(GTK_WIDGET(user_data));
}

void conttest_widget_get_accent_color(GdkRGBA *out) {
    g_return_if_fail(out != NULL);
    ensure_style_cache();
    *out = cached_accent_color;
}

gboolean conttest_widget_get_high_contrast(void) {
    ensure_style_cache();
    return cached_high_contrast;
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

void conttest_circular_indicator_cache_clear(ConttestCircularIndicatorCache *cache) {
    if (!cache)
        return;
    g_clear_pointer(&cache->ring_path, gsk_path_unref);
    g_clear_pointer(&cache->cross_path, gsk_path_unref);
    g_clear_pointer(&cache->stroke_1_5, gsk_stroke_free);
    g_clear_pointer(&cache->stroke_1_0, gsk_stroke_free);
    cache->canvas_w = 0.0f;
    cache->canvas_h = 0.0f;
}

static void ensure_circular_cache(ConttestCircularIndicatorCache *cache,
                                  float canvas_w,
                                  float canvas_h,
                                  float cx,
                                  float cy,
                                  float radius) {
    if (!cache->stroke_1_5)
        cache->stroke_1_5 = gsk_stroke_new(1.5f);
    if (!cache->stroke_1_0)
        cache->stroke_1_0 = gsk_stroke_new(1.0f);

    if (cache->ring_path && cache->cross_path &&
        cache->canvas_w == canvas_w && cache->canvas_h == canvas_h) {
        return;
    }

    g_clear_pointer(&cache->ring_path, gsk_path_unref);
    g_clear_pointer(&cache->cross_path, gsk_path_unref);

    GskPathBuilder *ring_builder = gsk_path_builder_new();
    gsk_path_builder_add_circle(ring_builder, &GRAPHENE_POINT_INIT(cx, cy), radius);
    cache->ring_path = gsk_path_builder_free_to_path(ring_builder);

    GskPathBuilder *cross_builder = gsk_path_builder_new();
    gsk_path_builder_move_to(cross_builder, cx, cy - radius);
    gsk_path_builder_line_to(cross_builder, cx, cy + radius);
    gsk_path_builder_move_to(cross_builder, cx - radius, cy);
    gsk_path_builder_line_to(cross_builder, cx + radius, cy);
    cache->cross_path = gsk_path_builder_free_to_path(cross_builder);

    cache->canvas_w = canvas_w;
    cache->canvas_h = canvas_h;
}

void conttest_snapshot_circular_indicator_cached(GtkWidget *widget,
                                                 GtkSnapshot *snapshot,
                                                 ConttestCircularIndicatorCache *cache,
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

    ConttestCircularIndicatorCache temp_cache = {0};
    ConttestCircularIndicatorCache *c = cache ? cache : &temp_cache;
    ensure_circular_cache(c, canvas_w, canvas_h, cx, cy, radius);

    /* Trough disk and outer rim */
    GdkRGBA bg_col = fg;
    bg_col.alpha *= hc ? 0.14f : 0.06f;
    gtk_snapshot_append_fill(snapshot, c->ring_path, GSK_FILL_RULE_WINDING, &bg_col);

    GdkRGBA rim_col = fg;
    rim_col.alpha *= hc ? 0.55f : 0.30f;
    gtk_snapshot_append_stroke(snapshot, c->ring_path, c->stroke_1_5, &rim_col);

    /* Crosshair axes */
    GdkRGBA cross_col = fg;
    cross_col.alpha *= hc ? 0.32f : 0.14f;
    gtk_snapshot_append_stroke(snapshot, c->cross_path, c->stroke_1_0, &cross_col);

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
        dist = travel;
    }

    if (dist > 1.0f) {
        GdkRGBA tether_col = accent;
        tether_col.alpha *= 0.45f;
        float angle_deg = atan2f(dy, dx) * (180.0f / (float)G_PI);
        gtk_snapshot_save(snapshot);
        gtk_snapshot_translate(snapshot, &GRAPHENE_POINT_INIT(cx, cy));
        gtk_snapshot_rotate(snapshot, angle_deg);
        gtk_snapshot_append_color(snapshot, &tether_col,
                                  &GRAPHENE_RECT_INIT(0.0f, -0.75f, dist, 1.5f));
        gtk_snapshot_restore(snapshot);
    }

    GskRoundedRect dot_rr;
    gsk_rounded_rect_init_from_rect(
        &dot_rr,
        &GRAPHENE_RECT_INIT(cx + dx - dot_r, cy + dy - dot_r,
                            dot_r * 2.0f, dot_r * 2.0f),
        dot_r);

    GdkRGBA dot_col = accent;
    dot_col.alpha *= 0.90f;
    gtk_snapshot_push_rounded_clip(snapshot, &dot_rr);
    gtk_snapshot_append_color(snapshot, &dot_col, &dot_rr.bounds);
    gtk_snapshot_pop(snapshot);

    if (hc) {
        GskRoundedRect rim_rr;
        gsk_rounded_rect_init_from_rect(
            &rim_rr,
            &GRAPHENE_RECT_INIT(cx + dx - dot_r - 0.5f, cy + dy - dot_r - 0.5f,
                                dot_r * 2.0f + 1.0f, dot_r * 2.0f + 1.0f),
            dot_r + 0.5f);
        const float widths[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
        const GdkRGBA colors[4] = { fg, fg, fg, fg };
        gtk_snapshot_append_border(snapshot, &rim_rr, widths, colors);
    }

    if (!cache)
        conttest_circular_indicator_cache_clear(&temp_cache);
}
