#define _GNU_SOURCE
#include "speaker_test.h"
#include "speaker_routing.h"
#include "dsp_oscillator.h"
#include "haptika-config.h"
#include <glib/gi18n.h>

#if HAVE_PIPEWIRE

#include <libudev.h>
#include <limits.h>
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/audio/raw-types.h>
#include <spa/param/props.h>
#include <spa/support/system.h>
#include <string.h>
#include <time.h>

#define SAMPLE_RATE          48000
#define TONE_HZ              880.0
#define TONE_GAIN            0.5f
#define TONE_SECONDS         1.0
#define CONNECT_TIMEOUT_SEC  3
#define PLAYBACK_TIMEOUT_SEC 5

/* Plausible range for a controller audio endpoint. PipeWire's negotiated rate
 * is used for the tone math, so reject nonsensical values instead of letting
 * them reach an out-of-range floating-point-to-integer conversion. */
#define TONE_RATE_MIN        8000u
#define TONE_RATE_MAX        192000u

static const double CONTTEST_TWO_PI = 6.283185307179586476925286766559;

/* --- udev: resolve ALSA card index sharing a USB parent with evdev --- */

static int find_alsa_card_for_evdev(struct udev *udev, const char *evdev_syspath) {
    if (!udev || !evdev_syspath)
        return -1;

    struct udev_device *evdev = udev_device_new_from_syspath(udev, evdev_syspath);
    if (!evdev)
        return -1;

    struct udev_device *usb_dev =
        udev_device_get_parent_with_subsystem_devtype(evdev, "usb", "usb_device");

    int card_num = -1;
    if (usb_dev) {
        const char *usb_syspath = udev_device_get_syspath(usb_dev);
        struct udev_enumerate *en = udev_enumerate_new(udev);
        if (en) {
            udev_enumerate_add_match_subsystem(en, "sound");
            udev_enumerate_scan_devices(en);

            struct udev_list_entry *entry;
            udev_list_entry_foreach(entry, udev_enumerate_get_list_entry(en)) {
                const char *path = udev_list_entry_get_name(entry);
                struct udev_device *snd = udev_device_new_from_syspath(udev, path);
                if (!snd)
                    continue;

                const char *sysname = udev_device_get_sysname(snd);
                int parsed = -1;
                if (!sysname || strncmp(sysname, "card", 4) != 0 ||
                    !conttest_speaker_parse_int_strict(sysname + 4, &parsed)) {
                    udev_device_unref(snd);
                    continue;
                }

                struct udev_device *snd_usb =
                    udev_device_get_parent_with_subsystem_devtype(snd, "usb", "usb_device");
                if (snd_usb && g_strcmp0(udev_device_get_syspath(snd_usb), usb_syspath) == 0) {
                    card_num = parsed;
                    udev_device_unref(snd);
                    break;
                }
                udev_device_unref(snd);
            }
            udev_enumerate_unref(en);
        }
    }

    udev_device_unref(evdev);
    return card_num;
}

/* --- PipeWire live registry discovery for controller Audio/Sink --- */

static void ensure_pipewire_init(void) {
    static gsize initialized = 0;
    if (g_once_init_enter(&initialized)) {
        pw_init(NULL, NULL);
        g_once_init_leave(&initialized, 1);
    }
}

typedef struct {
    uint32_t node_id;
    char    *target_object;
    int      score;
    uint32_t channels;
    uint32_t position[SPA_AUDIO_MAX_CHANNELS];
    gboolean has_position;
} SinkTarget;

static void sink_target_clear(SinkTarget *t) {
    g_free(t->target_object);
    memset(t, 0, sizeof(*t));
    t->node_id = PW_ID_ANY;
}

typedef enum {
    DISC_PHASE_ENUM = 0,
    DISC_PHASE_INFO,
    DISC_PHASE_PARAM,
} DiscPhase;

typedef struct DiscoveryCtx DiscoveryCtx;

typedef struct {
    DiscoveryCtx    *disc;
    uint32_t         node_id;
    char            *fallback_serial;
    char            *fallback_name;
    struct pw_proxy *proxy;
    struct spa_hook  node_listener;
    struct spa_hook  proxy_listener;
    gboolean         removed;
} SinkProbe;

struct DiscoveryCtx {
    struct pw_main_loop *loop;
    struct pw_registry  *registry;
    struct pw_core      *core;
    GPtrArray           *probes;
    int                  target_card;
    DiscPhase            phase;
    int                  sync_seq;
    gboolean             found;
    gboolean             timed_out;
    char                *error;
    SinkTarget           best;
};

