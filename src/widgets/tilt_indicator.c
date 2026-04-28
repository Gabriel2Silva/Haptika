#include "tilt_indicator.h"
#include <pango/pangocairo.h>
#include <math.h>

struct _ConttestTiltIndicator {
    GtkWidget  parent_instance;
    GtkWidget *drawing_area;
    float      sax, say, saz; /* EMA-smoothed values */
    PangoLayout *layout;
};

G_DEFINE_TYPE(ConttestTiltIndicator, conttest_tilt_indicator, GTK_TYPE_WIDGET)

static void draw_tilt(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer user_data) {
    ConttestTiltIndicator *self = CONTTEST_TILT_INDICATOR(user_data);
    (void)area;

    double cx = width / 2.0, cy = height / 2.0;
    double radius = MIN(cx, cy) - 2.0;

    /* Boundary circle */
    cairo_set_source_rgba(cr, 0.5, 0.5, 0.5, 0.3);
    cairo_arc(cr, cx, cy, radius, 0, 2 * G_PI);
    cairo_stroke(cr);

    /* Crosshair */
    cairo_set_source_rgba(cr, 0.5, 0.5, 0.5, 0.1);
    cairo_move_to(cr, cx, cy - radius); cairo_line_to(cr, cx, cy + radius);
    cairo_move_to(cr, cx - radius, cy); cairo_line_to(cr, cx + radius, cy);
    cairo_stroke(cr);

    /* Derive roll and pitch from accelerometer.
     * roll  = tilt left/right  → maps to X axis of the circle
     * pitch = tilt forward/back → maps to Y axis of the circle
     * Clamp to ±1 so the dot stays inside. */
    float mag = sqrtf(self->sax * self->sax + self->say * self->say + self->saz * self->saz);
    double norm_x = 0.0, norm_y = 0.0;
    if (mag > 0.1f) {
        norm_x = CLAMP(-self->sax / mag, -1.0, 1.0);  /* roll  (invert: tilt left → dot left) */
        norm_y = CLAMP(-self->say / mag, -1.0, 1.0);  /* pitch (invert so forward = up) */
    }

    double dot_r = radius * 0.15;
    double travel = radius - dot_r;
    double dx = norm_x * travel;
    double dy = norm_y * travel;
    /* Clamp to circle boundary */
    double dist = sqrt(dx * dx + dy * dy);
    if (dist > travel) { dx *= travel / dist; dy *= travel / dist; }

    cairo_set_source_rgba(cr, 0.2, 0.6, 1.0, 0.8);
    cairo_arc(cr, cx + dx, cy + dy, dot_r, 0, 2 * G_PI);
    cairo_fill(cr);

    /* Label */
    if (self->layout) {
        pango_cairo_update_layout(cr, self->layout);
        int text_width;
        pango_layout_get_pixel_size(self->layout, &text_width, NULL);

        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.5);
        cairo_move_to(cr, cx - text_width / 2.0, cy + radius + 4.0);
        pango_cairo_show_layout(cr, self->layout);
    }
}

static void conttest_tilt_indicator_dispose(GObject *object) {
    ConttestTiltIndicator *self = CONTTEST_TILT_INDICATOR(object);
    if (self->drawing_area) { gtk_widget_unparent(self->drawing_area); self->drawing_area = NULL; }
    g_clear_object(&self->layout);
    G_OBJECT_CLASS(conttest_tilt_indicator_parent_class)->dispose(object);
}

static void conttest_tilt_indicator_measure(GtkWidget *widget, GtkOrientation orientation,
                                            int for_size, int *minimum, int *natural,
                                            int *minimum_baseline, int *natural_baseline) {
    (void)widget; (void)for_size; (void)minimum_baseline; (void)natural_baseline; (void)orientation;
    *minimum = *natural = 100;
}

static void conttest_tilt_indicator_size_allocate(GtkWidget *widget, int width, int height, int baseline) {
    ConttestTiltIndicator *self = CONTTEST_TILT_INDICATOR(widget);
    gtk_widget_size_allocate(self->drawing_area, &(GtkAllocation){0, 0, width, height}, baseline);
}

static void conttest_tilt_indicator_init(ConttestTiltIndicator *self) {
    self->saz = -9.81f; /* rest position: gravity pointing down */

    self->drawing_area = gtk_drawing_area_new();
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(self->drawing_area), draw_tilt, self, NULL);
    gtk_widget_set_parent(self->drawing_area, GTK_WIDGET(self));

    self->layout = gtk_widget_create_pango_layout(GTK_WIDGET(self), "Tilt");
    PangoFontDescription *desc = pango_font_description_from_string("Sans 10");
    pango_layout_set_font_description(self->layout, desc);
    pango_font_description_free(desc);
}

static void conttest_tilt_indicator_class_init(ConttestTiltIndicatorClass *klass) {
    G_OBJECT_CLASS(klass)->dispose = conttest_tilt_indicator_dispose;
    GtkWidgetClass *wclass = GTK_WIDGET_CLASS(klass);
    wclass->measure       = conttest_tilt_indicator_measure;
    wclass->size_allocate = conttest_tilt_indicator_size_allocate;

    gtk_widget_class_set_accessible_role(wclass, GTK_ACCESSIBLE_ROLE_IMG);
}

GtkWidget *conttest_tilt_indicator_new(void) {
    GtkWidget *w = GTK_WIDGET(g_object_new(CONTTEST_TYPE_TILT_INDICATOR, NULL));
    gtk_accessible_update_property(GTK_ACCESSIBLE(w),
        GTK_ACCESSIBLE_PROPERTY_LABEL, "Tilt",
        -1);
    return w;
}

void conttest_tilt_indicator_set_accel(ConttestTiltIndicator *self, float ax, float ay, float az) {
    /* EMA smoothing: α=0.15 keeps the dot stable while still feeling responsive */

    #define TILT_ALPHA 0.15f
    self->sax = TILT_ALPHA * ax + (1.0f - TILT_ALPHA) * self->sax;
    self->say = TILT_ALPHA * ay + (1.0f - TILT_ALPHA) * self->say;
    self->saz = TILT_ALPHA * az + (1.0f - TILT_ALPHA) * self->saz;
    #undef TILT_ALPHA
    gtk_widget_queue_draw(self->drawing_area);
}
