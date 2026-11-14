#define _POSIX_C_SOURCE 200809L
#include "speaker_routing.h"
#include <pipewire/keys.h>
#include <spa/param/audio/raw-types.h>
#include <spa/utils/keys.h>
#include <string.h>

static void test_parse_int_strict(void) {
    int val = -99;
    g_assert_true(conttest_speaker_parse_int_strict("0", &val));
    g_assert_cmpint(val, ==, 0);

    g_assert_true(conttest_speaker_parse_int_strict("42", &val));
    g_assert_cmpint(val, ==, 42);

    val = 7;
    g_assert_false(conttest_speaker_parse_int_strict(NULL, &val));
    g_assert_false(conttest_speaker_parse_int_strict("", &val));
    g_assert_false(conttest_speaker_parse_int_strict("-1", &val));
    g_assert_false(conttest_speaker_parse_int_strict(" 12", &val));
    g_assert_false(conttest_speaker_parse_int_strict("+12", &val));
    g_assert_false(conttest_speaker_parse_int_strict("12 ", &val));
    g_assert_false(conttest_speaker_parse_int_strict("12abc", &val));
    g_assert_false(conttest_speaker_parse_int_strict("abc", &val));
    g_assert_false(conttest_speaker_parse_int_strict("999999999999999999999", &val));
    g_assert_cmpint(val, ==, 7);
}

static void test_parse_audio_position_prop(void) {
    uint32_t pos[SPA_AUDIO_MAX_CHANNELS];
    uint32_t count = 99;

    conttest_speaker_parse_audio_position_prop(NULL, pos, &count);
    g_assert_cmpuint(count, ==, 0);

    conttest_speaker_parse_audio_position_prop("", pos, &count);
    g_assert_cmpuint(count, ==, 0);

    conttest_speaker_parse_audio_position_prop("[ FL, FR, RL, RR ]", pos, &count);
    g_assert_cmpuint(count, ==, 4);
    g_assert_cmpuint(pos[0], ==, SPA_AUDIO_CHANNEL_FL);
    g_assert_cmpuint(pos[1], ==, SPA_AUDIO_CHANNEL_FR);
    g_assert_cmpuint(pos[2], ==, SPA_AUDIO_CHANNEL_RL);
    g_assert_cmpuint(pos[3], ==, SPA_AUDIO_CHANNEL_RR);

    conttest_speaker_parse_audio_position_prop("[\"FC\", \"LFE\"]", pos, &count);
    g_assert_cmpuint(count, ==, 2);
    g_assert_cmpuint(pos[0], ==, SPA_AUDIO_CHANNEL_FC);
    g_assert_cmpuint(pos[1], ==, SPA_AUDIO_CHANNEL_LFE);

    conttest_speaker_parse_audio_position_prop("MONO", pos, &count);
    g_assert_cmpuint(count, ==, 1);
    g_assert_cmpuint(pos[0], ==, SPA_AUDIO_CHANNEL_MONO);

    /* Ensure excessive entries clamp safely at SPA_AUDIO_MAX_CHANNELS. */
    GString *huge = g_string_new("[");
    for (uint32_t i = 0; i < SPA_AUDIO_MAX_CHANNELS + 16; i++)
        g_string_append(huge, " FL,");
    g_string_append(huge, " ]");
    conttest_speaker_parse_audio_position_prop(huge->str, pos, &count);
    g_assert_cmpuint(count, ==, SPA_AUDIO_MAX_CHANNELS);
    g_string_free(huge, TRUE);
}

