#include "trigger_bar.h"
#include "drawing_utils.h"
#include <math.h>

#define TRIGGER_MIN_WIDTH  40
#define TRIGGER_MIN_HEIGHT 124
#define TRIGGER_NAT_WIDTH  40
#define TRIGGER_NAT_HEIGHT 124
#define TRIGGER_BAR_WIDTH  6.0f

struct _ConttestTriggerBar {
    GtkWidget  parent_instance;
    GtkWidget *label;
    int        canvas_w;
    int        canvas_h;
    double     value;
    gboolean   bipolar;
};

G_DEFINE_TYPE(ConttestTriggerBar, conttest_trigger_bar, GTK_TYPE_WIDGET)

static void update_accessible_value(ConttestTriggerBar *self) {
    double vmin = self->bipolar ? -1.0 : 0.0;
    double vmax = 1.0;
    double vnow = !isfinite(self->value) ? 0.0 : CLAMP(self->value, vmin, vmax);
    gtk_accessible_update_property(GTK_ACCESSIBLE(self),
                                   GTK_ACCESSIBLE_PROPERTY_VALUE_MIN, vmin,
                                   GTK_ACCESSIBLE_PROPERTY_VALUE_MAX, vmax,
                                   GTK_ACCESSIBLE_PROPERTY_VALUE_NOW, vnow,
                                   -1);
}

static void conttest_trigger_bar_dispose(GObject *object) {
    ConttestTriggerBar *self = CONTTEST_TRIGGER_BAR(object);
    g_clear_pointer(&self->label, gtk_widget_unparent);
    G_OBJECT_CLASS(conttest_trigger_bar_parent_class)->dispose(object);
}

static void conttest_trigger_bar_measure(GtkWidget *widget,
                                         GtkOrientation orientation,
                                         int for_size,
                                         int *minimum,
                                         int *natural,
                                         int *minimum_baseline,
                                         int *natural_baseline) {
    ConttestTriggerBar *self = CONTTEST_TRIGGER_BAR(widget);
    conttest_captioned_widget_measure(self->label, orientation, for_size,
                                      TRIGGER_MIN_WIDTH, TRIGGER_MIN_HEIGHT,
                                      TRIGGER_NAT_WIDTH, TRIGGER_NAT_HEIGHT,
                                      minimum, natural,
                                      minimum_baseline, natural_baseline);
}

static void conttest_trigger_bar_size_allocate(GtkWidget *widget,
                                               int width,
                                               int height,
                                               int baseline) {
    (void)baseline;
    ConttestTriggerBar *self = CONTTEST_TRIGGER_BAR(widget);
    conttest_captioned_widget_allocate(self->label, width, height,
                                       &self->canvas_w, &self->canvas_h);
}

