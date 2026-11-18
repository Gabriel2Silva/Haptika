#pragma once

#include "haptika-config.h"

#if HAVE_PIPEWIRE

#include <glib.h>
#include <spa/param/audio/raw.h>
#include <spa/utils/dict.h>
#include <stdint.h>

G_BEGIN_DECLS

/* Strictly parse a non-negative base-10 integer string into *out. */
gboolean conttest_speaker_parse_int_strict(const char *s, int *out);

/* Parse a PipeWire "audio.position" property string (e.g. "[ FL, FR, RL, RR ]")
 * into an array of SPA audio channel IDs. */
void conttest_speaker_parse_audio_position_prop(const char *str,
                                                uint32_t *positions,
                                                uint32_t *out_count);

/* Score a PipeWire Audio/Sink node's property dictionary so controller speaker
 * endpoints rank above headphones or fallback outputs. */
int conttest_speaker_score_sink_props(const struct spa_dict *props);

/* Populate active_ch[] so primary/front speaker channels (MONO or FL/FR/FC/AUX0/1)
 * receive audio while haptic rear channels (RL/RR/AUX2+) stay silent. */
void conttest_speaker_build_channel_mask(const struct spa_audio_info_raw *info,
                                         gboolean active_ch[SPA_AUDIO_MAX_CHANNELS]);

G_END_DECLS

#endif /* HAVE_PIPEWIRE */
