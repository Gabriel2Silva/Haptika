#include "controller_info_parsers.h"
#include "controller_info_utils.h"
#include <glib.h>
#include <string.h>

static void test_endian_and_utf8_utils(void) {
    const guint8 bytes[] = { 0x34, 0x12, 0x78, 0x56, 0x9a, 0xbc };
    g_assert_cmpuint(controller_info_read_le16(bytes), ==, 0x1234u);
    g_assert_cmpuint(controller_info_read_le32(bytes + 2), ==, 0xbc9a5678u);

    g_assert_null(controller_info_sanitize_hid_string(NULL, 5));

    /* Stops at embedded NUL and preserves earlier printable characters. */
    char *s1 = controller_info_sanitize_hid_string("SONY\0GARBAGE", 12);
    g_assert_cmpstr(s1, ==, "SONY");
    g_free(s1);

    /* Handles non-NUL-terminated buffer bounded by max_len. */
    static const char raw_non_nul[11] = {
        'O', 'c', 't', ' ', '2', '8', ' ', '2', '0', '2', '1'
    };
    char *s2 = controller_info_sanitize_hid_string(raw_non_nul, 11);
    g_assert_cmpstr(s2, ==, "Oct 28 2021");
    g_free(s2);

    /* Strips control characters and trailing spaces while preserving leading UTF-8. */
    char *s3 = controller_info_sanitize_hid_string("AB\x01\nCD\r   \t", -1);
    g_assert_cmpstr(s3, ==, "ABCD");
    g_free(s3);

    /* Repairs invalid UTF-8 sequences cleanly. */
    char *s4 = controller_info_sanitize_hid_string("A\xff\xfe" "B", 4);
    g_assert_true(g_utf8_validate(s4, -1, NULL));
    g_free(s4);

    /* UTF-8 truncation must never leave partial multi-byte characters. */
    const char *rag = "Ragnar\xc3\xb6k"; /* "Ragnarök": 9 bytes, 'ö' at [6..7] */
    for (gsize sz = 1; sz <= 12; sz++) {
        char buf[16];
        memset(buf, 0x55, sizeof(buf));
        controller_info_copy_utf8(buf, sz, rag);
        g_assert_true(g_utf8_validate(buf, -1, NULL));
    }
    char trunc_mid[8]; /* max_bytes = 7 cuts mid-'ö' -> must rewind to "Ragnar" */
    controller_info_copy_utf8(trunc_mid, sizeof(trunc_mid), rag);
    g_assert_cmpstr(trunc_mid, ==, "Ragnar");

    char small_fmt[6];
    controller_info_format(small_fmt, sizeof(small_fmt), "0x%08x", 0x12345678u);
    g_assert_cmpstr(small_fmt, ==, "0x123");
}

static void test_ds3_oui_lookup(void) {
    gsize count = conttest_ds3_oui_count();
    g_assert_cmpuint(count, >, 0);

    for (gsize i = 0; i < count; i++) {
        const char *oui = conttest_ds3_oui_get(i);
        g_assert_nonnull(oui);
        g_assert_cmpuint(strlen(oui), ==, 8);
        if (i > 0) {
            g_assert_cmpint(g_ascii_strcasecmp(conttest_ds3_oui_get(i - 1), oui), <, 0);
        }

        char *mac_lower = g_strdup_printf("%s:aa:bb:cc", oui);
        char *mac_upper = g_ascii_strup(mac_lower, -1);
        g_assert_true(conttest_ds3_oui_is_genuine(mac_lower));
        g_assert_true(conttest_ds3_oui_is_genuine(mac_upper));
        g_free(mac_lower);
        g_free(mac_upper);
    }

    g_assert_false(conttest_ds3_oui_is_genuine("11:22:33:44:55:66"));
    g_assert_false(conttest_ds3_oui_is_genuine("ff:ff:ff:00:00:00"));
    g_assert_false(conttest_ds3_oui_is_genuine("00:02:c"));
    g_assert_false(conttest_ds3_oui_is_genuine(""));
    g_assert_false(conttest_ds3_oui_is_genuine(NULL));
}

