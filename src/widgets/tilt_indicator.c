#include "tilt_indicator.h"
#include "drawing_utils.h"
#include <glib/gi18n.h>
#include <math.h>

#define TILT_MIN_SIZE 100
#define TILT_NAT_SIZE 100
/* Frame-rate-independent smoothing time constant (seconds).
 * TAU = 0.10 s reproduces alpha ~ 0.15 at 60 Hz across all refresh rates. */
#define TILT_TAU_SEC    0.10f
#define TILT_SETTLE_EPS 0.02f
/* Sub-pixel visual delta gate (~0.12 px on 100 px canvas): suppresses resting
 * MEMS accelerometer thermal noise (especially pure Z-axis gravity jitter)
 * without delaying real physical tilt. */
#define TILT_VISUAL_EPS 0.003f

struct _ConttestTiltIndicator {
    GtkWidget                      parent_instance;
    GtkWidget                     *label;
    ConttestCircularIndicatorCache geom_cache;
    int                            canvas_w;
    int                            canvas_h;
    float                          target_x;
    float                          target_y;
    float                          target_z;
    float                          sax;
    float                          say;
    float                          saz;
    float                          drawn_nx;
    float                          drawn_ny;
    guint                          tick_id;
    gint64                         last_frame_time_us;
};

G_DEFINE_TYPE(ConttestTiltIndicator, conttest_tilt_indicator, GTK_TYPE_WIDGET)

static inline void compute_tilt_norm(float ax, float ay, float az,
                                     float *out_nx, float *out_ny) {
    /* Inputs are bounded to [-1000, 1000], so x^2 + y^2 + z^2 <= 3e6 << FLT_MAX. */
    float mag = sqrtf(ax * ax + ay * ay + az * az);
    if (isfinite(mag) && mag > 0.1f) {
        *out_nx = CLAMP(-ax / mag, -1.0f, 1.0f);
        *out_ny = CLAMP(-ay / mag, -1.0f, 1.0f);
    } else {
        *out_nx = 0.0f;
        *out_ny = 0.0f;
    }
}

static inline gboolean tilt_norm_changed(float prev_nx, float prev_ny,
                                         float nx, float ny, float eps) {
    gboolean boundary =
        (nx == 0.0f && prev_nx != 0.0f) ||
        (ny == 0.0f && prev_ny != 0.0f) ||
        (fabsf(nx) >= 1.0f && fabsf(prev_nx) < 1.0f) ||
        (fabsf(ny) >= 1.0f && fabsf(prev_ny) < 1.0f);
    if (boundary)
        return TRUE;
    return fabsf(nx - prev_nx) >= eps || fabsf(ny - prev_ny) >= eps;
}

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

    float nx, ny, tnx, tny;
    compute_tilt_norm(self->sax, self->say, self->saz, &nx, &ny);
    compute_tilt_norm(self->target_x, self->target_y, self->target_z, &tnx, &tny);

    gboolean settled =
        fabsf(tnx - nx) < TILT_VISUAL_EPS &&
        fabsf(tny - ny) < TILT_VISUAL_EPS &&
        fabsf(self->target_x - self->sax) < TILT_SETTLE_EPS &&
        fabsf(self->target_y - self->say) < TILT_SETTLE_EPS &&
        fabsf(self->target_z - self->saz) < TILT_SETTLE_EPS;

    if (settled) {
        self->sax = self->target_x;
        self->say = self->target_y;
        self->saz = self->target_z;
        nx = tnx;
        ny = tny;
    }

    if (tilt_norm_changed(self->drawn_nx, self->drawn_ny, nx, ny, TILT_VISUAL_EPS)) {
        self->drawn_nx = nx;
        self->drawn_ny = ny;
        gtk_widget_queue_draw(widget);
    }

    if (settled) {
        self->tick_id = 0;
        self->last_frame_time_us = 0;
        return G_SOURCE_REMOVE;
    }

    return G_SOURCE_CONTINUE;
}

static void conttest_tilt_indicator_dispose(GObject *object) {
    ConttestTiltIndicator *self = CONTTEST_TILT_INDICATOR(object);
    stop_tilt_tick(self);
    conttest_circular_indicator_cache_clear(&self->geom_cache);
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

    conttest_snapshot_circular_indicator_cached(widget, snapshot,
                                                &self->geom_cache,
                                                (float)self->canvas_w,
                                                (float)self->canvas_h,
                                                self->drawn_nx,
                                                self->drawn_ny);
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

    float tnx, tny;
    compute_tilt_norm(ax, ay, az, &tnx, &tny);
    if (self->tick_id == 0 &&
        !tilt_norm_changed(self->drawn_nx, self->drawn_ny, tnx, tny, TILT_VISUAL_EPS)) {
        return;
    }

    if (self->tick_id == 0) {
        self->last_frame_time_us = 0;
        self->tick_id = gtk_widget_add_tick_callback(GTK_WIDGET(self),
                                                     on_tilt_tick, NULL, NULL);
    }
}
