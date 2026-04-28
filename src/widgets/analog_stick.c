#include "analog_stick.h"
#include <pango/pangocairo.h>
#include <math.h>

struct _ConttestAnalogStick {
    GtkWidget parent_instance;
    GtkWidget *drawing_area;
    double stick_x;
    double stick_y;
    char *name;
    PangoLayout *layout;
};

G_DEFINE_TYPE(ConttestAnalogStick, conttest_analog_stick, GTK_TYPE_WIDGET)

static void draw_analog_stick(GtkDrawingArea *drawing_area, cairo_t *cr, int width, int height, gpointer user_data) {
    ConttestAnalogStick *self = CONTTEST_ANALOG_STICK(user_data);
    (void)drawing_area;

    double center_x = width / 2.0;
    double center_y = height / 2.0;

    double radius = MIN(center_x, center_y) - 2.0;

    // Draw boundary circle
    cairo_set_source_rgba(cr, 0.5, 0.5, 0.5, 0.3);
    cairo_arc(cr, center_x, center_y, radius, 0, 2 * G_PI);
    cairo_stroke(cr);

    // Draw crosshair
    cairo_set_source_rgba(cr, 0.5, 0.5, 0.5, 0.1);
    cairo_move_to(cr, center_x, center_y - radius);
    cairo_line_to(cr, center_x, center_y + radius);
    cairo_move_to(cr, center_x - radius, center_y);
    cairo_line_to(cr, center_x + radius, center_y);
    cairo_stroke(cr);

    // Draw stick dot — clamp travel so the dot stays inside the boundary.
    double dot_radius = radius * 0.15;
    double travel = radius - dot_radius;

    double raw_x = self->stick_x * travel;
    double raw_y = self->stick_y * travel;
    double dist = sqrt(raw_x * raw_x + raw_y * raw_y);
    if (dist > travel) {
        raw_x *= travel / dist;
        raw_y *= travel / dist;
    }

    double dot_x = center_x + raw_x;
    double dot_y = center_y + raw_y;

    cairo_set_source_rgba(cr, 0.2, 0.6, 1.0, 0.8);
    cairo_arc(cr, dot_x, dot_y, dot_radius, 0, 2 * G_PI);
    cairo_fill(cr);

    // Draw label
    if (self->layout) {
        pango_cairo_update_layout(cr, self->layout);
        int text_width;
        pango_layout_get_pixel_size(self->layout, &text_width, NULL);

        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.5);
        cairo_move_to(cr, center_x - text_width / 2.0, center_y + radius + 4.0);
        pango_cairo_show_layout(cr, self->layout);
    }
}

static void conttest_analog_stick_dispose(GObject *object) {
    ConttestAnalogStick *self = CONTTEST_ANALOG_STICK(object);
    if (self->drawing_area) {
        gtk_widget_unparent(self->drawing_area);
        self->drawing_area = NULL;
    }
    g_free(self->name);
    self->name = NULL;
    g_clear_object(&self->layout);
    G_OBJECT_CLASS(conttest_analog_stick_parent_class)->dispose(object);
}

static void conttest_analog_stick_measure(GtkWidget *widget, GtkOrientation orientation, int for_size, int *minimum, int *natural, int *minimum_baseline, int *natural_baseline) {
    (void)widget; (void)orientation; (void)for_size; (void)minimum_baseline; (void)natural_baseline;
    *minimum = *natural = 100;
}

static void conttest_analog_stick_size_allocate(GtkWidget *widget, int width, int height, int baseline) {
    ConttestAnalogStick *self = CONTTEST_ANALOG_STICK(widget);
    gtk_widget_size_allocate(self->drawing_area, &(GtkAllocation){0, 0, width, height}, baseline);
}

static void conttest_analog_stick_init(ConttestAnalogStick *self) {
    self->drawing_area = gtk_drawing_area_new();
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(self->drawing_area), draw_analog_stick, self, NULL);
    gtk_widget_set_parent(self->drawing_area, GTK_WIDGET(self));
}

static void conttest_analog_stick_class_init(ConttestAnalogStickClass *klass) {
    GObjectClass *oclass = G_OBJECT_CLASS(klass);
    GtkWidgetClass *wclass = GTK_WIDGET_CLASS(klass);

    oclass->dispose = conttest_analog_stick_dispose;
    wclass->measure = conttest_analog_stick_measure;
    wclass->size_allocate = conttest_analog_stick_size_allocate;

    gtk_widget_class_set_accessible_role(wclass, GTK_ACCESSIBLE_ROLE_IMG);
}

GtkWidget *conttest_analog_stick_new(const char *name) {
    ConttestAnalogStick *self = g_object_new(CONTTEST_TYPE_ANALOG_STICK, NULL);
    if (name) {
        self->name = g_strdup(name);
        self->layout = gtk_widget_create_pango_layout(GTK_WIDGET(self), self->name);
        PangoFontDescription *desc = pango_font_description_from_string("Sans 10");
        pango_layout_set_font_description(self->layout, desc);
        pango_font_description_free(desc);

        gtk_accessible_update_property(GTK_ACCESSIBLE(self),
            GTK_ACCESSIBLE_PROPERTY_LABEL, name,
            -1);
    }
    return GTK_WIDGET(self);
}

void conttest_analog_stick_set_position(ConttestAnalogStick *self, double x, double y) {
    if (fabs(x - self->stick_x) < 0.005 && fabs(y - self->stick_y) < 0.005) return;
    self->stick_x = x;
    self->stick_y = y;
    gtk_widget_queue_draw(self->drawing_area);
}