static void test_ds4_parsers(void) {
    g_assert_cmpstr(conttest_ds4_hw_to_board_model(0x3100), ==, "JDM-001");
    g_assert_cmpstr(conttest_ds4_hw_to_board_model(0x4300), ==, "JDM-011");
    g_assert_cmpstr(conttest_ds4_hw_to_board_model(0x5400), ==, "JDM-030");
    g_assert_cmpstr(conttest_ds4_hw_to_board_model(0x6400), ==, "JDM-040");
    g_assert_cmpstr(conttest_ds4_hw_to_board_model(0x74ff), ==, "JDM-040");
    g_assert_cmpstr(conttest_ds4_hw_to_board_model(0x8200), ==, "JDM-020");
    g_assert_cmpstr(conttest_ds4_hw_to_board_model(0x9000), ==, "JDM-050");
    g_assert_cmpstr(conttest_ds4_hw_to_board_model(0xb000), ==, "JDM-055 (Scuf?)");
    g_assert_cmpstr(conttest_ds4_hw_to_board_model(0xb400), ==, "JDM-055");
    g_assert_cmpstr(conttest_ds4_hw_to_board_model(0x0100), ==, "Unknown");

    guint8 r[49] = {0};
    r[0] = 0xa3;
    memcpy(r + 1, "Sep 14 2016", 11);
    memcpy(r + 0x10, "05:22:31", 8);
    r[0x21] = 0x01; r[0x22] = 0x02;                 /* hw_major = 0x0201 */
    r[0x23] = 0x05; r[0x24] = 0xb4;                 /* hw_minor = 0xb405 -> JDM-055 */
    r[0x25] = 0x78; r[0x26] = 0x56; r[0x27] = 0x34; r[0x28] = 0x12; /* sw_major = 0x12345678 */
    r[0x29] = 0xef; r[0x2a] = 0xbe;                 /* sw_minor = 0xbeef */

    ConttestDS4ReportA3 parsed;
    g_assert_true(conttest_ds4_parse_report_a3(r, sizeof(r), &parsed));
    g_assert_cmpstr(parsed.build_date, ==, "Sep 14 2016 05:22:31");
    g_assert_cmpstr(parsed.hw_version, ==, "0201:b405");
    g_assert_cmpstr(parsed.sw_version, ==, "12345678:beef");
    g_assert_cmpstr(parsed.board_model, ==, "JDM-055");

    /* Reject truncated buffer or wrong report ID. */
    g_assert_false(conttest_ds4_parse_report_a3(r, 0x2a, &parsed));
    r[0] = 0xa2;
    g_assert_false(conttest_ds4_parse_report_a3(r, sizeof(r), &parsed));

    guint8 r12[7] = { 0x12, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11 };
    char bt[32] = {0};
    g_assert_true(conttest_ds4_parse_report_12_bt(r12, sizeof(r12), bt, sizeof(bt)));
    g_assert_cmpstr(bt, ==, "11:22:33:44:55:66");
    g_assert_false(conttest_ds4_parse_report_12_bt(r12, 6, bt, sizeof(bt)));
}

