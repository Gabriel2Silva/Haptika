#include "button_indicator.h"
#include <glib/gi18n.h>

struct _ConttestButtonIndicator {
    GtkWidget parent_instance;
    GtkWidget *label_widget;
    gboolean pressed;
    gboolean unpainted_press;
    gboolean release_after_paint;
    guint unlatch_tick_id;
};

G_DEFINE_TYPE(ConttestButtonIndicator, conttest_button_indicator, GTK_TYPE_WIDGET)

static gboolean on_unlatch_tick(GtkWidget *widget,
                                GdkFrameClock *frame_clock,
                                gpointer user_data) {
    (void)frame_clock;
    (void)user_data;
    ConttestButtonIndicator *self = CONTTEST_BUTTON_INDICATOR(widget);
    self->unlatch_tick_id = 0;
    self->release_after_paint = FALSE;
    if (!self->pressed) {
        gtk_widget_remove_css_class(widget, "pressed");
        gtk_accessible_update_property(GTK_ACCESSIBLE(self),
            GTK_ACCESSIBLE_PROPERTY_DESCRIPTION, _("Released"),
            -1);
    }
    return G_SOURCE_REMOVE;
}

static void conttest_button_indicator_dispose(GObject *object) {
    ConttestButtonIndicator *self = CONTTEST_BUTTON_INDICATOR(object);
    if (self->unlatch_tick_id != 0) {
        gtk_widget_remove_tick_callback(GTK_WIDGET(self), self->unlatch_tick_id);
        self->unlatch_tick_id = 0;
    }
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

static void conttest_button_indicator_snapshot(GtkWidget *widget, GtkSnapshot *snapshot) {
    ConttestButtonIndicator *self = CONTTEST_BUTTON_INDICATOR(widget);
    GTK_WIDGET_CLASS(conttest_button_indicator_parent_class)->snapshot(widget, snapshot);
    self->unpainted_press = FALSE;
    if (self->release_after_paint && self->unlatch_tick_id == 0) {
        self->unlatch_tick_id = gtk_widget_add_tick_callback(
            widget, on_unlatch_tick, NULL, NULL);
    }
}

static void conttest_button_indicator_class_init(ConttestButtonIndicatorClass *klass) {
    GObjectClass *oclass = G_OBJECT_CLASS(klass);
    GtkWidgetClass *wclass = GTK_WIDGET_CLASS(klass);

    oclass->dispose = conttest_button_indicator_dispose;
    wclass->measure = conttest_button_indicator_measure;
    wclass->size_allocate = conttest_button_indicator_size_allocate;
    wclass->snapshot = conttest_button_indicator_snapshot;

    gtk_widget_class_set_accessible_role(wclass, GTK_ACCESSIBLE_ROLE_GENERIC);
}

GtkWidget *conttest_button_indicator_new(const char *label) {
    ConttestButtonIndicator *self = g_object_new(CONTTEST_TYPE_BUTTON_INDICATOR, NULL);
    gtk_label_set_text(GTK_LABEL(self->label_widget), label);
    gtk_accessible_update_property(GTK_ACCESSIBLE(self),
        GTK_ACCESSIBLE_PROPERTY_LABEL, label,
        GTK_ACCESSIBLE_PROPERTY_DESCRIPTION, _("Released"),
        -1);
    return GTK_WIDGET(self);
}

void conttest_button_indicator_set_pressed(ConttestButtonIndicator *self, gboolean pressed) {
    pressed = !!pressed;
    if (self->pressed == pressed) return;
    self->pressed = pressed;
    if (pressed) {
        self->release_after_paint = FALSE;
        self->unpainted_press = TRUE;
        if (!gtk_widget_has_css_class(GTK_WIDGET(self), "pressed"))
            gtk_widget_add_css_class(GTK_WIDGET(self), "pressed");
        gtk_accessible_update_property(GTK_ACCESSIBLE(self),
            GTK_ACCESSIBLE_PROPERTY_DESCRIPTION, _("Pressed"),
            -1);
    } else if (self->unpainted_press && gtk_widget_get_mapped(GTK_WIDGET(self))) {
        /* Keep .pressed visible for at least one rendered frame so sub-frame
         * taps sampled at 1 kHz are never swallowed between VSync intervals. */
        self->release_after_paint = TRUE;
        gtk_widget_queue_draw(GTK_WIDGET(self));
    } else {
        self->unpainted_press = FALSE;
        self->release_after_paint = FALSE;
        gtk_widget_remove_css_class(GTK_WIDGET(self), "pressed");
        gtk_accessible_update_property(GTK_ACCESSIBLE(self),
            GTK_ACCESSIBLE_PROPERTY_DESCRIPTION, _("Released"),
            -1);
    }
}
