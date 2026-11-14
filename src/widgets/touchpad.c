#include "touchpad.h"
#include "drawing_utils.h"
#include <glib/gi18n.h>
#include <math.h>

/* DS4/DualSense touchpad aspect ratio: 1920 x 943 ~ 2.035 : 1 */
#define TOUCHPAD_MIN_WIDTH  200
#define TOUCHPAD_MIN_HEIGHT  98
#define TOUCHPAD_NAT_WIDTH  200
#define TOUCHPAD_NAT_HEIGHT  98

typedef struct {
    gboolean active;
    float    x;
    float    y;
} TouchpadFingerState;

struct _ConttestTouchpad {
    GtkWidget           parent_instance;
    GtkWidget          *label;
    int                 canvas_w;
    int                 canvas_h;
    TouchpadFingerState fingers[CONTTEST_TOUCHPAD_MAX_FINGERS];
};

G_DEFINE_TYPE(ConttestTouchpad, conttest_touchpad, GTK_TYPE_WIDGET)

static void conttest_touchpad_dispose(GObject *object) {
    ConttestTouchpad *self = CONTTEST_TOUCHPAD(object);
    g_clear_pointer(&self->label, gtk_widget_unparent);
    G_OBJECT_CLASS(conttest_touchpad_parent_class)->dispose(object);
}

static void conttest_touchpad_measure(GtkWidget *widget,
                                      GtkOrientation orientation,
                                      int for_size,
                                      int *minimum,
                                      int *natural,
                                      int *minimum_baseline,
                                      int *natural_baseline) {
    ConttestTouchpad *self = CONTTEST_TOUCHPAD(widget);
    conttest_captioned_widget_measure(self->label, orientation, for_size,
                                      TOUCHPAD_MIN_WIDTH, TOUCHPAD_MIN_HEIGHT,
                                      TOUCHPAD_NAT_WIDTH, TOUCHPAD_NAT_HEIGHT,
                                      minimum, natural,
                                      minimum_baseline, natural_baseline);
}

static void conttest_touchpad_size_allocate(GtkWidget *widget,
                                            int width,
                                            int height,
                                            int baseline) {
    (void)baseline;
    ConttestTouchpad *self = CONTTEST_TOUCHPAD(widget);
    conttest_captioned_widget_allocate(self->label, width, height,
                                       &self->canvas_w, &self->canvas_h);
}