static void sink_probe_free(gpointer data) {
    SinkProbe *p = data;
    if (!p)
        return;
    if (p->proxy) {
        spa_hook_remove(&p->node_listener);
        spa_hook_remove(&p->proxy_listener);
        pw_proxy_destroy(p->proxy);
        p->proxy = NULL;
    }
    g_free(p->fallback_serial);
    g_free(p->fallback_name);
    g_free(p);
}

static void evaluate_sink_props(DiscoveryCtx *disc,
                                uint32_t node_id,
                                const struct spa_dict *props,
                                const char *fallback_serial,
                                const char *fallback_name) {
    if (!props)
        return;

    const char *media_class = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
    if (media_class && strcmp(media_class, "Audio/Sink") != 0)
        return;

    const char *card_str = spa_dict_lookup(props, "api.alsa.pcm.card");
    if (!card_str)
        card_str = spa_dict_lookup(props, "alsa.card");
    if (!card_str)
        card_str = spa_dict_lookup(props, "api.alsa.card");

    int node_card = -1;
    if (!conttest_speaker_parse_int_strict(card_str, &node_card) || node_card != disc->target_card)
        return;

    const char *serial = spa_dict_lookup(props, PW_KEY_OBJECT_SERIAL);
    const char *name   = spa_dict_lookup(props, PW_KEY_NODE_NAME);
    if (!serial || !*serial)
        serial = fallback_serial;
    if (!name || !*name)
        name = fallback_name;

    const char *target_obj = (serial && *serial) ? serial : name;
    if (!target_obj || !*target_obj)
        return;

    int score = conttest_speaker_score_sink_props(props);
    if (disc->found && disc->best.node_id != node_id) {
        if (score < disc->best.score ||
            (score == disc->best.score && node_id > disc->best.node_id)) {
            return;
        }
    }

    sink_target_clear(&disc->best);
    disc->found = TRUE;
    disc->best.node_id = node_id;
    disc->best.target_object = g_strdup(target_obj);
    disc->best.score = score;

    int ch = 0;
    if (conttest_speaker_parse_int_strict(spa_dict_lookup(props, SPA_KEY_AUDIO_CHANNELS), &ch) && ch > 0)
        disc->best.channels = MIN((uint32_t)ch, (uint32_t)SPA_AUDIO_MAX_CHANNELS);

    const char *pos_str = spa_dict_lookup(props, SPA_KEY_AUDIO_POSITION);
    if (pos_str) {
        uint32_t pos_count = 0;
        conttest_speaker_parse_audio_position_prop(pos_str, disc->best.position, &pos_count);
        if (pos_count > 0) {
            if (disc->best.channels == 0)
                disc->best.channels = pos_count;
            if (pos_count == disc->best.channels)
                disc->best.has_position = TRUE;
        }
    }
}

static void on_node_info(void *userdata, const struct pw_node_info *info) {
    SinkProbe *probe = userdata;
    if (!probe || probe->removed || !info || !info->props)
        return;
    evaluate_sink_props(probe->disc, probe->node_id, info->props,
                        probe->fallback_serial, probe->fallback_name);
}

static void on_node_param(void *userdata, int seq,
                          uint32_t id, uint32_t index, uint32_t next,
                          const struct spa_pod *param) {
    (void)seq; (void)id; (void)index; (void)next;
    SinkProbe *probe = userdata;
    DiscoveryCtx *disc = probe->disc;
    if (!param || !disc->found || probe->node_id != disc->best.node_id || disc->best.has_position)
        return;

    struct spa_audio_info_raw raw = { 0 };
    if (spa_format_audio_raw_parse(param, &raw) >= 0 && raw.channels > 0) {
        disc->best.channels = MIN(raw.channels, (uint32_t)SPA_AUDIO_MAX_CHANNELS);
        if (!SPA_FLAG_IS_SET(raw.flags, SPA_AUDIO_FLAG_UNPOSITIONED)) {
            memcpy(disc->best.position, raw.position,
                   disc->best.channels * sizeof(uint32_t));
            disc->best.has_position = TRUE;
        }
    }
}

static const struct pw_node_events disc_node_events = {
    PW_VERSION_NODE_EVENTS,
    .info  = on_node_info,
    .param = on_node_param,
};

