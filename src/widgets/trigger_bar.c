#include "trigger_bar.h"

struct _ConttestTriggerBar {
    GtkBox parent_instance;
    GtkWidget *label;
    GtkWidget *progress;
};

G_DEFINE_TYPE(ConttestTriggerBar, conttest_trigger_bar, GTK_TYPE_BOX)

static void conttest_trigger_bar_init(ConttestTriggerBar *self) {
    gtk_orientable_set_orientation(GTK_ORIENTABLE(self), GTK_ORIENTATION_VERTICAL);
    gtk_box_set_spacing(GTK_BOX(self), 4);

    self->progress = gtk_progress_bar_new();
    gtk_orientable_set_orientation(GTK_ORIENTABLE(self->progress), GTK_ORIENTATION_VERTICAL);
    gtk_progress_bar_set_inverted(GTK_PROGRESS_BAR(self->progress), TRUE); // Fill from bottom up
    gtk_widget_set_vexpand(self->progress, TRUE);
    gtk_box_append(GTK_BOX(self), self->progress);

    self->label = gtk_label_new("");
    gtk_widget_add_css_class(self->label, "caption");
    gtk_box_append(GTK_BOX(self), self->label);
}

static void conttest_trigger_bar_class_init(ConttestTriggerBarClass *klass) {
    (void)klass;
}

GtkWidget *conttest_trigger_bar_new(const char *name) {
    ConttestTriggerBar *self = g_object_new(CONTTEST_TYPE_TRIGGER_BAR, NULL);
    gtk_label_set_text(GTK_LABEL(self->label), name);
    gtk_accessible_update_property(GTK_ACCESSIBLE(self->progress),
        GTK_ACCESSIBLE_PROPERTY_LABEL, name,
        -1);
    return GTK_WIDGET(self);
}

void conttest_trigger_bar_set_value(ConttestTriggerBar *self, double value) {
    double clamped = CLAMP(value, 0.0, 1.0);
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(self->progress), clamped);
}