static void conttest_touchpad_snapshot(GtkWidget *widget, GtkSnapshot *snapshot) {
    ConttestTouchpad *self = CONTTEST_TOUCHPAD(widget);

    const float pad = 2.0f;
    const float r = 8.0f;
    float max_w = MIN((float)self->canvas_w, (float)TOUCHPAD_NAT_WIDTH) - pad * 2.0f;
    float max_h = (float)self->canvas_h - pad * 2.0f;
    float plate_w = max_w;
    float plate_h = MIN(max_h, plate_w * ((float)TOUCHPAD_NAT_HEIGHT / (float)TOUCHPAD_NAT_WIDTH));
    float x0 = ((float)self->canvas_w - plate_w) * 0.5f;
    float y0 = pad + (max_h - plate_h) * 0.5f;

    if (plate_w > r * 2.0f && plate_h > r * 2.0f) {
        GdkRGBA fg;
        gtk_widget_get_color(widget, &fg);

        GdkRGBA accent;
        conttest_widget_get_accent_color(&accent);
        gboolean hc = conttest_widget_get_high_contrast();

        GskRoundedRect rr;
        gsk_rounded_rect_init_from_rect(&rr,
                                        &GRAPHENE_RECT_INIT(x0, y0, plate_w, plate_h),
                                        r);

        g_autoptr(GskPathBuilder) plate_builder = gsk_path_builder_new();
        gsk_path_builder_add_rounded_rect(plate_builder, &rr);
        g_autoptr(GskPath) plate_path =
            gsk_path_builder_free_to_path(g_steal_pointer(&plate_builder));

        GdkRGBA fill_col = fg;
        fill_col.alpha *= hc ? 0.16f : 0.10f;
        gtk_snapshot_append_fill(snapshot, plate_path, GSK_FILL_RULE_WINDING, &fill_col);

        GdkRGBA stroke_col = fg;
        stroke_col.alpha *= hc ? 0.60f : 0.35f;
        g_autoptr(GskStroke) border_stroke = gsk_stroke_new(1.5f);
        gtk_snapshot_append_stroke(snapshot, plate_path, border_stroke, &stroke_col);

        /* Active touch contacts (multi-finger) */
        float dot_r = MIN(plate_w, plate_h) * 0.12f;
        for (guint i = 0; i < CONTTEST_TOUCHPAD_MAX_FINGERS; i++) {
            const TouchpadFingerState *finger = &self->fingers[i];
            if (!finger->active)
                continue;

            float cx = x0 + dot_r + finger->x * (plate_w - dot_r * 2.0f);
            float cy = y0 + dot_r + finger->y * (plate_h - dot_r * 2.0f);

            g_autoptr(GskPathBuilder) dot_builder = gsk_path_builder_new();
            gsk_path_builder_add_circle(dot_builder,
                                        &GRAPHENE_POINT_INIT(cx, cy),
                                        dot_r);
            g_autoptr(GskPath) dot_path =
                gsk_path_builder_free_to_path(g_steal_pointer(&dot_builder));

            GdkRGBA dot_col = accent;
            dot_col.alpha *= (i == 0) ? 0.85f : 0.65f;
            gtk_snapshot_append_fill(snapshot, dot_path, GSK_FILL_RULE_WINDING, &dot_col);

            GdkRGBA ring_col = (i == 0) ? fg : accent;
            ring_col.alpha *= (i == 0) ? (hc ? 0.85f : 0.28f) : 0.95f;
            g_autoptr(GskStroke) dot_stroke = gsk_stroke_new((i == 0) ? 1.5f : 2.0f);
            gtk_snapshot_append_stroke(snapshot, dot_path, dot_stroke, &ring_col);

            if (i > 0) {
                /* Secondary contacts get an outer concentric halo for quick disambiguation */
                g_autoptr(GskPathBuilder) halo_builder = gsk_path_builder_new();
                gsk_path_builder_add_circle(halo_builder,
                                            &GRAPHENE_POINT_INIT(cx, cy),
                                            dot_r + 3.0f);
                g_autoptr(GskPath) halo_path =
                    gsk_path_builder_free_to_path(g_steal_pointer(&halo_builder));
                GdkRGBA halo_col = accent;
                halo_col.alpha *= 0.45f;
                g_autoptr(GskStroke) halo_stroke = gsk_stroke_new(1.0f);
                gtk_snapshot_append_stroke(snapshot, halo_path, halo_stroke, &halo_col);
            }
        }
    }

    if (self->label && gtk_widget_get_visible(self->label))
        gtk_widget_snapshot_child(widget, self->label, snapshot);
}

static void conttest_touchpad_init(ConttestTouchpad *self) {
    self->label = conttest_captioned_widget_create_label(GTK_WIDGET(self), _("Touchpad"));
    conttest_widget_connect_style_redraw(GTK_WIDGET(self));
}

static void conttest_touchpad_class_init(ConttestTouchpadClass *klass) {
    GObjectClass *oclass = G_OBJECT_CLASS(klass);
    GtkWidgetClass *wclass = GTK_WIDGET_CLASS(klass);

    oclass->dispose = conttest_touchpad_dispose;
    wclass->measure = conttest_touchpad_measure;
    wclass->size_allocate = conttest_touchpad_size_allocate;
    wclass->snapshot = conttest_touchpad_snapshot;

    gtk_widget_class_set_accessible_role(wclass, GTK_ACCESSIBLE_ROLE_IMG);
}

GtkWidget *conttest_touchpad_new(void) {
    return GTK_WIDGET(g_object_new(CONTTEST_TYPE_TOUCHPAD, NULL));
}

void conttest_touchpad_set_finger(ConttestTouchpad *self, gboolean active, float x, float y) {
    conttest_touchpad_set_finger_indexed(self, 0, active, x, y);
}

void conttest_touchpad_set_finger_indexed(ConttestTouchpad *self,
                                          guint finger_idx,
                                          gboolean active,
                                          float x,
                                          float y) {
    g_return_if_fail(CONTTEST_IS_TOUCHPAD(self));
    g_return_if_fail(finger_idx < CONTTEST_TOUCHPAD_MAX_FINGERS);

    TouchpadFingerState *slot = &self->fingers[finger_idx];
    float fx = isnan(x) ? 0.5f : CLAMP(x, 0.0f, 1.0f);
    float fy = isnan(y) ? 0.5f : CLAMP(y, 0.0f, 1.0f);

    if (!active && !slot->active)
        return;
    if (active && slot->active &&
        fabsf(fx - slot->x) < 0.001f &&
        fabsf(fy - slot->y) < 0.001f) {
        return;
    }

    slot->active = active;
    slot->x = fx;
    slot->y = fy;
    gtk_widget_queue_draw(GTK_WIDGET(self));
}