static void on_probe_proxy_removed(void *userdata) {
    SinkProbe *probe = userdata;
    probe->removed = TRUE;
}

static const struct pw_proxy_events disc_probe_proxy_events = {
    PW_VERSION_PROXY_EVENTS,
    .removed = on_probe_proxy_removed,
};

static void on_registry_global(void *userdata, uint32_t id, uint32_t permissions,
                               const char *type, uint32_t version,
                               const struct spa_dict *props) {
    (void)permissions; (void)version;
    DiscoveryCtx *disc = userdata;
    if (!props || g_strcmp0(type, PW_TYPE_INTERFACE_Node) != 0)
        return;

    const char *media_class = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
    if (!media_class || strcmp(media_class, "Audio/Sink") != 0)
        return;

    struct pw_proxy *proxy = pw_registry_bind(disc->registry, id,
                                              PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, 0);
    if (!proxy)
        return;

    SinkProbe *probe = g_new0(SinkProbe, 1);
    probe->disc            = disc;
    probe->node_id         = id;
    probe->fallback_serial = g_strdup(spa_dict_lookup(props, PW_KEY_OBJECT_SERIAL));
    probe->fallback_name   = g_strdup(spa_dict_lookup(props, PW_KEY_NODE_NAME));
    probe->proxy           = proxy;

    pw_proxy_add_listener(proxy, &probe->proxy_listener, &disc_probe_proxy_events, probe);
    pw_node_add_listener((struct pw_node *)proxy, &probe->node_listener,
                         &disc_node_events, probe);
    g_ptr_array_add(disc->probes, probe);
}

static const struct pw_registry_events disc_registry_events = {
    PW_VERSION_REGISTRY_EVENTS,
    .global = on_registry_global,
};

static SinkProbe *find_probe_by_id(DiscoveryCtx *disc, uint32_t node_id) {
    for (guint i = 0; i < disc->probes->len; i++) {
        SinkProbe *p = g_ptr_array_index(disc->probes, i);
        if (p->node_id == node_id)
            return p;
    }
    return NULL;
}

