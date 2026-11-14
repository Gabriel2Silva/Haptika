#include "inputplumber_logic.h"
#include <inttypes.h>
#include <string.h>

/* ── Source / Info data helpers ──────────────────────────────────────────── */

static ConttestInputPlumberSource *source_copy(const ConttestInputPlumberSource *src) {
    if (!src) return NULL;
    ConttestInputPlumberSource *c = g_new0(ConttestInputPlumberSource, 1);
    c->name        = g_strdup(src->name);
    c->device_path = g_strdup(src->device_path);
    c->phys_path   = g_strdup(src->phys_path);
    c->vendor_id   = g_strdup(src->vendor_id);
    c->product_id  = g_strdup(src->product_id);
    c->subsystem   = g_strdup(src->subsystem);
    return c;
}

static void source_free(gpointer data) {
    ConttestInputPlumberSource *src = data;
    if (!src) return;
    g_free(src->name);
    g_free(src->device_path);
    g_free(src->phys_path);
    g_free(src->vendor_id);
    g_free(src->product_id);
    g_free(src->subsystem);
    g_free(src);
}

static gboolean source_equal(const ConttestInputPlumberSource *a,
                             const ConttestInputPlumberSource *b) {
    return g_strcmp0(a->name, b->name) == 0 &&
           g_strcmp0(a->device_path, b->device_path) == 0 &&
           g_strcmp0(a->phys_path, b->phys_path) == 0 &&
           g_strcmp0(a->vendor_id, b->vendor_id) == 0 &&
           g_strcmp0(a->product_id, b->product_id) == 0 &&
           g_strcmp0(a->subsystem, b->subsystem) == 0;
}

ConttestInputPlumberInfo *conttest_inputplumber_info_copy(const ConttestInputPlumberInfo *info) {
    if (!info) return NULL;
    ConttestInputPlumberInfo *c = g_new0(ConttestInputPlumberInfo, 1);
    c->daemon_version = g_strdup(info->daemon_version);
    c->composite_path = g_strdup(info->composite_path);
    c->composite_name = g_strdup(info->composite_name);
    c->persistent_id  = g_strdup(info->persistent_id);
    c->profile_name   = g_strdup(info->profile_name);
    c->profile_path   = g_strdup(info->profile_path);
    c->intercept_mode = info->intercept_mode;
    c->target_type    = g_strdup(info->target_type);
    c->target_name    = g_strdup(info->target_name);
    c->target_uniq    = g_strdup(info->target_uniq);
    c->virtual_bus    = g_strdup(info->virtual_bus);

    c->output_caps = g_ptr_array_new_with_free_func(g_free);
    if (info->output_caps) {
        for (guint i = 0; i < info->output_caps->len; i++) {
            g_ptr_array_add(c->output_caps, g_strdup(g_ptr_array_index(info->output_caps, i)));
        }
    }

    c->sources = g_ptr_array_new_with_free_func(source_free);
    if (info->sources) {
        for (guint i = 0; i < info->sources->len; i++) {
            g_ptr_array_add(c->sources, source_copy(g_ptr_array_index(info->sources, i)));
        }
    }
    return c;
}

void conttest_inputplumber_info_free(ConttestInputPlumberInfo *info) {
    if (!info) return;
    g_free(info->daemon_version);
    g_free(info->composite_path);
    g_free(info->composite_name);
    g_free(info->persistent_id);
    g_free(info->profile_name);
    g_free(info->profile_path);
    g_free(info->target_type);
    g_free(info->target_name);
    g_free(info->target_uniq);
    g_free(info->virtual_bus);
    if (info->output_caps) g_ptr_array_free(info->output_caps, TRUE);
    if (info->sources) g_ptr_array_free(info->sources, TRUE);
    g_free(info);
}

