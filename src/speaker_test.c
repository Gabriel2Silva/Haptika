#define _GNU_SOURCE
#include "speaker_test.h"
#include "haptika-config.h"
#include <glib/gi18n.h>

#if HAVE_PIPEWIRE

#include <gio/gio.h>
#include <libudev.h>
#include <math.h>
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <string.h>

#define SAMPLE_RATE  48000
#define TONE_HZ      440.0
#define TONE_SECONDS 1.0

/* ── udev: get ID_ID of the ALSA card sharing a USB parent with evdev ─────── */

static char *find_alsa_id_id_for_evdev(struct udev *udev, const char *evdev_syspath) {
    struct udev_device *evdev = udev_device_new_from_syspath(udev, evdev_syspath);
    if (!evdev) return NULL;

    struct udev_device *usb_dev =
        udev_device_get_parent_with_subsystem_devtype(evdev, "usb", "usb_device");

    char *id_id = NULL;
    if (usb_dev) {
        const char *usb_syspath = udev_device_get_syspath(usb_dev);

        struct udev_enumerate *en = udev_enumerate_new(udev);
        udev_enumerate_add_match_subsystem(en, "sound");
        udev_enumerate_scan_devices(en);

        struct udev_list_entry *entry;
        udev_list_entry_foreach(entry, udev_enumerate_get_list_entry(en)) {
            const char *path = udev_list_entry_get_name(entry);
            struct udev_device *snd = udev_device_new_from_syspath(udev, path);
            if (!snd) continue;

            const char *sysname = udev_device_get_sysname(snd);
            if (!sysname || strncmp(sysname, "card", 4) != 0) {
                udev_device_unref(snd); continue;
            }

            struct udev_device *snd_usb =
                udev_device_get_parent_with_subsystem_devtype(snd, "usb", "usb_device");
            if (snd_usb && g_strcmp0(udev_device_get_syspath(snd_usb), usb_syspath) == 0) {
                const char *val = udev_device_get_property_value(snd, "ID_ID");
                if (val) id_id = g_strdup(val);
                udev_device_unref(snd);
                break;
            }
            udev_device_unref(snd);
        }
        udev_enumerate_unref(en);
    }

    udev_device_unref(evdev);
    return id_id;
}

/* ── PipeWire tone playback ──────────────────────────────────────────────── */

typedef struct {
    struct pw_main_loop *loop;
    struct pw_stream    *stream;
    double               phase;
    int                  total_frames;
    int                  frames_written;
} ToneCtx;

static void on_process(void *userdata) {
    ToneCtx *ctx = userdata;
    struct pw_buffer *b = pw_stream_dequeue_buffer(ctx->stream);
    if (!b) return;

    struct spa_buffer *buf = b->buffer;
    float *dst = buf->datas[0].data;
    if (!dst) { pw_stream_queue_buffer(ctx->stream, b); return; }

    uint32_t n = buf->datas[0].maxsize / sizeof(float);
    int rem = ctx->total_frames - ctx->frames_written;
    if (rem <= 0) { pw_main_loop_quit(ctx->loop); pw_stream_queue_buffer(ctx->stream, b); return; }
    if ((int)n > rem) n = (uint32_t)rem;

    double phase = ctx->phase;
    for (uint32_t i = 0; i < n; i++) {
        dst[i] = (float)sin(phase);
        phase += 2.0 * M_PI * TONE_HZ / SAMPLE_RATE;
        if (phase >= 2.0 * M_PI) phase -= 2.0 * M_PI;
    }
    ctx->phase = phase;
    ctx->frames_written += (int)n;

    buf->datas[0].chunk->offset = 0;
    buf->datas[0].chunk->stride = sizeof(float);
    buf->datas[0].chunk->size   = n * sizeof(float);
    pw_stream_queue_buffer(ctx->stream, b);

    if (ctx->frames_written >= ctx->total_frames)
        pw_main_loop_quit(ctx->loop);
}

static const struct pw_stream_events stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .process = on_process,
};