static const struct spa_pod *build_stream_volume_props_pod(struct spa_pod_builder *b,
                                                           uint32_t channels) {
    uint32_t n_ch = CLAMP(channels, 1u, (uint32_t)SPA_AUDIO_MAX_CHANNELS);
    float vols[SPA_AUDIO_MAX_CHANNELS];
    for (uint32_t i = 0; i < n_ch; i++)
        vols[i] = 1.0f;

    struct spa_pod_frame f;
    spa_pod_builder_push_object(b, &f, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
    spa_pod_builder_add(
        b,
        SPA_PROP_mute,           SPA_POD_Bool(false),
        SPA_PROP_channelVolumes, SPA_POD_Array(sizeof(float), SPA_TYPE_Float, n_ch, vols),
        0);
    return spa_pod_builder_pop(b, &f);
}

static void on_core_done(void *userdata, uint32_t id, int seq) {
    DiscoveryCtx *disc = userdata;
    if (id != PW_ID_CORE || seq != disc->sync_seq)
        return;

    if (disc->phase == DISC_PHASE_ENUM) {
        if (disc->probes->len == 0) {
            pw_main_loop_quit(disc->loop);
            return;
        }
        disc->phase = DISC_PHASE_INFO;
        disc->sync_seq = pw_core_sync(disc->core, PW_ID_CORE, 0);
        if (disc->sync_seq < 0)
            pw_main_loop_quit(disc->loop);
        return;
    }

    if (disc->phase == DISC_PHASE_INFO) {
        if (disc->found && (!disc->best.has_position || disc->best.channels == 0)) {
            SinkProbe *best_probe = find_probe_by_id(disc, disc->best.node_id);
            if (best_probe && !best_probe->removed && best_probe->proxy) {
                pw_node_enum_params((struct pw_node *)best_probe->proxy, 0,
                                    SPA_PARAM_Format, 0, 2, NULL);
                pw_node_enum_params((struct pw_node *)best_probe->proxy, 0,
                                    SPA_PARAM_EnumFormat, 0, 4, NULL);
                disc->phase = DISC_PHASE_PARAM;
                disc->sync_seq = pw_core_sync(disc->core, PW_ID_CORE, 0);
                if (disc->sync_seq >= 0)
                    return;
            }
        }
        pw_main_loop_quit(disc->loop);
        return;
    }

    pw_main_loop_quit(disc->loop);
}

static void on_core_error(void *userdata, uint32_t id, int seq, int res, const char *message) {
    (void)seq;
    DiscoveryCtx *disc = userdata;
    if (id != PW_ID_CORE && res == -ENOENT)
        return;
    if (!disc->error) {
        disc->error = message
            ? g_strdup_printf(_("PipeWire error: %s"), message)
            : g_strdup_printf(_("PipeWire error: %s"), g_strerror(-res));
    }
    pw_main_loop_quit(disc->loop);
}

static const struct pw_core_events disc_core_events = {
    PW_VERSION_CORE_EVENTS,
    .done  = on_core_done,
    .error = on_core_error,
};

static void on_loop_timeout_quit(void *userdata, uint64_t expirations) {
    (void)expirations;
    DiscoveryCtx *disc = userdata;
    disc->timed_out = TRUE;
    pw_main_loop_quit(disc->loop);
}

static void on_loop_cancel_quit(void *userdata, int fd, uint32_t mask) {
    (void)fd; (void)mask;
    struct pw_main_loop *loop = userdata;
    pw_main_loop_quit(loop);
}

static gboolean discover_controller_sink(int card_num,
                                     GCancellable *cancellable,
                                     SinkTarget *out_target,
                                     char **out_error) {
    sink_target_clear(out_target);
    *out_error = NULL;

    struct pw_main_loop *loop = pw_main_loop_new(NULL);
    if (!loop) {
        *out_error = g_strdup(_("Failed to create PipeWire loop"));
        return FALSE;
    }
    struct pw_loop *pw_loop = pw_main_loop_get_loop(loop);

    struct pw_context *pw_ctx = pw_context_new(pw_loop, NULL, 0);
    if (!pw_ctx) {
        pw_main_loop_destroy(loop);
        *out_error = g_strdup(_("Failed to create PipeWire context."));
        return FALSE;
    }

    struct pw_core *core = pw_context_connect(pw_ctx, NULL, 0);
    if (!core) {
        pw_context_destroy(pw_ctx);
        pw_main_loop_destroy(loop);
        *out_error = g_strdup(_("Failed to connect to PipeWire."));
        return FALSE;
    }

    DiscoveryCtx disc = {
        .loop        = loop,
        .core        = core,
        .probes      = g_ptr_array_new_with_free_func(sink_probe_free),
        .target_card = card_num,
        .phase       = DISC_PHASE_ENUM,
        .sync_seq    = -1,
    };
    disc.best.node_id = PW_ID_ANY;

    struct spa_hook core_listener = { 0 };
    pw_core_add_listener(core, &core_listener, &disc_core_events, &disc);

    disc.registry = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);
    struct spa_hook reg_listener = { 0 };
    if (disc.registry)
        pw_registry_add_listener(disc.registry, &reg_listener, &disc_registry_events, &disc);

    struct spa_source *timeout_src = pw_loop_add_timer(pw_loop, on_loop_timeout_quit, &disc);
    if (timeout_src) {
        struct timespec t = { .tv_sec = CONNECT_TIMEOUT_SEC, .tv_nsec = 0 };
        struct timespec z = { 0, 0 };
        pw_loop_update_timer(pw_loop, timeout_src, &t, &z, false);
    }

    int cancel_fd = cancellable ? g_cancellable_get_fd(cancellable) : -1;
    struct spa_source *cancel_src = (cancel_fd >= 0)
        ? pw_loop_add_io(pw_loop, cancel_fd, SPA_IO_IN, false, on_loop_cancel_quit, loop)
        : NULL;

    if (!disc.registry) {
        *out_error = g_strdup(_("Failed to inspect PipeWire audio sinks."));
    } else if (!cancellable || !g_cancellable_is_cancelled(cancellable)) {
        disc.sync_seq = pw_core_sync(core, PW_ID_CORE, 0);
        if (disc.sync_seq >= 0)
            pw_main_loop_run(loop);
    }

    if (timeout_src) pw_loop_destroy_source(pw_loop, timeout_src);
    if (cancel_src)  pw_loop_destroy_source(pw_loop, cancel_src);
    if (cancel_fd >= 0) g_cancellable_release_fd(cancellable);

    g_ptr_array_unref(disc.probes);
    if (disc.registry) {
        spa_hook_remove(&reg_listener);
        pw_proxy_destroy((struct pw_proxy *)disc.registry);
    }
    spa_hook_remove(&core_listener);
    pw_core_disconnect(core);
    pw_context_destroy(pw_ctx);
    pw_main_loop_destroy(loop);

    if (cancellable && g_cancellable_is_cancelled(cancellable)) {
        sink_target_clear(&disc.best);
        g_free(disc.error);
        return FALSE;
    }

    if (!*out_error) {
        if (disc.error) {
            *out_error = disc.error;
            disc.error = NULL;
        } else if (disc.timed_out) {
            *out_error = g_strdup(_("Timed out connecting to controller speaker."));
        } else if (!disc.found) {
            *out_error = g_strdup(_("No active audio sink found for this controller."));
        }
    }
    g_free(disc.error);

    if (*out_error) {
        sink_target_clear(&disc.best);
        return FALSE;
    }

    *out_target = disc.best;
    return TRUE;
}