static void test_score_sink_props(void) {
    struct spa_dict_item spk_mono_items[] = {
        { PW_KEY_NODE_NAME, "alsa_output.usb-Sony_DualSense-00.pro-output-0-Speaker" },
        { SPA_KEY_AUDIO_CHANNELS, "1" },
    };
    struct spa_dict spk_mono = SPA_DICT_INIT(spk_mono_items, G_N_ELEMENTS(spk_mono_items));

    struct spa_dict_item spk_profile_items[] = {
        { PW_KEY_NODE_NAME, "alsa_output.usb-Sony" },
        { "device.profile.description", "Analog Speaker" },
        { SPA_KEY_AUDIO_CHANNELS, "4" },
    };
    struct spa_dict spk_profile = SPA_DICT_INIT(spk_profile_items, G_N_ELEMENTS(spk_profile_items));

    struct spa_dict_item generic_items[] = {
        { PW_KEY_NODE_NAME, "alsa_output.usb-Sony-00.analog-stereo" },
        { SPA_KEY_AUDIO_CHANNELS, "2" },
    };
    struct spa_dict generic = SPA_DICT_INIT(generic_items, G_N_ELEMENTS(generic_items));

    struct spa_dict_item hp_items[] = {
        { PW_KEY_NODE_NAME, "alsa_output.usb-Sony-00.Headphones" },
        { SPA_KEY_AUDIO_CHANNELS, "2" },
    };
    struct spa_dict hp = SPA_DICT_INIT(hp_items, G_N_ELEMENTS(hp_items));

    g_assert_cmpint(conttest_speaker_score_sink_props(&spk_mono), ==, 105);
    g_assert_cmpint(conttest_speaker_score_sink_props(&spk_profile), ==, 100);
    g_assert_cmpint(conttest_speaker_score_sink_props(&generic), ==, 10);
    g_assert_cmpint(conttest_speaker_score_sink_props(&hp), ==, -20);
}

static void test_build_channel_mask(void) {
    gboolean active[SPA_AUDIO_MAX_CHANNELS];

    /* 0 channels -> all silent */
    struct spa_audio_info_raw empty = { .channels = 0 };
    conttest_speaker_build_channel_mask(&empty, active);
    g_assert_false(active[0]);

    /* 1 channel -> channel 0 active */
    struct spa_audio_info_raw mono = { .channels = 1 };
    conttest_speaker_build_channel_mask(&mono, active);
    g_assert_true(active[0]);
    g_assert_false(active[1]);

    /* 4-ch DualSense USB [FL, FR, RL, RR]: only FL/FR active, RL/RR haptic silent */
    struct spa_audio_info_raw ds5_4ch = {
        .channels = 4,
        .position = {
            SPA_AUDIO_CHANNEL_FL,
            SPA_AUDIO_CHANNEL_FR,
            SPA_AUDIO_CHANNEL_RL,
            SPA_AUDIO_CHANNEL_RR,
        },
    };
    conttest_speaker_build_channel_mask(&ds5_4ch, active);
    g_assert_true(active[0]);
    g_assert_true(active[1]);
    g_assert_false(active[2]);
    g_assert_false(active[3]);

    /* Explicit MONO positioned in multichannel stream -> only MONO channel */
    struct spa_audio_info_raw mixed_mono = {
        .channels = 3,
        .position = {
            SPA_AUDIO_CHANNEL_AUX0,
            SPA_AUDIO_CHANNEL_MONO,
            SPA_AUDIO_CHANNEL_AUX2,
        },
    };
    conttest_speaker_build_channel_mask(&mixed_mono, active);
    g_assert_false(active[0]);
    g_assert_true(active[1]);
    g_assert_false(active[2]);

    /* Unpositioned multichannel stream -> channels 0 and 1 active */
    struct spa_audio_info_raw unpos = {
        .channels = 4,
        .flags = SPA_AUDIO_FLAG_UNPOSITIONED,
    };
    conttest_speaker_build_channel_mask(&unpos, active);
    g_assert_true(active[0]);
    g_assert_true(active[1]);
    g_assert_false(active[2]);
    g_assert_false(active[3]);
}

int main(int argc, char *argv[]) {
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/speaker/parse_int_strict", test_parse_int_strict);
    g_test_add_func("/speaker/parse_audio_position_prop", test_parse_audio_position_prop);
    g_test_add_func("/speaker/score_sink_props", test_score_sink_props);
    g_test_add_func("/speaker/build_channel_mask", test_build_channel_mask);
    return g_test_run();
}
