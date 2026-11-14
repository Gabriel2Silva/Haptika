#include "tilt_indicator.h"
#include "drawing_utils.h"
#include <glib/gi18n.h>
#include <math.h>

#define TILT_MIN_SIZE 100
#define TILT_NAT_SIZE 100
/* Frame-rate-independent smoothing time constant (seconds).
 * TAU = 0.10 s reproduces alpha ~ 0.15 at 60 Hz across all refresh rates. */
#define TILT_TAU_SEC  0.10f
#define TILT_EPS      0.01f

struct _ConttestTiltIndicator {
    GtkWidget  parent_instance;
    GtkWidget *label;
    int        canvas_w;
    int        canvas_h;
    float      target_x;
    float      target_y;
    float      target_z;
    float      sax;
    float      say;
    float      saz;
    guint      tick_id;
    gint64     last_frame_time_us;
};

G_DEFINE_TYPE(ConttestTiltIndicator, conttest_tilt_indicator, GTK_TYPE_WIDGET)

static void stop_tilt_tick(ConttestTiltIndicator *self) {
    if (self->tick_id != 0) {
        gtk_widget_remove_tick_callback(GTK_WIDGET(self), self->tick_id);
        self->tick_id = 0;
    }
    self->last_frame_time_us = 0;
}

static gboolean on_tilt_tick(GtkWidget *widget,
                             GdkFrameClock *frame_clock,
                             gpointer user_data) {
    (void)user_data;
    ConttestTiltIndicator *self = CONTTEST_TILT_INDICATOR(widget);

    gint64 now = gdk_frame_clock_get_frame_time(frame_clock);
    float dt = 1.0f / 60.0f;
    if (self->last_frame_time_us > 0) {
        dt = (float)(now - self->last_frame_time_us) * 1e-6f;
        dt = CLAMP(dt, 0.0f, 0.1f);
    }
    self->last_frame_time_us = now;

    float alpha = 1.0f - expf(-dt / TILT_TAU_SEC);
    self->sax += alpha * (self->target_x - self->sax);
    self->say += alpha * (self->target_y - self->say);
    self->saz += alpha * (self->target_z - self->saz);

    gtk_widget_queue_draw(widget);

    if (fabsf(self->target_x - self->sax) < TILT_EPS &&
        fabsf(self->target_y - self->say) < TILT_EPS &&
        fabsf(self->target_z - self->saz) < TILT_EPS) {
        self->sax = self->target_x;
        self->say = self->target_y;
        self->saz = self->target_z;
        self->tick_id = 0;
        self->last_frame_time_us = 0;
        return G_SOURCE_REMOVE;
    }

    return G_SOURCE_CONTINUE;
}

static void conttest_tilt_indicator_dispose(GObject *object) {
    ConttestTiltIndicator *self = CONTTEST_TILT_INDICATOR(object);
    stop_tilt_tick(self);
    g_clear_pointer(&self->label, gtk_widget_unparent);
    G_OBJECT_CLASS(conttest_tilt_indicator_parent_class)->dispose(object);
}

static void conttest_tilt_indicator_measure(GtkWidget *widget,
                                            GtkOrientation orientation,
                                            int for_size,
                                            int *minimum,
                                            int *natural,
                                            int *minimum_baseline,
                                            int *natural_baseline) {
    ConttestTiltIndicator *self = CONTTEST_TILT_INDICATOR(widget);
    conttest_captioned_widget_measure(self->label, orientation, for_size,
                                      TILT_MIN_SIZE, TILT_MIN_SIZE,
                                      TILT_NAT_SIZE, TILT_NAT_SIZE,
                                      minimum, natural,
                                      minimum_baseline, natural_baseline);
}

static void conttest_tilt_indicator_size_allocate(GtkWidget *widget,
                                                  int width,
                                                  int height,
                                                  int baseline) {
    (void)baseline;
    ConttestTiltIndicator *self = CONTTEST_TILT_INDICATOR(widget);
    conttest_captioned_widget_allocate(self->label, width, height,
                                       &self->canvas_w, &self->canvas_h);
}

static void conttest_tilt_indicator_snapshot(GtkWidget *widget, GtkSnapshot *snapshot) {
    ConttestTiltIndicator *self = CONTTEST_TILT_INDICATOR(widget);

    /* Derive roll and pitch from accelerometer:
     * roll  = tilt left/right   -> maps to X axis of the circle
     * pitch = tilt forward/back -> maps to Y axis of the circle */
    float mag = hypotf(hypotf(self->sax, self->say), self->saz);
    float norm_x = 0.0f;
    float norm_y = 0.0f;
    if (isfinite(mag) && mag > 0.1f) {
        norm_x = CLAMP(-self->sax / mag, -1.0f, 1.0f);
        norm_y = CLAMP(-self->say / mag, -1.0f, 1.0f);
    }

    conttest_snapshot_circular_indicator(widget, snapshot,
                                         (float)self->canvas_w,
                                         (float)self->canvas_h,
                                         norm_x, norm_y);
    if (self->label && gtk_widget_get_visible(self->label))
        gtk_widget_snapshot_child(widget, self->label, snapshot);
}

static void conttest_tilt_indicator_init(ConttestTiltIndicator *self) {
    self->target_z = -9.81f; /* rest position: gravity pointing down */
    self->saz = -9.81f;
    self->label = conttest_captioned_widget_create_label(GTK_WIDGET(self), _("Tilt"));
    conttest_widget_connect_style_redraw(GTK_WIDGET(self));
}

static void conttest_tilt_indicator_class_init(ConttestTiltIndicatorClass *klass) {
    GObjectClass *oclass = G_OBJECT_CLASS(klass);
    GtkWidgetClass *wclass = GTK_WIDGET_CLASS(klass);

    oclass->dispose = conttest_tilt_indicator_dispose;
    wclass->measure = conttest_tilt_indicator_measure;
    wclass->size_allocate = conttest_tilt_indicator_size_allocate;
    wclass->snapshot = conttest_tilt_indicator_snapshot;

    gtk_widget_class_set_accessible_role(wclass, GTK_ACCESSIBLE_ROLE_IMG);
}

GtkWidget *conttest_tilt_indicator_new(void) {
    return GTK_WIDGET(g_object_new(CONTTEST_TYPE_TILT_INDICATOR, NULL));
}

void conttest_tilt_indicator_set_accel(ConttestTiltIndicator *self, float ax, float ay, float az) {
    g_return_if_fail(CONTTEST_IS_TILT_INDICATOR(self));
    if (!isfinite(ax) || !isfinite(ay) || !isfinite(az))
        return;

    ax = CLAMP(ax, -1000.0f, 1000.0f);
    ay = CLAMP(ay, -1000.0f, 1000.0f);
    az = CLAMP(az, -1000.0f, 1000.0f);

    self->target_x = ax;
    self->target_y = ay;
    self->target_z = az;

    if (fabsf(ax - self->sax) < TILT_EPS &&
        fabsf(ay - self->say) < TILT_EPS &&
        fabsf(az - self->saz) < TILT_EPS) {
        return;
    }

    if (self->tick_id == 0) {
        self->last_frame_time_us = 0;
        self->tick_id = gtk_widget_add_tick_callback(GTK_WIDGET(self),
                                                     on_tilt_tick, NULL, NULL);
    }
}
