#include "controller_info_parsers.h"
#include "controller_info_utils.h"
#include <glib/gi18n.h>
#include <stdlib.h>
#include <string.h>

/* ── DualShock 3 OUI lookup ───────────────────────────────────────────────── */

/* Genuine Sony OUI database (must stay lexicographically sorted for bsearch). */
static const char * const GENUINE_OUIS[] = {
    "00:02:c7", "00:04:1f", "00:06:f5", "00:06:f7", "00:07:04",
    "00:13:15", "00:15:c1", "00:16:fe", "00:19:c1", "00:19:c5",
    "00:1b:fb", "00:1d:0d", "00:1e:3d", "00:1f:a7", "00:21:4f",
    "00:23:06", "00:24:33", "00:24:8d", "00:26:08", "00:26:43",
    "00:d9:d1", "00:e4:21", "04:76:6e", "04:98:f3", "08:a9:5a",
    "0c:fe:45", "28:0d:fc", "28:a1:83", "2c:cc:44", "30:c3:d9",
    "34:c7:31", "38:c0:96", "44:d8:32", "48:f0:7b", "58:16:d7",
    "60:38:0e", "64:05:e4", "64:d4:bd", "70:9e:29", "74:95:ec",
    "78:c8:81", "9c:8d:7c", "a8:e3:ee", "ac:7a:4d", "b4:ec:02",
    "bc:42:8c", "bc:60:a7", "bc:75:36", "c8:63:f1", "e0:75:0a",
    "e0:ae:5e", "f8:46:1c", "f8:d0:ac", "fc:0f:e6", "fc:62:b9",
};

static int compare_oui_prefix(const void *key, const void *elem) {
    const char *bt_addr = (const char *)key;
    const char *entry = *(const char * const *)elem;
    return g_ascii_strncasecmp(bt_addr, entry, 8);
}

gboolean conttest_ds3_oui_is_genuine(const char *bt_addr) {
    if (!bt_addr || strlen(bt_addr) < 8)
        return FALSE;
    return bsearch(bt_addr, GENUINE_OUIS, G_N_ELEMENTS(GENUINE_OUIS),
                   sizeof(GENUINE_OUIS[0]), compare_oui_prefix) != NULL;
}

gsize conttest_ds3_oui_count(void) {
    return G_N_ELEMENTS(GENUINE_OUIS);
}

const char *conttest_ds3_oui_get(gsize index) {
    g_return_val_if_fail(index < G_N_ELEMENTS(GENUINE_OUIS), NULL);
    return GENUINE_OUIS[index];
}

/* ── DualShock 4 binary parsers & lookup ──────────────────────────────────── */

static const struct {
    guint8 min;
    guint8 max;
    const char *model;
} DS4_BOARD_MODELS[] = {
    { 0x31, 0x31, "JDM-001" },
    { 0x43, 0x43, "JDM-011" },
    { 0x54, 0x54, "JDM-030" },
    { 0x64, 0x74, "JDM-040" },
    { 0x81, 0x83, "JDM-020" },
    { 0x90, 0x90, "JDM-050" },
    { 0x93, 0x93, "JDM-020" },
    { 0xa0, 0xa0, "JDM-050" },
    { 0xa4, 0xa4, "JDM-050" },
    { 0xb0, 0xb0, N_("JDM-055 (Scuf?)") },
    { 0xb4, 0xb4, "JDM-055" },
};

const char *conttest_ds4_hw_to_board_model(guint16 hw_ver_minor) {
    guint8 a = (guint8)(hw_ver_minor >> 8);
    for (gsize i = 0; i < G_N_ELEMENTS(DS4_BOARD_MODELS); i++) {
        if (a >= DS4_BOARD_MODELS[i].min && a <= DS4_BOARD_MODELS[i].max)
            return g_dgettext(NULL, DS4_BOARD_MODELS[i].model);
    }
    return _("Unknown");
}