gboolean conttest_inputplumber_info_equal(const ConttestInputPlumberInfo *a,
                                          const ConttestInputPlumberInfo *b) {
    if (a == b) return TRUE;
    if (!a || !b) return FALSE;

    if (a->intercept_mode != b->intercept_mode ||
        g_strcmp0(a->daemon_version, b->daemon_version) != 0 ||
        g_strcmp0(a->composite_path, b->composite_path) != 0 ||
        g_strcmp0(a->composite_name, b->composite_name) != 0 ||
        g_strcmp0(a->persistent_id, b->persistent_id) != 0 ||
        g_strcmp0(a->profile_name, b->profile_name) != 0 ||
        g_strcmp0(a->profile_path, b->profile_path) != 0 ||
        g_strcmp0(a->target_type, b->target_type) != 0 ||
        g_strcmp0(a->target_name, b->target_name) != 0 ||
        g_strcmp0(a->target_uniq, b->target_uniq) != 0 ||
        g_strcmp0(a->virtual_bus, b->virtual_bus) != 0) {
        return FALSE;
    }

    guint caps_a = a->output_caps ? a->output_caps->len : 0;
    guint caps_b = b->output_caps ? b->output_caps->len : 0;
    if (caps_a != caps_b) return FALSE;
    for (guint i = 0; i < caps_a; i++) {
        if (g_strcmp0(g_ptr_array_index(a->output_caps, i),
                      g_ptr_array_index(b->output_caps, i)) != 0)
            return FALSE;
    }

    guint src_a = a->sources ? a->sources->len : 0;
    guint src_b = b->sources ? b->sources->len : 0;
    if (src_a != src_b) return FALSE;
    for (guint i = 0; i < src_a; i++) {
        if (!source_equal(g_ptr_array_index(a->sources, i),
                          g_ptr_array_index(b->sources, i)))
            return FALSE;
    }

    return TRUE;
}

/* ── Deterministic ID / MAC / target lookup helpers ──────────────────────── */

guint64 conttest_inputplumber_fnv1a_64(const char *str) {
    guint64 hash = G_GUINT64_CONSTANT(0xcbf29ce484222325);
    if (!str) return hash;
    for (const unsigned char *p = (const unsigned char *)str; *p; p++) {
        hash ^= *p;
        hash *= G_GUINT64_CONSTANT(0x100000001b3);
    }
    return hash;
}

/* Matches InputPlumber's generate_mac() + reversed uniq string formatting. */
void conttest_inputplumber_format_mac_uniq(const guint8 prefix[3],
                                           const char *persistent_id,
                                           char *buf, gsize buf_len) {
    guint64 hash = conttest_inputplumber_fnv1a_64(persistent_id);
    guint8 h5 = (guint8)((hash >> 16) & 0xff);
    guint8 h6 = (guint8)((hash >> 8)  & 0xff);
    guint8 h7 = (guint8)(hash & 0xff);
    g_snprintf(buf, buf_len, "%02x:%02x:%02x:%02x:%02x:%02x",
               h7, h6, h5, prefix[2], prefix[1], prefix[0]);
}

gboolean conttest_inputplumber_target_type_expected_ids(const char *target_type,
                                                        uint16_t *out_vid,
                                                        uint16_t *out_pid) {
    static const struct {
        const char *type;
        uint16_t vid;
        uint16_t pid;
    } TABLE[] = {
        { "ds5",         0x054c, 0x0ce6 },
        { "ds5-edge",    0x054c, 0x0df2 },
        { "xb360",       0x045e, 0x028e },
        { "xbox-elite",  0x045e, 0x0b00 },
        { "xbox-series", 0x045e, 0x0b12 },
        { "8bitdo-u2",   0x2dc8, 0x6012 },
    };
    if (!target_type) return FALSE;
    for (gsize i = 0; i < G_N_ELEMENTS(TABLE); i++) {
        if (g_strcmp0(target_type, TABLE[i].type) == 0) {
            *out_vid = TABLE[i].vid;
            *out_pid = TABLE[i].pid;
            return TRUE;
        }
    }
    return FALSE;
}