/* --- PipeWire tone playback --- */

typedef struct {
    struct pw_main_loop *loop;
    struct pw_loop      *pw_loop;
    struct pw_stream    *stream;
    struct spa_source   *timeout_src;
    struct spa_source   *cancel_src;
    GCancellable        *cancellable;
    char                *error;
    double               phase;
    double               omega;
    uint32_t             rate;
    uint32_t             channels;
    gboolean             active_ch[SPA_AUDIO_MAX_CHANNELS];
    int                  total_frames;
    int                  frames_written;
    gboolean             started_streaming;
    gboolean             draining;
    gboolean             drained;
    gboolean             tearing_down;
} ToneCtx;

static void on_state_changed(void *userdata, enum pw_stream_state old,
                             enum pw_stream_state state, const char *error) {
    ToneCtx *ctx = userdata;
    if (ctx->tearing_down)
        return;

    if (state == PW_STREAM_STATE_ERROR) {
        if (!ctx->error) {
            ctx->error = error
                ? g_strdup_printf(_("PipeWire stream error: %s"), error)
                : g_strdup(_("PipeWire stream error."));
        }
        pw_main_loop_quit(ctx->loop);
    } else if (state == PW_STREAM_STATE_UNCONNECTED && old != PW_STREAM_STATE_UNCONNECTED) {
        if (!ctx->error && !ctx->drained) {
            ctx->error = g_strdup(_("Controller speaker became unavailable."));
        }
        pw_main_loop_quit(ctx->loop);
    }
}

static void on_param_changed(void *userdata, uint32_t id, const struct spa_pod *param) {
    ToneCtx *ctx = userdata;
    if (!param || id != SPA_PARAM_Format)
        return;

    struct spa_audio_info_raw info = { 0 };
    if (spa_format_audio_raw_parse(param, &info) < 0)
        return;

    if (info.format != SPA_AUDIO_FORMAT_F32) {
        if (!ctx->error)
            ctx->error = g_strdup(_("Unsupported PipeWire audio sample format."));
        pw_main_loop_quit(ctx->loop);
        return;
    }

    if (info.channels == 0 || info.channels > SPA_AUDIO_MAX_CHANNELS) {
        if (!ctx->error)
            ctx->error = g_strdup_printf(
                _("Unsupported PipeWire channel count: %u"), info.channels);
        pw_main_loop_quit(ctx->loop);
        return;
    }

    if (!ctx->started_streaming) {
        ctx->channels = info.channels;
        conttest_speaker_build_channel_mask(&info, ctx->active_ch);
    } else if (info.channels != ctx->channels ||
               (info.rate > 0 && info.rate != ctx->rate)) {
        if (!ctx->error)
            ctx->error = g_strdup(
                _("PipeWire audio format changed unexpectedly during playback."));
        pw_main_loop_quit(ctx->loop);
        return;
    }

    if (info.rate > 0 && !ctx->started_streaming) {
        if (info.rate < TONE_RATE_MIN || info.rate > TONE_RATE_MAX) {
            if (!ctx->error)
                ctx->error = g_strdup_printf(
                    _("Unsupported PipeWire sample rate: %u Hz"), info.rate);
            pw_main_loop_quit(ctx->loop);
            return;
        }

        ctx->rate = info.rate;
        ctx->omega = CONTTEST_TWO_PI * TONE_HZ / (double)ctx->rate;
        /* Range-validated above, so total_frames is always representable. */
        ctx->total_frames = (int)((double)ctx->rate * TONE_SECONDS);

        uint8_t prop_buf[512];
        struct spa_pod_builder pb = SPA_POD_BUILDER_INIT(prop_buf, sizeof(prop_buf));
        const struct spa_pod *prop_param =
            build_stream_volume_props_pod(&pb, ctx->channels > 0 ? ctx->channels : 1u);
        if (prop_param)
            pw_stream_set_param(ctx->stream, SPA_PARAM_Props, prop_param);
    }
}