gboolean conttest_ds4_parse_report_a3(const guint8 *buf, gsize len,
                                      ConttestDS4ReportA3 *out) {
    if (!buf || !out || len < 0x2b || buf[0] != 0xa3)
        return FALSE;

    memset(out, 0, sizeof(*out));
    char *k1 = controller_info_sanitize_hid_string((const char *)(buf + 1), 15);
    char *k2 = controller_info_sanitize_hid_string((const char *)(buf + 0x10), 16);
    char *combined = g_strdup_printf("%s %s", k1 ? k1 : "", k2 ? k2 : "");
    controller_info_copy_utf8(out->build_date, sizeof(out->build_date), combined);
    g_free(combined);
    g_free(k1);
    g_free(k2);

    guint16 hw_major = controller_info_read_le16(buf + 0x21);
    guint16 hw_minor = controller_info_read_le16(buf + 0x23);
    guint32 sw_major = controller_info_read_le32(buf + 0x25);
    guint16 sw_minor = controller_info_read_le16(buf + 0x29);

    controller_info_format(out->hw_version, sizeof(out->hw_version),
                           "%04x:%04x", hw_major, hw_minor);
    controller_info_format(out->sw_version, sizeof(out->sw_version),
                           "%08x:%04x", sw_major, sw_minor);
    controller_info_copy_utf8(out->board_model, sizeof(out->board_model),
                              conttest_ds4_hw_to_board_model(hw_minor));
    return TRUE;
}

gboolean conttest_ds4_parse_report_12_bt(const guint8 *buf, gsize len,
                                         char *bt_address, gsize bt_size) {
    if (!buf || !bt_address || bt_size == 0 || len < 7 || buf[0] != 0x12)
        return FALSE;

    controller_info_format(bt_address, bt_size, "%02x:%02x:%02x:%02x:%02x:%02x",
                           buf[6], buf[5], buf[4], buf[3], buf[2], buf[1]);
    return TRUE;
}

/* ── DualSense binary parsers & lookup ────────────────────────────────────── */

static const struct {
    guint8 min;
    guint8 max;
    const char *model;
} DS5_BOARD_MODELS[] = {
    { 0x03, 0x03, "BDM-010" },
    { 0x04, 0x04, "BDM-020" },
    { 0x05, 0x05, "BDM-030" },
    { 0x06, 0x06, "BDM-040" },
    { 0x07, 0x08, "BDM-050" },
    { 0x11, 0x11, "BDM-060M" },
    { 0x13, 0x13, "BDM-060X" },
};

const char *conttest_ds5_hw_to_board_model(guint32 hwinfo) {
    guint8 a = (guint8)((hwinfo >> 8) & 0xffu);
    for (gsize i = 0; i < G_N_ELEMENTS(DS5_BOARD_MODELS); i++) {
        if (a >= DS5_BOARD_MODELS[i].min && a <= DS5_BOARD_MODELS[i].max)
            return DS5_BOARD_MODELS[i].model;
    }
    return _("Unknown");
}

static const struct {
    const char code[3];
    const char *name;
} DS5_COLORS[] = {
    { "00", N_("White") },
    { "01", N_("Midnight Black") },
    { "02", N_("Cosmic Red") },
    { "03", N_("Nova Pink") },
    { "04", N_("Galactic Purple") },
    { "05", N_("Starlight Blue") },
    { "06", N_("Grey Camouflage") },
    { "07", N_("Volcanic Red") },
    { "08", N_("Sterling Silver") },
    { "09", N_("Cobalt Blue") },
    { "10", N_("Chroma Teal") },
    { "11", N_("Chroma Indigo") },
    { "12", N_("Chroma Pearl") },
    { "30", N_("30th Anniversary") },
    { "Z1", N_("God of War Ragnarök") },
    { "Z2", N_("Spider-Man 2") },
    { "Z3", N_("Astro Bot") },
    { "Z4", N_("Fortnite") },
    { "Z6", N_("The Last of Us") },
    { "ZB", N_("Icon Blue Limited Edition") },
};

const char *conttest_ds5_color_from_serial(const char *serial) {
    if (!serial || strlen(serial) < 6)
        return _("Unknown");
    for (gsize i = 0; i < G_N_ELEMENTS(DS5_COLORS); i++) {
        if (strncmp(serial + 4, DS5_COLORS[i].code, 2) == 0)
            return g_dgettext(NULL, DS5_COLORS[i].name);
    }
    return _("Unknown");
}

char *conttest_ds5_reverse_hex_bytes(const char *hex) {
    if (!hex)
        return NULL;
    gsize len = strlen(hex);
    if (len < 2 || len % 2 != 0)
        return g_strdup(hex);
    char *out = g_malloc(len + 1);
    for (gsize i = 0; i < len / 2; i++) {
        out[i * 2]     = hex[len - 2 - i * 2];
        out[i * 2 + 1] = hex[len - 1 - i * 2];
    }
    out[len] = '\0';
    return out;
}