gboolean conttest_inputplumber_target_type_has_deterministic_uniq(const char *target_type,
                                                                  const char *persistent_id,
                                                                  char *buf, gsize buf_len) {
    if (!target_type || !persistent_id || !*persistent_id) return FALSE;

    if (g_strcmp0(target_type, "ds5") == 0) {
        static const guint8 p[3] = { 0x02, 0x05, 0xd5 };
        conttest_inputplumber_format_mac_uniq(p, persistent_id, buf, buf_len);
        return TRUE;
    }
    if (g_strcmp0(target_type, "ds5-edge") == 0) {
        static const guint8 p[3] = { 0x02, 0x0e, 0xd6 };
        conttest_inputplumber_format_mac_uniq(p, persistent_id, buf, buf_len);
        return TRUE;
    }
    if (g_strcmp0(target_type, "8bitdo-u2") == 0) {
        static const guint8 p[3] = { 0x02, 0x2d, 0xc8 };
        conttest_inputplumber_format_mac_uniq(p, persistent_id, buf, buf_len);
        return TRUE;
    }
    if (g_strcmp0(target_type, "deck-uhid") == 0) {
        uint64_t hash = conttest_inputplumber_fnv1a_64(persistent_id) & UINT64_C(0xFFFFFFFFFF);
        g_snprintf(buf, buf_len, "%010" PRIX64, hash);
        return TRUE;
    }
    return FALSE;
}

const char *conttest_inputplumber_target_type_virtual_bus(const char *target_type) {
    if (!target_type) return NULL;
    if (g_strcmp0(target_type, "ds5") == 0 ||
        g_strcmp0(target_type, "ds5-edge") == 0 ||
        g_strcmp0(target_type, "8bitdo-u2") == 0 ||
        g_strcmp0(target_type, "deck-uhid") == 0 ||
        g_strcmp0(target_type, "hori-steam") == 0) {
        return "uhid";
    }
    return "uinput";
}

gboolean conttest_inputplumber_is_gamepad_target_type(const char *type) {
    if (!type || !*type) return FALSE;
    static const char *const NON_GAMEPAD[] = {
        "null", "dbus", "keyboard", "mouse", "touchpad", "touchscreen", "debug"
    };
    for (gsize i = 0; i < G_N_ELEMENTS(NON_GAMEPAD); i++) {
        if (g_strcmp0(type, NON_GAMEPAD[i]) == 0) return FALSE;
    }
    return TRUE;
}

/* ── ManagedObjects parser ────────────────────────────────────────────────── */

typedef struct {
    char *device_type;
    char *name;
} TargetRecord;

static void target_record_free(gpointer data) {
    TargetRecord *t = data;
    if (!t) return;
    g_free(t->device_type);
    g_free(t->name);
    g_free(t);
}

static char *dict_dup_string(GVariant *props, const char *key) {
    GVariant *v = g_variant_lookup_value(props, key, G_VARIANT_TYPE_STRING);
    if (!v) return NULL;
    char *out = g_variant_dup_string(v, NULL);
    g_variant_unref(v);
    return out;
}

static guint32 dict_get_uint32(GVariant *props, const char *key) {
    guint32 val = 0;
    g_variant_lookup(props, key, "u", &val);
    return val;
}

static GPtrArray *dict_dup_strv(GVariant *props, const char *key) {
    GPtrArray *arr = g_ptr_array_new_with_free_func(g_free);
    GVariant *v = g_variant_lookup_value(props, key, G_VARIANT_TYPE_STRING_ARRAY);
    if (v) {
        GVariantIter iter;
        const char *item = NULL;
        g_variant_iter_init(&iter, v);
        while (g_variant_iter_next(&iter, "&s", &item)) {
            g_ptr_array_add(arr, g_strdup(item));
        }
        g_variant_unref(v);
    }
    return arr;
}

