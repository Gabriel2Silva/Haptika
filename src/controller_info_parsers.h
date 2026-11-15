#pragma once

#include <glib.h>
#include <stdint.h>

G_BEGIN_DECLS

/* --- DualShock 3 --- */

gboolean conttest_ds3_oui_is_genuine(const char *bt_addr);
gsize conttest_ds3_oui_count(void);
const char *conttest_ds3_oui_get(gsize index);

/* --- DualShock 4 --- */

typedef struct {
    char build_date[64];
    char hw_version[16];
    char sw_version[16];
    char board_model[64];
} ConttestDS4ReportA3;

const char *conttest_ds4_hw_to_board_model(guint16 hw_ver_minor);
gboolean conttest_ds4_parse_report_a3(const guint8 *buf, gsize len,
                                      ConttestDS4ReportA3 *out);
gboolean conttest_ds4_parse_report_12_bt(const guint8 *buf, gsize len,
                                         char *bt_address, gsize bt_size);

/* --- DualSense --- */

typedef struct {
    char fw_build_date[64];
    char fw_type[12];
    char fw_series[12];
    char fw_version[12];
    char fw_update[12];
    char fw_update_info[16];
    char sbl_fw_version[12];
    char venom_fw_version[12];
    char spider_fw_version[12];
    char board_model[32];
    char hw_model[12];
} ConttestDS5Report20;

const char *conttest_ds5_hw_to_board_model(guint32 hwinfo);
const char *conttest_ds5_color_from_serial(const char *serial);
char *conttest_ds5_reverse_hex_bytes(const char *hex);
gboolean conttest_ds5_parse_report_20(const guint8 *buf, gsize len,
                                      ConttestDS5Report20 *out);
char *conttest_ds5_parse_system_info_response(const guint8 *resp, gsize resp_len,
                                              guint8 base, guint8 num,
                                              gsize length, gboolean as_hex);
gboolean conttest_ds5_parse_bt_addr_response(const guint8 *resp, gsize resp_len,
                                             char *bt_address, gsize bt_size);

G_END_DECLS