static void on_startup_timeout(void *userdata, uint64_t expirations) {
    (void)expirations;
    ToneCtx *ctx = userdata;
    if (!ctx->error) {
        ctx->error = g_strdup(ctx->started_streaming
            ? _("Timed out playing controller speaker tone.")
            : _("Timed out connecting to controller speaker."));
    }
    pw_main_loop_quit(ctx->loop);
}

static void on_drained(void *userdata) {
    ToneCtx *ctx = userdata;
    ctx->drained = TRUE;
    pw_main_loop_quit(ctx->loop);
}

static void queue_empty_buffer(ToneCtx *ctx, struct pw_buffer *b, uint32_t stride) {
    struct spa_buffer *buf = b->buffer;
    if (buf->n_datas > 0 && buf->datas[0].chunk) {
        buf->datas[0].chunk->offset = 0;
        buf->datas[0].chunk->stride = (int32_t)stride;
        buf->datas[0].chunk->size   = 0;
    }
    b->size = 0;
    pw_stream_queue_buffer(ctx->stream, b);
}

static void on_process(void *userdata) {
    ToneCtx *ctx = userdata;
    if (ctx->tearing_down)
        return;
    if (ctx->cancellable && g_cancellable_is_cancelled(ctx->cancellable)) {
        pw_main_loop_quit(ctx->loop);
        return;
    }

    struct pw_buffer *b = pw_stream_dequeue_buffer(ctx->stream);
    if (!b)
        return;

    struct spa_buffer *buf = b->buffer;
    uint32_t channels = ctx->channels > 0 ? ctx->channels : 1;
    uint32_t stride = channels * (uint32_t)sizeof(float);

    if (buf->n_datas == 0 || !buf->datas[0].data || !buf->datas[0].chunk) {
        queue_empty_buffer(ctx, b, stride);
        return;
    }

    int rem = ctx->total_frames - ctx->frames_written;
    if (ctx->draining || rem <= 0) {
        queue_empty_buffer(ctx, b, stride);
        return;
    }

    uint32_t max_frames = buf->datas[0].maxsize / stride;
    uint32_t n = max_frames;
    if (b->requested > 0 && b->requested < (uint64_t)n)
        n = (uint32_t)b->requested;
    if (n > (uint32_t)rem)
        n = (uint32_t)rem;

    if (n == 0) {
        queue_empty_buffer(ctx, b, stride);
        return;
    }

    if (!ctx->started_streaming) {
        ctx->started_streaming = TRUE;
        if (ctx->timeout_src) {
            struct timespec play_timeout = { .tv_sec = PLAYBACK_TIMEOUT_SEC, .tv_nsec = 0 };
            struct timespec zero = { 0, 0 };
            pw_loop_update_timer(ctx->pw_loop, ctx->timeout_src, &play_timeout, &zero, false);
        }
    }

    float *dst = buf->datas[0].data;
    if (channels == 1) {
        conttest_dsp_generate_sine(dst, n, &ctx->phase, ctx->omega, TONE_GAIN);
    } else {
        float mono[1024];
        uint32_t done = 0;
        while (done < n) {
            uint32_t chunk = MIN(n - done, (uint32_t)G_N_ELEMENTS(mono));
            conttest_dsp_generate_sine(mono, chunk, &ctx->phase, ctx->omega, TONE_GAIN);
            for (uint32_t i = 0; i < chunk; i++) {
                float s = mono[i];
                for (uint32_t ch = 0; ch < channels; ch++)
                    *dst++ = ctx->active_ch[ch] ? s : 0.0f;
            }
            done += chunk;
        }
    }

    ctx->frames_written += (int)n;

    buf->datas[0].chunk->offset = 0;
    buf->datas[0].chunk->stride = (int32_t)stride;
    buf->datas[0].chunk->size   = n * stride;
    b->size                     = n;
    pw_stream_queue_buffer(ctx->stream, b);

    if (ctx->frames_written >= ctx->total_frames) {
        ctx->draining = TRUE;
        pw_stream_flush(ctx->stream, true);
    }
}