GPtrArray *conttest_inputplumber_parse_managed_objects(GVariant *reply,
                                                       const char *version,
                                                       GHashTable **out_source_devnodes) {
    GPtrArray *results = g_ptr_array_new_with_free_func((GDestroyNotify)conttest_inputplumber_info_free);
    GHashTable *source_devnodes = NULL;
    if (out_source_devnodes) {
        source_devnodes = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
        *out_source_devnodes = source_devnodes;
    }
    if (!reply || !g_variant_is_of_type(reply, G_VARIANT_TYPE("(a{oa{sa{sv}}})")))
        return results;

    GVariant *objects = g_variant_get_child_value(reply, 0);
    if (!objects) return results;

    GHashTable *sources_by_devpath = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, source_free);
    GHashTable *targets_by_path    = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, target_record_free);
    GPtrArray *composites          = g_ptr_array_new(); /* borrowed (path, props) */

    typedef struct {
        char *path;
        GVariant *props;
    } CompositeRaw;

    GVariantIter obj_iter;
    const char *obj_path = NULL;
    GVariant *ifaces = NULL;
    g_variant_iter_init(&obj_iter, objects);
    while (g_variant_iter_next(&obj_iter, "{&o@a{sa{sv}}}", &obj_path, &ifaces)) {
        GVariant *udev_props = g_variant_lookup_value(ifaces, "org.shadowblip.Input.Source.UdevDevice", G_VARIANT_TYPE_VARDICT);
        if (udev_props) {
            char *dev_path = dict_dup_string(udev_props, "DevicePath");
            if (dev_path && *dev_path) {
                ConttestInputPlumberSource *src = g_new0(ConttestInputPlumberSource, 1);
                src->name        = dict_dup_string(udev_props, "Name");
                src->device_path = g_strdup(dev_path);
                src->phys_path   = dict_dup_string(udev_props, "PhysPath");
                src->vendor_id   = dict_dup_string(udev_props, "IdVendor");
                src->product_id  = dict_dup_string(udev_props, "IdProduct");
                src->subsystem   = dict_dup_string(udev_props, "Subsystem");
                g_hash_table_replace(sources_by_devpath, dev_path, src);
            } else {
                g_free(dev_path);
            }
            g_variant_unref(udev_props);
        }

        GVariant *target_props = g_variant_lookup_value(ifaces, "org.shadowblip.Input.Target", G_VARIANT_TYPE_VARDICT);
        if (target_props) {
            TargetRecord *tr = g_new0(TargetRecord, 1);
            tr->device_type = dict_dup_string(target_props, "DeviceType");
            tr->name        = dict_dup_string(target_props, "Name");
            g_hash_table_replace(targets_by_path, g_strdup(obj_path), tr);
            g_variant_unref(target_props);
        }

        GVariant *comp_props = g_variant_lookup_value(ifaces, "org.shadowblip.Input.CompositeDevice", G_VARIANT_TYPE_VARDICT);
        if (comp_props) {
            CompositeRaw *cr = g_new0(CompositeRaw, 1);
            cr->path  = g_strdup(obj_path);
            cr->props = comp_props;
            g_ptr_array_add(composites, cr);
        }

        g_variant_unref(ifaces);
    }

    for (guint i = 0; i < composites->len; i++) {
        CompositeRaw *cr = g_ptr_array_index(composites, i);
        GVariant *props = cr->props;

        char *comp_name     = dict_dup_string(props, "Name");
        char *persistent_id = dict_dup_string(props, "PersistentId");
        char *profile_name  = dict_dup_string(props, "ProfileName");
        char *profile_path  = dict_dup_string(props, "ProfilePath");
        guint32 intercept   = dict_get_uint32(props, "InterceptMode");
        GPtrArray *src_paths = dict_dup_strv(props, "SourceDevicePaths");
        GPtrArray *tgt_paths = dict_dup_strv(props, "TargetDevices");
        GPtrArray *out_caps  = dict_dup_strv(props, "OutputCapabilities");

        if (source_devnodes) {
            for (guint s = 0; s < src_paths->len; s++) {
                const char *sp = g_ptr_array_index(src_paths, s);
                if (sp && *sp) {
                    g_hash_table_add(source_devnodes, g_strdup(sp));
                }
            }
        }

        for (guint t = 0; t < tgt_paths->len; t++) {
            const char *tgt_path = g_ptr_array_index(tgt_paths, t);
            TargetRecord *tr = g_hash_table_lookup(targets_by_path, tgt_path);
            if (!tr || !conttest_inputplumber_is_gamepad_target_type(tr->device_type)) continue;

            ConttestInputPlumberInfo *info = g_new0(ConttestInputPlumberInfo, 1);
            info->daemon_version = g_strdup(version);
            info->composite_path = g_strdup(cr->path);
            info->composite_name = g_strdup(comp_name);
            info->persistent_id  = g_strdup(persistent_id);
            info->profile_name   = g_strdup(profile_name);
            info->profile_path   = g_strdup(profile_path);
            info->intercept_mode = intercept;
            info->target_type    = g_strdup(tr->device_type);
            info->target_name    = g_strdup(tr->name);

            info->output_caps = g_ptr_array_new_with_free_func(g_free);
            for (guint c = 0; c < out_caps->len; c++) {
                g_ptr_array_add(info->output_caps, g_strdup(g_ptr_array_index(out_caps, c)));
            }

            info->sources = g_ptr_array_new_with_free_func(source_free);
            for (guint s = 0; s < src_paths->len; s++) {
                const char *sp = g_ptr_array_index(src_paths, s);
                ConttestInputPlumberSource *src = g_hash_table_lookup(sources_by_devpath, sp);
                if (src) {
                    g_ptr_array_add(info->sources, source_copy(src));
                } else {
                    ConttestInputPlumberSource *fallback = g_new0(ConttestInputPlumberSource, 1);
                    fallback->device_path = g_strdup(sp);
                    g_ptr_array_add(info->sources, fallback);
                }
            }

            g_ptr_array_add(results, info);
        }

        g_free(comp_name);
        g_free(persistent_id);
        g_free(profile_name);
        g_free(profile_path);
        g_ptr_array_free(src_paths, TRUE);
        g_ptr_array_free(tgt_paths, TRUE);
        g_ptr_array_free(out_caps, TRUE);
        g_variant_unref(props);
        g_free(cr->path);
        g_free(cr);
    }

    g_ptr_array_free(composites, TRUE);
    g_hash_table_destroy(targets_by_path);
    g_hash_table_destroy(sources_by_devpath);
    g_variant_unref(objects);
    return results;
}