static char *play_tone(const char *node_name) {
    pw_init(NULL, NULL);

    ToneCtx ctx = { .total_frames = (int)(SAMPLE_RATE * TONE_SECONDS) };

    ctx.loop = pw_main_loop_new(NULL);
    if (!ctx.loop) { pw_deinit(); return g_strdup(_("Failed to create PipeWire loop")); }

    struct pw_properties *props = pw_properties_new(
        PW_KEY_MEDIA_TYPE,     "Audio",
        PW_KEY_MEDIA_CATEGORY, "Playback",
        PW_KEY_MEDIA_ROLE,     "Game",
        PW_KEY_APP_NAME,       "Haptika",
        PW_KEY_NODE_NAME,      "haptika-speaker-test",
        PW_KEY_TARGET_OBJECT,  node_name,
        NULL);

    ctx.stream = pw_stream_new_simple(
        pw_main_loop_get_loop(ctx.loop),
        "haptika-speaker-test",
        props,
        &stream_events,
        &ctx);

    if (!ctx.stream) {
        pw_main_loop_destroy(ctx.loop);
        pw_deinit();
        return g_strdup(_("Failed to create PipeWire stream"));
    }

    uint8_t buffer[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    const struct spa_pod *params[1];
    params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat,
        &SPA_AUDIO_INFO_RAW_INIT(
            .format   = SPA_AUDIO_FORMAT_F32,
            .channels = 1,
            .rate     = SAMPLE_RATE));

    pw_stream_connect(ctx.stream,
                      PW_DIRECTION_OUTPUT,
                      PW_ID_ANY,
                      PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS,
                      params, 1);

    pw_main_loop_run(ctx.loop);

    pw_stream_destroy(ctx.stream);
    pw_main_loop_destroy(ctx.loop);
    pw_deinit();
    return NULL;
}

/* ── GTask async wrapper ─────────────────────────────────────────────────── */

typedef struct {
    char *evdev_syspath;
} SpeakerTestTask;

static void speaker_test_task_free(gpointer p) {
    SpeakerTestTask *t = p;
    g_free(t->evdev_syspath);
    g_free(t);
}

typedef struct {
    SpeakerTestCallback callback;
    gpointer            user_data;
} SpeakerTestCaller;

typedef struct {
    SpeakerTestCallback callback;
    gpointer            user_data;
    char               *error;
} SpeakerTestResult;

static gboolean deliver_result(gpointer data) {
    SpeakerTestResult *r = data;
    r->callback(r->error, r->user_data);
    g_free(r->error);
    g_free(r);
    return G_SOURCE_REMOVE;
}

static void speaker_test_thread(GTask *task, gpointer source,
                                 gpointer task_data, GCancellable *cancellable) {
    (void)source; (void)cancellable;
    SpeakerTestTask *t = task_data;

    char *error = NULL;
    struct udev *udev = udev_new();
    char *id_id = udev ? find_alsa_id_id_for_evdev(udev, t->evdev_syspath) : NULL;
    if (udev) udev_unref(udev);
    if (!id_id) {
        error = g_strdup(_("No audio device found for this controller."));
    } else {
        char *node_name = g_strdup_printf("alsa_output.%s.HiFi__Speaker__sink", id_id);
        g_free(id_id);
        error = play_tone(node_name);
        g_free(node_name);
    }

    g_task_return_pointer(task, error, g_free);
}

static void on_task_done(GObject *source, GAsyncResult *result, gpointer user_data) {
    (void)source;
    SpeakerTestCaller *caller = user_data;
    char *error = g_task_propagate_pointer(G_TASK(result), NULL);

    SpeakerTestResult *r = g_new(SpeakerTestResult, 1);
    r->callback  = caller->callback;
    r->user_data = caller->user_data;
    r->error     = error;
    g_idle_add(deliver_result, r);

    g_free(caller);
}

void speaker_test_play(const char *evdev_syspath,
                       SpeakerTestCallback callback,
                       gpointer user_data) {
    SpeakerTestTask *t = g_new(SpeakerTestTask, 1);
    t->evdev_syspath = g_strdup(evdev_syspath);

    SpeakerTestCaller *caller = g_new(SpeakerTestCaller, 1);
    caller->callback  = callback;
    caller->user_data = user_data;

    GTask *task = g_task_new(NULL, NULL, on_task_done, caller);
    g_task_set_task_data(task, t, speaker_test_task_free);
    g_task_run_in_thread(task, speaker_test_thread);
    g_object_unref(task);
}

#else /* !HAVE_PIPEWIRE */

void speaker_test_play(const char *evdev_syspath,
                       SpeakerTestCallback callback,
                       gpointer user_data) {
    (void)evdev_syspath;
    callback(_("Speaker test requires PipeWire (not available in this build)."), user_data);
}

#endif /* HAVE_PIPEWIRE */