static const struct pw_stream_events stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .state_changed = on_state_changed,
    .param_changed = on_param_changed,
    .process       = on_process,
    .drained       = on_drained,
};

static char *play_tone(int card_num, GCancellable *cancellable) {
    if (cancellable && g_cancellable_is_cancelled(cancellable))
        return NULL;

    ensure_pipewire_init();

    SinkTarget target = { .node_id = PW_ID_ANY };
    char *err = NULL;
    if (!discover_controller_sink(card_num, cancellable, &target, &err))
        return err;

    struct pw_main_loop *loop = pw_main_loop_new(NULL);
    if (!loop) {
        sink_target_clear(&target);
        return g_strdup(_("Failed to create PipeWire loop"));
    }
    struct pw_loop *pw_loop = pw_main_loop_get_loop(loop);

    ToneCtx ctx = {
        .loop         = loop,
        .pw_loop      = pw_loop,
        .cancellable  = cancellable,
        .omega        = CONTTEST_TWO_PI * TONE_HZ / SAMPLE_RATE,
        .rate         = SAMPLE_RATE,
        .channels     = (target.channels > 0) ? target.channels : 1u,
        .total_frames = (int)(SAMPLE_RATE * TONE_SECONDS),
    };
    ctx.active_ch[0] = TRUE;

    struct spa_audio_info_raw fmt = SPA_AUDIO_INFO_RAW_INIT(
        .format = SPA_AUDIO_FORMAT_F32,
        .rate   = SAMPLE_RATE);
    if (target.channels > 0) {
        fmt.channels = target.channels;
        if (target.has_position) {
            memcpy(fmt.position, target.position, target.channels * sizeof(uint32_t));
        } else if (target.channels == 1) {
            fmt.position[0] = SPA_AUDIO_CHANNEL_MONO;
        } else {
            fmt.flags |= SPA_AUDIO_FLAG_UNPOSITIONED;
        }
        conttest_speaker_build_channel_mask(&fmt, ctx.active_ch);
    }

    struct pw_properties *props = pw_properties_new(
        PW_KEY_MEDIA_TYPE,          "Audio",
        PW_KEY_MEDIA_CATEGORY,      "Playback",
        PW_KEY_MEDIA_ROLE,          "Game",
        PW_KEY_APP_NAME,            "Haptika",
        PW_KEY_NODE_NAME,           "haptika-speaker-test",
        PW_KEY_TARGET_OBJECT,       target.target_object,
        PW_KEY_NODE_DONT_RECONNECT, "true",
        "node.dont-fallback",       "true",
        "node.dont-move",           "true",
        NULL);
    sink_target_clear(&target);

    ctx.stream = pw_stream_new_simple(
        ctx.pw_loop,
        "haptika-speaker-test",
        props,
        &stream_events,
        &ctx);

    if (!ctx.stream) {
        pw_main_loop_destroy(ctx.loop);
        return g_strdup(_("Failed to create PipeWire stream"));
    }

    ctx.timeout_src = pw_loop_add_timer(ctx.pw_loop, on_startup_timeout, &ctx);
    if (ctx.timeout_src) {
        struct timespec connect_timeout = { .tv_sec = CONNECT_TIMEOUT_SEC, .tv_nsec = 0 };
        struct timespec zero = { 0, 0 };
        pw_loop_update_timer(ctx.pw_loop, ctx.timeout_src, &connect_timeout, &zero, false);
    }

    int cancel_fd = cancellable ? g_cancellable_get_fd(cancellable) : -1;
    if (cancel_fd >= 0) {
        ctx.cancel_src = pw_loop_add_io(ctx.pw_loop, cancel_fd, SPA_IO_IN, false,
                                        on_loop_cancel_quit, ctx.loop);
    }

    uint8_t buffer[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    const struct spa_pod *params[1];
    params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &fmt);

    int res = pw_stream_connect(ctx.stream,
                                PW_DIRECTION_OUTPUT,
                                PW_ID_ANY,
                                PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS,
                                params, 1);

    if (res < 0) {
        ctx.error = g_strdup_printf(_("Failed to connect PipeWire stream: %s"),
                                    g_strerror(-res));
    } else if (!ctx.error && (!cancellable || !g_cancellable_is_cancelled(cancellable))) {
        pw_main_loop_run(ctx.loop);
        if (!ctx.drained && !ctx.error &&
            (!cancellable || !g_cancellable_is_cancelled(cancellable))) {
            ctx.error = g_strdup(_("Audio playback ended prematurely."));
        }
    }

    ctx.tearing_down = TRUE;
    pw_stream_destroy(ctx.stream);
    if (ctx.timeout_src) pw_loop_destroy_source(ctx.pw_loop, ctx.timeout_src);
    if (ctx.cancel_src)  pw_loop_destroy_source(ctx.pw_loop, ctx.cancel_src);
    if (cancel_fd >= 0)  g_cancellable_release_fd(cancellable);
    pw_main_loop_destroy(ctx.loop);
    return ctx.error;
}

