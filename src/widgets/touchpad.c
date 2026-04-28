#include "touchpad.h"
#include <pango/pangocairo.h>
#include <math.h>

/* DS4 touchpad aspect ratio: 1920 x 943 ≈ 2.035 : 1 */
#define TOUCHPAD_ASPECT (1920.0 / 943.0)

struct _ConttestTouchpad {
    GtkWidget parent_instance;
    GtkWidget *drawing_area;
    gboolean   finger_active;
    float      finger_x; /* normalized 0–1 */
    float      finger_y; /* normalized 0–1 */
    PangoLayout *layout;
};

G_DEFINE_TYPE(ConttestTouchpad, conttest_touchpad, GTK_TYPE_WIDGET)

static void draw_touchpad(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer user_data) {
    ConttestTouchpad *self = CONTTEST_TOUCHPAD(user_data);
    (void)area;

    const double pad   = 2.0;
    const double r     = 8.0; /* corner radius */
    const double x0    = pad, y0 = pad;
    const double w     = width  - pad * 2;
    const double h     = height - pad * 2;

    /* Rounded rectangle */
    cairo_new_sub_path(cr);
    cairo_arc(cr, x0 + w - r, y0 + r,     r, -G_PI_2, 0);
    cairo_arc(cr, x0 + w - r, y0 + h - r, r,  0,      G_PI_2);
    cairo_arc(cr, x0 + r,     y0 + h - r, r,  G_PI_2, G_PI);
    cairo_arc(cr, x0 + r,     y0 + r,     r,  G_PI,   3 * G_PI_2);
    cairo_close_path(cr);

    cairo_set_source_rgba(cr, 0.5, 0.5, 0.5, 0.15);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 0.5, 0.5, 0.5, 0.4);
    cairo_set_line_width(cr, 1.5);
    cairo_stroke(cr);

    /* Finger dot */
    if (self->finger_active) {
        const double dot_r = MIN(w, h) * 0.12;
        double cx = x0 + dot_r + self->finger_x * (w - dot_r * 2);
        double cy = y0 + dot_r + self->finger_y * (h - dot_r * 2);

        cairo_set_source_rgba(cr, 0.13, 0.59, 0.95, 0.75);
        cairo_arc(cr, cx, cy, dot_r, 0, 2 * G_PI);
        cairo_fill(cr);

        cairo_set_source_rgba(cr, 0.2, 0.75, 1.0, 0.9);
        cairo_set_line_width(cr, 1.5);
        cairo_arc(cr, cx, cy, dot_r, 0, 2 * G_PI);
        cairo_stroke(cr);
    }

    /* Label */
    if (self->layout) {
        pango_cairo_update_layout(cr, self->layout);
        int text_width;
        pango_layout_get_pixel_size(self->layout, &text_width, NULL);

        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.5);
        cairo_move_to(cr, x0 + (w - text_width) / 2.0, y0 + h + 4.0);
        pango_cairo_show_layout(cr, self->layout);
    }
}

static void conttest_touchpad_dispose(GObject *object) {
    ConttestTouchpad *self = CONTTEST_TOUCHPAD(object);
    if (self->drawing_area) {
        gtk_widget_unparent(self->drawing_area);
        self->drawing_area = NULL;
    }
    g_clear_object(&self->layout);
    G_OBJECT_CLASS(conttest_touchpad_parent_class)->dispose(object);
}

static void conttest_touchpad_measure(GtkWidget *widget, GtkOrientation orientation,
                                      int for_size, int *minimum, int *natural,
                                      int *minimum_baseline, int *natural_baseline) {
    (void)widget; (void)minimum_baseline; (void)natural_baseline;
    if (orientation == GTK_ORIENTATION_HORIZONTAL) {
        int base = (for_size > 0) ? (int)(for_size * TOUCHPAD_ASPECT) : 200;
        *minimum = *natural = base;
    } else {
        int base = (for_size > 0) ? (int)(for_size / TOUCHPAD_ASPECT) : 98;
        *minimum = *natural = base;
    }
}

static void conttest_touchpad_size_allocate(GtkWidget *widget, int width, int height, int baseline) {
    ConttestTouchpad *self = CONTTEST_TOUCHPAD(widget);
    gtk_widget_size_allocate(self->drawing_area,
                             &(GtkAllocation){0, 0, width, height}, baseline);
}

static void conttest_touchpad_init(ConttestTouchpad *self) {
    self->drawing_area = gtk_drawing_area_new();
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(self->drawing_area), draw_touchpad, self, NULL);
    gtk_widget_set_parent(self->drawing_area, GTK_WIDGET(self));

    self->layout = gtk_widget_create_pango_layout(GTK_WIDGET(self), "Touchpad");
    PangoFontDescription *desc = pango_font_description_from_string("Sans 10");
    pango_layout_set_font_description(self->layout, desc);
    pango_font_description_free(desc);
}

static void conttest_touchpad_class_init(ConttestTouchpadClass *klass) {
    GObjectClass *oclass = G_OBJECT_CLASS(klass);
    GtkWidgetClass *wclass = GTK_WIDGET_CLASS(klass);
    oclass->dispose        = conttest_touchpad_dispose;
    wclass->measure        = conttest_touchpad_measure;
    wclass->size_allocate  = conttest_touchpad_size_allocate;

    gtk_widget_class_set_accessible_role(wclass, GTK_ACCESSIBLE_ROLE_IMG);
}

GtkWidget *conttest_touchpad_new(void) {
    GtkWidget *w = GTK_WIDGET(g_object_new(CONTTEST_TYPE_TOUCHPAD, NULL));
    gtk_accessible_update_property(GTK_ACCESSIBLE(w),
        GTK_ACCESSIBLE_PROPERTY_LABEL, "Touchpad",
        -1);
    return w;
}

void conttest_touchpad_set_finger(ConttestTouchpad *self, gboolean active, float x, float y) {
    self->finger_active = active;
    self->finger_x = x;
    self->finger_y = y;
    gtk_widget_queue_draw(self->drawing_area);
}
