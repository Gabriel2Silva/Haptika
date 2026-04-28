#include "button_indicator.h"

struct _ConttestButtonIndicator {
    GtkWidget parent_instance;
    GtkWidget *label_widget;
    gboolean pressed;
};

G_DEFINE_TYPE(ConttestButtonIndicator, conttest_button_indicator, GTK_TYPE_WIDGET)

static void conttest_button_indicator_dispose(GObject *object) {
    ConttestButtonIndicator *self = CONTTEST_BUTTON_INDICATOR(object);
    if (self->label_widget) {
        gtk_widget_unparent(self->label_widget);
        self->label_widget = NULL;
    }
    G_OBJECT_CLASS(conttest_button_indicator_parent_class)->dispose(object);
}

static void conttest_button_indicator_measure(GtkWidget *widget, GtkOrientation orientation, int for_size, int *minimum, int *natural, int *minimum_baseline, int *natural_baseline) {
    ConttestButtonIndicator *self = CONTTEST_BUTTON_INDICATOR(widget);
    gtk_widget_measure(self->label_widget, orientation, for_size, minimum, natural, minimum_baseline, natural_baseline);
}

static void conttest_button_indicator_size_allocate(GtkWidget *widget, int width, int height, int baseline) {
    ConttestButtonIndicator *self = CONTTEST_BUTTON_INDICATOR(widget);
    gtk_widget_size_allocate(self->label_widget, &(GtkAllocation){0, 0, width, height}, baseline);
}

static void conttest_button_indicator_init(ConttestButtonIndicator *self) {
    self->label_widget = gtk_label_new("");
    gtk_widget_add_css_class(self->label_widget, "button-label");
    gtk_widget_set_parent(self->label_widget, GTK_WIDGET(self));
    
    gtk_widget_add_css_class(GTK_WIDGET(self), "button-indicator");
}

static void conttest_button_indicator_class_init(ConttestButtonIndicatorClass *klass) {
    GObjectClass *oclass = G_OBJECT_CLASS(klass);
    GtkWidgetClass *wclass = GTK_WIDGET_CLASS(klass);

    oclass->dispose = conttest_button_indicator_dispose;
    wclass->measure = conttest_button_indicator_measure;
    wclass->size_allocate = conttest_button_indicator_size_allocate;

    gtk_widget_class_set_accessible_role(wclass, GTK_ACCESSIBLE_ROLE_METER);
}

GtkWidget *conttest_button_indicator_new(const char *label) {
    ConttestButtonIndicator *self = g_object_new(CONTTEST_TYPE_BUTTON_INDICATOR, NULL);
    gtk_label_set_text(GTK_LABEL(self->label_widget), label);
    gtk_accessible_update_property(GTK_ACCESSIBLE(self),
        GTK_ACCESSIBLE_PROPERTY_LABEL, label,
        -1);
    return GTK_WIDGET(self);
}

void conttest_button_indicator_set_pressed(ConttestButtonIndicator *self, gboolean pressed) {
    if (self->pressed == pressed) return;
    self->pressed = pressed;
    if (pressed) {
        gtk_widget_add_css_class(GTK_WIDGET(self), "pressed");
    } else {
        gtk_widget_remove_css_class(GTK_WIDGET(self), "pressed");
    }
}
