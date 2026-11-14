#define _POSIX_C_SOURCE 200809L
#include "speaker_routing.h"

#if HAVE_PIPEWIRE

#include <limits.h>
#include <pipewire/keys.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/audio/raw-types.h>
#include <spa/utils/keys.h>
#include <string.h>

gboolean conttest_speaker_parse_int_strict(const char *s, int *out) {
    if (!s || !g_ascii_isdigit((guchar)*s))
        return FALSE;
    char *end = NULL;
    gint64 val = g_ascii_strtoll(s, &end, 10);
    if (end == s || *end != '\0' || val < 0 || val > INT_MAX)
        return FALSE;
    *out = (int)val;
    return TRUE;
}

void conttest_speaker_parse_audio_position_prop(const char *str,
                                                uint32_t *positions,
                                                uint32_t *out_count) {
    *out_count = 0;
    if (!str)
        return;

    const char *p = str;
    while (*p && *out_count < SPA_AUDIO_MAX_CHANNELS) {
        while (*p && (g_ascii_isspace(*p) || *p == '[' || *p == ']' ||
                      *p == ',' || *p == '"')) {
            p++;
        }
        if (!*p)
            break;

        const char *start = p;
        while (*p && !g_ascii_isspace(*p) && *p != '[' && *p != ']' &&
               *p != ',' && *p != '"') {
            p++;
        }

        gsize tok_len = (gsize)(p - start);
        if (tok_len > 0 && tok_len < 32) {
            char tok[32];
            memcpy(tok, start, tok_len);
            tok[tok_len] = '\0';
            positions[*out_count] = spa_type_audio_channel_from_short_name(tok);
            (*out_count)++;
        }
    }
}

int conttest_speaker_score_sink_props(const struct spa_dict *props) {
    const char *node_name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
    const char *prof_name = spa_dict_lookup(props, "device.profile.name");
    const char *prof_desc = spa_dict_lookup(props, "device.profile.description");
    const char *ch_str    = spa_dict_lookup(props, SPA_KEY_AUDIO_CHANNELS);

    int score = 0;
    if ((node_name && strstr(node_name, "Speaker")) ||
        (prof_name && strstr(prof_name, "Speaker")) ||
        (prof_desc && strstr(prof_desc, "Speaker"))) {
        score += 100;
    } else if ((node_name && (strstr(node_name, "Headphones") || strstr(node_name, "SPDIF"))) ||
               (prof_name && (strstr(prof_name, "Headphones") || strstr(prof_name, "SPDIF")))) {
        score -= 20;
    } else {
        score += 10;
    }

    int ch = 0;
    if (conttest_speaker_parse_int_strict(ch_str, &ch) && ch == 1)
        score += 5;

    return score;
}

void conttest_speaker_build_channel_mask(const struct spa_audio_info_raw *info,
                                         gboolean active_ch[SPA_AUDIO_MAX_CHANNELS]) {
    memset(active_ch, 0, sizeof(gboolean) * SPA_AUDIO_MAX_CHANNELS);
    uint32_t channels = MIN(info->channels, (uint32_t)SPA_AUDIO_MAX_CHANNELS);
    if (channels == 0)
        return;
    if (channels == 1) {
        active_ch[0] = TRUE;
        return;
    }

    if (!SPA_FLAG_IS_SET(info->flags, SPA_AUDIO_FLAG_UNPOSITIONED)) {
        gboolean has_mono = FALSE;
        for (uint32_t ch = 0; ch < channels; ch++) {
            if (info->position[ch] == SPA_AUDIO_CHANNEL_MONO) {
                active_ch[ch] = TRUE;
                has_mono = TRUE;
            }
        }
        if (has_mono)
            return;

        /* Drive front/primary speaker channels while leaving haptic rear/AUX2+ silent. */
        gboolean any = FALSE;
        for (uint32_t ch = 0; ch < channels; ch++) {
            uint32_t pos = info->position[ch];
            if (pos == SPA_AUDIO_CHANNEL_FL || pos == SPA_AUDIO_CHANNEL_FR ||
                pos == SPA_AUDIO_CHANNEL_FC || pos == SPA_AUDIO_CHANNEL_AUX0 ||
                pos == SPA_AUDIO_CHANNEL_AUX1) {
                active_ch[ch] = TRUE;
                any = TRUE;
            }
        }
        if (any)
            return;
    }

    active_ch[0] = TRUE;
    active_ch[1] = TRUE;
}

#endif /* HAVE_PIPEWIRE */