static void test_ds5_parsers(void) {
    g_assert_cmpstr(conttest_ds5_hw_to_board_model(0x00000300u), ==, "BDM-010");
    g_assert_cmpstr(conttest_ds5_hw_to_board_model(0x00000700u), ==, "BDM-050");
    g_assert_cmpstr(conttest_ds5_hw_to_board_model(0x00000800u), ==, "BDM-050");
    g_assert_cmpstr(conttest_ds5_hw_to_board_model(0x00001100u), ==, "BDM-060M");
    g_assert_cmpstr(conttest_ds5_hw_to_board_model(0x00001300u), ==, "BDM-060X");
    g_assert_cmpstr(conttest_ds5_hw_to_board_model(0x0000ff00u), ==, "Unknown");

    g_assert_cmpstr(conttest_ds5_color_from_serial("F131001234567"), ==, "White");
    g_assert_cmpstr(conttest_ds5_color_from_serial("F131Z1ABCDEF"), ==, "God of War Ragnar\xc3\xb6k");
    g_assert_cmpstr(conttest_ds5_color_from_serial("F131ZBABCDEF"), ==, "Icon Blue Limited Edition");
    g_assert_cmpstr(conttest_ds5_color_from_serial("F131XXABCDEF"), ==, "Unknown");
    g_assert_cmpstr(conttest_ds5_color_from_serial("SHORT"), ==, "Unknown");
    g_assert_cmpstr(conttest_ds5_color_from_serial(NULL), ==, "Unknown");

    g_assert_null(conttest_ds5_reverse_hex_bytes(NULL));
    char *rev1 = conttest_ds5_reverse_hex_bytes("010203ab");
    g_assert_cmpstr(rev1, ==, "ab030201");
    g_free(rev1);
    char *rev_odd = conttest_ds5_reverse_hex_bytes("abc");
    g_assert_cmpstr(rev_odd, ==, "abc");
    g_free(rev_odd);

    guint8 r20[64] = {0};
    r20[0] = 0x20;
    memcpy(r20 + 1, "Nov 01 2023", 11);
    memcpy(r20 + 12, "12:34:56", 8);
    r20[20] = 0x02; r20[21] = 0x00; /* fwtype */
    r20[22] = 0x34; r20[23] = 0x12; /* swseries */
    r20[24] = 0x01; r20[25] = 0x06; r20[26] = 0x00; r20[27] = 0x00; /* hwinfo 0x00000601 -> BDM-040 */
    r20[28] = 0x00; r20[29] = 0x04; r20[30] = 0x02; r20[31] = 0x01; /* fwversion 0x01020400 */
    r20[44] = 0x10; r20[45] = 0x00; /* updver */
    r20[46] = 'A';                  /* update_image_info */
    r20[48] = 0x11; r20[49] = 0x00; r20[50] = 0x00; r20[51] = 0x00;
    r20[52] = 0x22; r20[53] = 0x00; r20[54] = 0x00; r20[55] = 0x00;
    r20[56] = 0x33; r20[57] = 0x00; r20[58] = 0x00; r20[59] = 0x00;

    ConttestDS5Report20 parsed;
    g_assert_true(conttest_ds5_parse_report_20(r20, sizeof(r20), &parsed));
    g_assert_cmpstr(parsed.fw_build_date, ==, "Nov 01 2023 12:34:56");
    g_assert_cmpstr(parsed.fw_type, ==, "0x0002");
    g_assert_cmpstr(parsed.fw_series, ==, "0x1234");
    g_assert_cmpstr(parsed.hw_model, ==, "0x00000601");
    g_assert_cmpstr(parsed.board_model, ==, "BDM-040");
    g_assert_cmpstr(parsed.fw_version, ==, "0x01020400");
    g_assert_cmpstr(parsed.fw_update, ==, "0x0010");
    g_assert_cmpstr(parsed.fw_update_info, ==, "0x41 ('A')");
    g_assert_cmpstr(parsed.sbl_fw_version, ==, "0x00000011");
    g_assert_cmpstr(parsed.venom_fw_version, ==, "0x00000022");
    g_assert_cmpstr(parsed.spider_fw_version, ==, "0x00000033");

    /* Non-alphanumeric update_image_info formats without char suffix. */
    r20[46] = 0x01;
    g_assert_true(conttest_ds5_parse_report_20(r20, sizeof(r20), &parsed));
    g_assert_cmpstr(parsed.fw_update_info, ==, "0x01");
    g_assert_false(conttest_ds5_parse_report_20(r20, 59, &parsed));

    /* getSystemInfo ASCII and hex parsing */
    guint8 sys_ascii[21] = { 0x81, 1, 19, 2 };
    memcpy(sys_ascii + 4, "F13102XYZ98765432", 17);
    char *serial = conttest_ds5_parse_system_info_response(sys_ascii, sizeof(sys_ascii), 1, 19, 17, FALSE);
    g_assert_cmpstr(serial, ==, "F13102XYZ98765432");
    g_free(serial);

    guint8 sys_hex[8] = { 0x81, 1, 9, 2, 0xde, 0xad, 0xbe, 0xef };
    char *hex = conttest_ds5_parse_system_info_response(sys_hex, sizeof(sys_hex), 1, 9, 4, TRUE);
    g_assert_cmpstr(hex, ==, "0xdeadbeef");
    g_free(hex);

    sys_hex[3] = 1; /* wrong status byte */
    g_assert_null(conttest_ds5_parse_system_info_response(sys_hex, sizeof(sys_hex), 1, 9, 4, TRUE));
    sys_hex[3] = 2;
    g_assert_null(conttest_ds5_parse_system_info_response(sys_hex, 7, 1, 9, 4, TRUE));
    g_assert_null(conttest_ds5_parse_system_info_response(sys_hex, 3, 1, 9, 0, FALSE));
    g_assert_null(conttest_ds5_parse_system_info_response(sys_hex, sizeof(sys_hex), 1, 9, G_MAXSIZE - 2, FALSE));

    /* BT address response parsing */
    guint8 bt_resp[10] = { 0x81, 9, 2, 2, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11 };
    char bt[32] = {0};
    g_assert_true(conttest_ds5_parse_bt_addr_response(bt_resp, sizeof(bt_resp), bt, sizeof(bt)));
    g_assert_cmpstr(bt, ==, "11:22:33:44:55:66");
    g_assert_false(conttest_ds5_parse_bt_addr_response(bt_resp, 9, bt, sizeof(bt)));
}

int main(int argc, char *argv[]) {
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/parsers/endian_and_utf8_utils", test_endian_and_utf8_utils);
    g_test_add_func("/parsers/ds3_oui_lookup", test_ds3_oui_lookup);
    g_test_add_func("/parsers/ds4_parsers", test_ds4_parsers);
    g_test_add_func("/parsers/ds5_parsers", test_ds5_parsers);
    return g_test_run();
}