gboolean conttest_ds5_parse_report_20(const guint8 *buf, gsize len,
                                      ConttestDS5Report20 *out) {
    if (!buf || !out || len < 60 || buf[0] != 0x20)
        return FALSE;

    memset(out, 0, sizeof(*out));
    /* FW Build Date: bytes 1..11 (date) + 12..19 (time). */
    char *date = controller_info_sanitize_hid_string((const char *)(buf + 1), 11);
    char *time_s = controller_info_sanitize_hid_string((const char *)(buf + 12), 8);
    char *combined_date = g_strdup_printf("%s %s", date ? date : "", time_s ? time_s : "");
    controller_info_copy_utf8(out->fw_build_date, sizeof(out->fw_build_date), combined_date);
    g_free(combined_date);
    g_free(date);
    g_free(time_s);

    guint16 fwtype            = controller_info_read_le16(buf + 20);
    guint16 swseries          = controller_info_read_le16(buf + 22);
    guint32 hwinfo            = controller_info_read_le32(buf + 24);
    guint32 fwversion         = controller_info_read_le32(buf + 28);
    guint16 updver            = controller_info_read_le16(buf + 44);
    guint8  update_image_info = buf[46];
    guint32 fwver1            = controller_info_read_le32(buf + 48);
    guint32 fwver2            = controller_info_read_le32(buf + 52);
    guint32 fwver3            = controller_info_read_le32(buf + 56);

    controller_info_format(out->fw_type,           sizeof(out->fw_type),           "0x%04x", fwtype);
    controller_info_format(out->fw_series,         sizeof(out->fw_series),         "0x%04x", swseries);
    controller_info_format(out->fw_version,        sizeof(out->fw_version),        "0x%08x", fwversion);
    controller_info_format(out->fw_update,         sizeof(out->fw_update),         "0x%04x", updver);
    if (g_ascii_isalnum((char)update_image_info)) {
        controller_info_format(out->fw_update_info, sizeof(out->fw_update_info),
                               "0x%02x ('%c')", update_image_info, (char)update_image_info);
    } else {
        controller_info_format(out->fw_update_info, sizeof(out->fw_update_info),
                               "0x%02x", update_image_info);
    }
    controller_info_format(out->sbl_fw_version,    sizeof(out->sbl_fw_version),    "0x%08x", fwver1);
    controller_info_format(out->venom_fw_version,  sizeof(out->venom_fw_version),  "0x%08x", fwver2);
    controller_info_format(out->spider_fw_version, sizeof(out->spider_fw_version), "0x%08x", fwver3);
    controller_info_copy_utf8(out->board_model, sizeof(out->board_model),
                              conttest_ds5_hw_to_board_model(hwinfo));
    controller_info_format(out->hw_model,          sizeof(out->hw_model),          "0x%08x", hwinfo);
    return TRUE;
}

char *conttest_ds5_parse_system_info_response(const guint8 *resp, gsize resp_len,
                                              guint8 base, guint8 num,
                                              gsize length, gboolean as_hex) {
    if (!resp || resp_len < 4 || length > resp_len - 4 || length > (gsize)G_MAXSSIZE)
        return NULL;
    if (resp[0] != 0x81 || resp[1] != base || resp[2] != num || resp[3] != 2)
        return NULL;

    if (as_hex) {
        GString *s = g_string_new("0x");
        for (gsize i = 0; i < length; i++)
            g_string_append_printf(s, "%02x", resp[4 + i]);
        return g_string_free(s, FALSE);
    }
    return controller_info_sanitize_hid_string((const char *)(resp + 4), (gssize)length);
}

gboolean conttest_ds5_parse_bt_addr_response(const guint8 *resp, gsize resp_len,
                                             char *bt_address, gsize bt_size) {
    if (!resp || !bt_address || bt_size == 0 || resp_len < 10)
        return FALSE;
    if (resp[0] != 0x81 || resp[1] != 9 || resp[2] != 2 || resp[3] != 2)
        return FALSE;

    controller_info_format(bt_address, bt_size, "%02x:%02x:%02x:%02x:%02x:%02x",
                           resp[9], resp[8], resp[7], resp[6], resp[5], resp[4]);
    return TRUE;
}