/* ── Candidate matching ───────────────────────────────────────────────────── */

static ConttestInputPlumberInfo *clone_matched(const ConttestInputPlumberInfo *cand,
                                               const char *uniq,
                                               const char *virtual_bus,
                                               const char *daemon_version) {
    ConttestInputPlumberInfo *out = conttest_inputplumber_info_copy(cand);
    if (!out) return NULL;
    if (!out->daemon_version && daemon_version)
        out->daemon_version = g_strdup(daemon_version);
    if (uniq && *uniq) {
        g_free(out->target_uniq);
        out->target_uniq = g_strdup(uniq);
    }
    if (virtual_bus && *virtual_bus) {
        g_free(out->virtual_bus);
        out->virtual_bus = g_strdup(virtual_bus);
    }
    return out;
}

typedef enum {
    PASS2_MATCH_NONE = 0,
    PASS2_MATCH_PARTIAL, /* VID/PID or Target.Name matches with non-contradicting IDs */
    PASS2_MATCH_EXACT    /* Both expected VID/PID and Target.Name match */
} Pass2MatchScore;

static Pass2MatchScore score_pass2_candidate(const ConttestInputPlumberInfo *cand,
                                             const char *name,
                                             uint16_t vendor_id,
                                             uint16_t product_id,
                                             const char *virtual_bus) {
    const char *exp_vbus = conttest_inputplumber_target_type_virtual_bus(cand->target_type);
    if (virtual_bus && exp_vbus && g_strcmp0(virtual_bus, exp_vbus) != 0)
        return PASS2_MATCH_NONE;

    /* Deterministic UHID targets always expose their PersistentId-derived
     * uniq; never fall back to VID/PID or name for them, even if an
     * unrelated virtual controller has no uniq. */
    char expected[32] = {0};
    if (conttest_inputplumber_target_type_has_deterministic_uniq(cand->target_type, cand->persistent_id,
                                                                 expected, sizeof(expected))) {
        return PASS2_MATCH_NONE;
    }

    uint16_t exp_vid = 0, exp_pid = 0;
    gboolean has_exp_ids = conttest_inputplumber_target_type_expected_ids(cand->target_type, &exp_vid, &exp_pid);
    gboolean vid_pid_match = has_exp_ids && vendor_id == exp_vid && product_id == exp_pid;
    gboolean ids_contradict =
        has_exp_ids && vendor_id != 0 && product_id != 0 && !vid_pid_match;
    gboolean name_match =
        cand->target_name && *cand->target_name && name && *name &&
        g_strcmp0(cand->target_name, name) == 0;

    if (vid_pid_match && name_match)
        return PASS2_MATCH_EXACT;
    if (!ids_contradict && (vid_pid_match || name_match))
        return PASS2_MATCH_PARTIAL;
    return PASS2_MATCH_NONE;
}