/* --- GTask async wrapper --- */

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
    GDestroyNotify      destroy_notify;
} SpeakerTestCaller;

static void speaker_test_caller_free(SpeakerTestCaller *caller) {
    if (!caller)
        return;
    if (caller->destroy_notify)
        caller->destroy_notify(caller->user_data);
    g_free(caller);
}

static void speaker_test_thread(GTask *task, gpointer source,
                                gpointer task_data, GCancellable *cancellable) {
    (void)source;
    SpeakerTestTask *t = task_data;

    if (g_cancellable_is_cancelled(cancellable)) {
        g_task_return_pointer(task, NULL, NULL);
        return;
    }

    struct udev *udev = udev_new();
    int card_num = udev ? find_alsa_card_for_evdev(udev, t->evdev_syspath) : -1;
    if (udev)
        udev_unref(udev);

    char *error = (card_num < 0)
        ? g_strdup(_("No audio device found for this controller."))
        : play_tone(card_num, cancellable);

    g_task_return_pointer(task, error, g_free);
}

static void on_task_done(GObject *source, GAsyncResult *result, gpointer user_data) {
    (void)source;
    SpeakerTestCaller *caller = user_data;
    GTask *task = G_TASK(result);
    GCancellable *cancellable = g_task_get_cancellable(task);
    char *error = g_task_propagate_pointer(task, NULL);

    if (!cancellable || !g_cancellable_is_cancelled(cancellable))
        caller->callback(error, caller->user_data);

    g_free(error);
    speaker_test_caller_free(caller);
}

void speaker_test_play(const char *evdev_syspath,
                       GCancellable *cancellable,
                       SpeakerTestCallback callback,
                       gpointer user_data,
                       GDestroyNotify destroy_notify) {
    SpeakerTestTask *t = g_new(SpeakerTestTask, 1);
    t->evdev_syspath = g_strdup(evdev_syspath);

    SpeakerTestCaller *caller = g_new(SpeakerTestCaller, 1);
    caller->callback       = callback;
    caller->user_data      = user_data;
    caller->destroy_notify = destroy_notify;

    GTask *task = g_task_new(NULL, cancellable, on_task_done, caller);
    g_task_set_task_data(task, t, speaker_test_task_free);
    g_task_run_in_thread(task, speaker_test_thread);
    g_object_unref(task);
}

#else /* !HAVE_PIPEWIRE */

typedef struct {
    GCancellable        *cancellable;
    SpeakerTestCallback  callback;
    gpointer             user_data;
    GDestroyNotify       destroy_notify;
} NoPipeWireIdleData;

static void no_pipewire_idle_data_free(gpointer data) {
    NoPipeWireIdleData *d = data;
    if (!d)
        return;
    if (d->destroy_notify)
        d->destroy_notify(d->user_data);
    g_clear_object(&d->cancellable);
    g_free(d);
}

static gboolean deliver_no_pipewire_error(gpointer data) {
    NoPipeWireIdleData *d = data;
    if (!d->cancellable || !g_cancellable_is_cancelled(d->cancellable)) {
        d->callback(_("Speaker test requires PipeWire (not available in this build)."),
                    d->user_data);
    }
    return G_SOURCE_REMOVE;
}

void speaker_test_play(const char *evdev_syspath,
                       GCancellable *cancellable,
                       SpeakerTestCallback callback,
                       gpointer user_data,
                       GDestroyNotify destroy_notify) {
    (void)evdev_syspath;
    NoPipeWireIdleData *d = g_new0(NoPipeWireIdleData, 1);
    d->cancellable    = cancellable ? g_object_ref(cancellable) : NULL;
    d->callback       = callback;
    d->user_data      = user_data;
    d->destroy_notify = destroy_notify;
    g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, deliver_no_pipewire_error, d,
                    no_pipewire_idle_data_free);
}

#endif /* HAVE_PIPEWIRE */