static void conttest_trigger_bar_snapshot(GtkWidget *widget, GtkSnapshot *snapshot) {
    ConttestTriggerBar *self = CONTTEST_TRIGGER_BAR(widget);

    float cw = (float)self->canvas_w;
    float ch = (float)self->canvas_h;
    float bar_w = MIN(TRIGGER_BAR_WIDTH, MAX(4.0f, cw - 4.0f));
    float vpad = 2.0f;
    float bar_h = ch - vpad * 2.0f;

    if (bar_h > 8.0f && cw >= bar_w) {
        float x0 = (cw - bar_w) * 0.5f;
        float y0 = vpad;
        float radius = bar_w * 0.5f;

        GdkRGBA fg;
        gtk_widget_get_color(widget, &fg);

        GdkRGBA accent;
        conttest_widget_get_accent_color(&accent);
        gboolean hc = conttest_widget_get_high_contrast();

        GskRoundedRect track_rr;
        gsk_rounded_rect_init_from_rect(&track_rr,
                                        &GRAPHENE_RECT_INIT(x0, y0, bar_w, bar_h),
                                        radius);

        g_autoptr(GskPathBuilder) track_builder = gsk_path_builder_new();
        gsk_path_builder_add_rounded_rect(track_builder, &track_rr);
        g_autoptr(GskPath) track_path =
            gsk_path_builder_free_to_path(g_steal_pointer(&track_builder));

        GdkRGBA track_col = fg;
        track_col.alpha *= hc ? 0.22f : 0.14f;
        gtk_snapshot_append_fill(snapshot, track_path, GSK_FILL_RULE_WINDING, &track_col);

        if (hc) {
            GdkRGBA rim_col = fg;
            rim_col.alpha *= 0.55f;
            g_autoptr(GskStroke) rim_stroke = gsk_stroke_new(1.0f);
            gtk_snapshot_append_stroke(snapshot, track_path, rim_stroke, &rim_col);
        }

        double vmin = self->bipolar ? -1.0 : 0.0;
        double v = !isfinite(self->value) ? 0.0 : CLAMP(self->value, vmin, 1.0);

        gtk_snapshot_push_rounded_clip(snapshot, &track_rr);
        if (!self->bipolar) {
            if (v > 0.001) {
                float fill_h = (float)v * bar_h;
                graphene_rect_t fill_rect = GRAPHENE_RECT_INIT(x0, y0 + bar_h - fill_h, bar_w, fill_h);
                gtk_snapshot_append_color(snapshot, &accent, &fill_rect);
            }
        } else {
            float mid_y = y0 + bar_h * 0.5f;
            float half_h = bar_h * 0.5f;
            if (v > 0.001) {
                float fill_h = (float)v * half_h;
                graphene_rect_t fill_rect = GRAPHENE_RECT_INIT(x0, mid_y - fill_h, bar_w, fill_h);
                gtk_snapshot_append_color(snapshot, &accent, &fill_rect);
            } else if (v < -0.001) {
                float fill_h = (float)(-v) * half_h;
                graphene_rect_t fill_rect = GRAPHENE_RECT_INIT(x0, mid_y, bar_w, fill_h);
                gtk_snapshot_append_color(snapshot, &accent, &fill_rect);
            }
        }
        gtk_snapshot_pop(snapshot);

        if (self->bipolar) {
            float mid_y = y0 + bar_h * 0.5f;
            g_autoptr(GskPathBuilder) zero_builder = gsk_path_builder_new();
            gsk_path_builder_move_to(zero_builder, x0 - 3.0f, mid_y);
            gsk_path_builder_line_to(zero_builder, x0 + bar_w + 3.0f, mid_y);
            g_autoptr(GskPath) zero_path =
                gsk_path_builder_free_to_path(g_steal_pointer(&zero_builder));

            GdkRGBA zero_col = fg;
            zero_col.alpha *= hc ? 0.80f : 0.45f;
            g_autoptr(GskStroke) zero_stroke = gsk_stroke_new(1.5f);
            gtk_snapshot_append_stroke(snapshot, zero_path, zero_stroke, &zero_col);
        }
    }

    if (self->label && gtk_widget_get_visible(self->label))
        gtk_widget_snapshot_child(widget, self->label, snapshot);
}

static void conttest_trigger_bar_init(ConttestTriggerBar *self) {
    gtk_widget_set_vexpand(GTK_WIDGET(self), TRUE);
    self->label = conttest_captioned_widget_create_label(GTK_WIDGET(self), NULL);
    conttest_widget_connect_style_redraw(GTK_WIDGET(self));
    update_accessible_value(self);
}

static void conttest_trigger_bar_class_init(ConttestTriggerBarClass *klass) {
    GObjectClass *oclass = G_OBJECT_CLASS(klass);
    GtkWidgetClass *wclass = GTK_WIDGET_CLASS(klass);

    oclass->dispose = conttest_trigger_bar_dispose;
    wclass->measure = conttest_trigger_bar_measure;
    wclass->size_allocate = conttest_trigger_bar_size_allocate;
    wclass->snapshot = conttest_trigger_bar_snapshot;

    gtk_widget_class_set_accessible_role(wclass, GTK_ACCESSIBLE_ROLE_METER);
}

GtkWidget *conttest_trigger_bar_new(const char *name) {
    ConttestTriggerBar *self = g_object_new(CONTTEST_TYPE_TRIGGER_BAR, NULL);
    conttest_captioned_widget_set_caption(GTK_WIDGET(self), self->label, name);
    return GTK_WIDGET(self);
}

void conttest_trigger_bar_set_value(ConttestTriggerBar *self, double value) {
    g_return_if_fail(CONTTEST_IS_TRIGGER_BAR(self));
    if (!isfinite(value))
        return;

    double vmin = self->bipolar ? -1.0 : 0.0;
    double clamped = CLAMP(value, vmin, 1.0);
    if (fabs(clamped - self->value) < 0.001)
        return;

    self->value = clamped;
    update_accessible_value(self);
    gtk_widget_queue_draw(GTK_WIDGET(self));
}

void conttest_trigger_bar_set_bipolar(ConttestTriggerBar *self, gboolean bipolar) {
    g_return_if_fail(CONTTEST_IS_TRIGGER_BAR(self));
    gboolean norm_bipolar = !!bipolar;
    if (self->bipolar == norm_bipolar)
        return;

    self->bipolar = norm_bipolar;
    self->value = CLAMP(self->value, norm_bipolar ? -1.0 : 0.0, 1.0);
    update_accessible_value(self);
    gtk_widget_queue_draw(GTK_WIDGET(self));
}