ConttestInputPlumberInfo *conttest_inputplumber_match_candidates(
    GPtrArray *candidates,
    const char *daemon_version,
    const char *name,
    uint16_t vendor_id,
    uint16_t product_id,
    const char *uniq,
    const char *virtual_bus) {
    if (!candidates || candidates->len == 0) return NULL;

    /* Pass 1: Exact deterministic uniq match for targets that hash PersistentId. */
    if (uniq && *uniq) {
        for (guint i = 0; i < candidates->len; i++) {
            ConttestInputPlumberInfo *cand = g_ptr_array_index(candidates, i);
            const char *exp_vbus = conttest_inputplumber_target_type_virtual_bus(cand->target_type);
            if (virtual_bus && exp_vbus && g_strcmp0(virtual_bus, exp_vbus) != 0)
                continue;

            char expected[32] = {0};
            if (conttest_inputplumber_target_type_has_deterministic_uniq(cand->target_type, cand->persistent_id,
                                                                         expected, sizeof(expected))) {
                uint16_t exp_vid = 0, exp_pid = 0;
                gboolean id_ok = TRUE;
                if (conttest_inputplumber_target_type_expected_ids(cand->target_type, &exp_vid, &exp_pid) &&
                    vendor_id != 0 && product_id != 0) {
                    id_ok = (vendor_id == exp_vid && product_id == exp_pid);
                }
                if (id_ok && g_ascii_strcasecmp(uniq, expected) == 0) {
                    return clone_matched(cand, uniq, virtual_bus, daemon_version);
                }
            }
        }
    }

    /* Pass 2: Match by Target.DeviceType VID/PID and/or Target.Name for uinput /
     * unserialized targets on the same virtual bus. Prefer exact dual matches,
     * and refuse ambiguous multi-candidate ties so sibling virtual pads never
     * cross-attribute another CompositeDevice's physical source hardware. */
    Pass2MatchScore best_score = PASS2_MATCH_NONE;
    ConttestInputPlumberInfo *best_cand = NULL;
    guint best_count = 0;

    for (guint i = 0; i < candidates->len; i++) {
        ConttestInputPlumberInfo *cand = g_ptr_array_index(candidates, i);
        Pass2MatchScore score =
            score_pass2_candidate(cand, name, vendor_id, product_id, virtual_bus);
        if (score > best_score) {
            best_score = score;
            best_cand = cand;
            best_count = 1;
        } else if (score != PASS2_MATCH_NONE && score == best_score) {
            best_count++;
        }
    }

    if (best_score != PASS2_MATCH_NONE && best_count == 1 && best_cand) {
        return clone_matched(best_cand, uniq, virtual_bus, daemon_version);
    }

    return NULL;
}
