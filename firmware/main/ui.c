#include "ui.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "bsp_battery.h"
#include "bsp_display.h"
#include "lvgl.h"

/* 自有配色（不引用官方 demo 的 ui_pixel 色板/组件——官方规范：UI 须独立重设计） */
#define C_BG      0x0f1318
#define C_CARD    0x1a212b
#define C_TEXT    0xe6e9ef
#define C_MUTED   0x8b94a3
#define C_ACCENT  0xe8a33d
#define C_OK      0x6fcf87
#define C_BAD     0xe07070
#define C_DOT     0xff5555

#define CARD_MAX 6

static lv_obj_t *s_status_label;   /* 右：短 device id */
static lv_obj_t *s_key_label;     /* 中：KEY OK / NO KEY */
static lv_obj_t *s_net_label;     /* 左：WIFI / OFFLINE */
static lv_obj_t *s_batt_label;
static lv_obj_t *s_sync_label;
static lv_obj_t *s_list;
static lv_obj_t *s_detail;       /* 详情视图（覆盖列表区） */
static lv_obj_t *s_d_title;
static lv_obj_t *s_d_meta;
static lv_obj_t *s_d_body_box;
static lv_obj_t *s_d_body;
static int s_sel = 0;            /* 列表高亮索引 */
static int s_rendered = 0;       /* 当前渲染的卡片数 */
static struct {
  lv_obj_t *card;
  lv_obj_t *dot;
  lv_obj_t *title;
  lv_obj_t *meta;
  bool used;
} s_cards[CARD_MAX];

static bool with_lock(int timeout_ms) {
  return bsp_lvgl_lock(timeout_ms);
}

static void unlock(void) {
  bsp_lvgl_unlock();
}

void ui_init(void) {
  if (!with_lock(1000)) return;
  lv_obj_t *scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), 0);

  /* 顶栏 */
  lv_obj_t *top = lv_obj_create(scr);
  lv_obj_remove_style_all(top);
  lv_obj_set_size(top, 240, 28);
  lv_obj_align(top, LV_ALIGN_TOP_MID, 0, 0);

  lv_obj_t *title = lv_label_create(top);
  lv_label_set_text(title, "whisport");
  lv_obj_set_style_text_color(title, lv_color_hex(C_ACCENT), 0);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
  lv_obj_align(title, LV_ALIGN_LEFT_MID, 8, 0);

  s_batt_label = lv_label_create(top);
  lv_label_set_text(s_batt_label, "--%");
  lv_obj_set_style_text_color(s_batt_label, lv_color_hex(C_MUTED), 0);
  lv_obj_set_style_text_font(s_batt_label, &lv_font_montserrat_14, 0);
  lv_obj_align(s_batt_label, LV_ALIGN_RIGHT_MID, -6, -6); /* 电量右上角（官方约定） */

  s_sync_label = lv_label_create(top);
  lv_label_set_text(s_sync_label, "");
  lv_obj_set_style_text_color(s_sync_label, lv_color_hex(C_ACCENT), 0);
  lv_obj_set_style_text_font(s_sync_label, &lv_font_montserrat_14, 0);
  lv_obj_align(s_sync_label, LV_ALIGN_RIGHT_MID, -6, 8);

  /* 状态行：网络 / 凭证 / 设备号 */
  lv_obj_t *status_row = lv_obj_create(scr);
  lv_obj_remove_style_all(status_row);
  lv_obj_set_size(status_row, 240, 20);
  lv_obj_align(status_row, LV_ALIGN_TOP_MID, 0, 30);

  s_net_label = lv_label_create(status_row);
  lv_label_set_text(s_net_label, "OFFLINE");
  lv_obj_set_style_text_color(s_net_label, lv_color_hex(C_BAD), 0);
  lv_obj_set_style_text_font(s_net_label, &lv_font_montserrat_14, 0);
  lv_obj_align(s_net_label, LV_ALIGN_LEFT_MID, 8, 0);

  s_key_label = lv_label_create(status_row);
  lv_label_set_text(s_key_label, "NO KEY");
  lv_obj_set_style_text_color(s_key_label, lv_color_hex(C_ACCENT), 0);
  lv_obj_set_style_text_font(s_key_label, &lv_font_montserrat_14, 0);
  lv_obj_align(s_key_label, LV_ALIGN_CENTER, 0, 0);

  s_status_label = lv_label_create(status_row);
  lv_label_set_text(s_status_label, "--------");
  lv_obj_set_style_text_color(s_status_label, lv_color_hex(C_MUTED), 0);
  lv_obj_set_style_text_font(s_status_label, &lv_font_montserrat_14, 0);
  lv_obj_align(s_status_label, LV_ALIGN_RIGHT_MID, -8, 0);

  /* 公告卡片流 */
  s_list = lv_obj_create(scr);
  lv_obj_remove_style_all(s_list);
  lv_obj_set_size(s_list, 224, 252);
  lv_obj_align(s_list, LV_ALIGN_BOTTOM_MID, 0, -6);
  lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(s_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
  lv_obj_set_style_pad_row(s_list, 6, 0);
  lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
  lv_obj_set_style_pad_ver(s_list, 2, 0);

  for (int i = 0; i < CARD_MAX; i++) {
    s_cards[i].card = lv_obj_create(s_list);
    lv_obj_set_width(s_cards[i].card, 224);
    lv_obj_set_height(s_cards[i].card, 58);
    lv_obj_set_style_bg_color(s_cards[i].card, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_radius(s_cards[i].card, 10, 0);
    lv_obj_set_style_pad_all(s_cards[i].card, 8, 0);
    lv_obj_set_scroll_dir(s_cards[i].card, LV_DIR_NONE);

    s_cards[i].dot = lv_obj_create(s_cards[i].card);
    lv_obj_remove_style_all(s_cards[i].dot);
    lv_obj_set_size(s_cards[i].dot, 6, 6);
    lv_obj_set_style_bg_color(s_cards[i].dot, lv_color_hex(C_DOT), 0);
    lv_obj_set_style_radius(s_cards[i].dot, 3, 0);
    lv_obj_align(s_cards[i].dot, LV_ALIGN_TOP_LEFT, 0, 2);

    s_cards[i].title = lv_label_create(s_cards[i].card);
    lv_label_set_long_mode(s_cards[i].title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_cards[i].title, 190);
    lv_obj_set_style_text_color(s_cards[i].title, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_text_font(s_cards[i].title, &lv_font_montserrat_14, 0);
    lv_obj_align(s_cards[i].title, LV_ALIGN_TOP_LEFT, 12, 0);

    s_cards[i].meta = lv_label_create(s_cards[i].card);
    lv_label_set_long_mode(s_cards[i].meta, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_cards[i].meta, 190);
    lv_obj_set_style_text_color(s_cards[i].meta, lv_color_hex(C_MUTED), 0);
    lv_obj_set_style_text_font(s_cards[i].meta, &lv_font_montserrat_14, 0);
    lv_obj_align(s_cards[i].meta, LV_ALIGN_TOP_LEFT, 12, 24);

    s_cards[i].used = false;
    lv_obj_add_flag(s_cards[i].card, LV_OBJ_FLAG_HIDDEN);
  }

  /* ---- 详情视图（覆盖列表区，默认隐藏）---- */
  s_detail = lv_obj_create(scr);
  lv_obj_remove_style_all(s_detail);
  lv_obj_set_size(s_detail, 224, 252);
  lv_obj_align(s_detail, LV_ALIGN_BOTTOM_MID, 0, -6);
  lv_obj_set_style_bg_color(s_detail, lv_color_hex(C_CARD), 0);
  lv_obj_set_style_bg_opa(s_detail, LV_OPA_COVER, 0); /* 不透明，避免与列表叠加 */
  lv_obj_set_style_border_width(s_detail, 0, 0);
  lv_obj_set_style_radius(s_detail, 10, 0);
  lv_obj_set_style_pad_all(s_detail, 10, 0);
  lv_obj_set_scroll_dir(s_detail, LV_DIR_NONE);

  s_d_title = lv_label_create(s_detail);
  lv_label_set_long_mode(s_d_title, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(s_d_title, 200);
  lv_obj_set_style_text_color(s_d_title, lv_color_hex(C_TEXT), 0);
  lv_obj_set_style_text_font(s_d_title, &lv_font_montserrat_14, 0);
  lv_obj_align(s_d_title, LV_ALIGN_TOP_LEFT, 0, 0);

  s_d_meta = lv_label_create(s_detail);
  lv_obj_set_width(s_d_meta, 200);
  lv_label_set_long_mode(s_d_meta, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_color(s_d_meta, lv_color_hex(C_ACCENT), 0);
  lv_obj_set_style_text_font(s_d_meta, &lv_font_montserrat_14, 0);
  lv_obj_align(s_d_meta, LV_ALIGN_TOP_LEFT, 0, 34);

  s_d_body_box = lv_obj_create(s_detail);
  lv_obj_remove_style_all(s_d_body_box);
  lv_obj_set_size(s_d_body_box, 204, 168);
  lv_obj_align(s_d_body_box, LV_ALIGN_TOP_LEFT, 0, 56);
  lv_obj_set_scroll_dir(s_d_body_box, LV_DIR_VER);
  lv_obj_set_style_pad_right(s_d_body_box, 4, 0);

  s_d_body = lv_label_create(s_d_body_box);
  lv_label_set_long_mode(s_d_body, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(s_d_body, 196);
  lv_obj_set_style_text_color(s_d_body, lv_color_hex(C_TEXT), 0);
  lv_obj_set_style_text_font(s_d_body, &lv_font_montserrat_14, 0);

  lv_obj_t *hint = lv_label_create(s_detail);
  lv_label_set_text(hint, "UP/DN scroll  OK back");
  lv_obj_set_style_text_color(hint, lv_color_hex(C_MUTED), 0);
  lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
  lv_obj_align(hint, LV_ALIGN_BOTTOM_LEFT, 0, 0);

  lv_obj_add_flag(s_detail, LV_OBJ_FLAG_HIDDEN);

  unlock();
}

/* ---- 列表选择 ---- */

static void apply_highlight(void) {
  for (int i = 0; i < CARD_MAX; i++) {
    bool on = (i == s_sel) && (i < s_rendered);
    lv_obj_set_style_border_width(s_cards[i].card, on ? 2 : 0, 0);
    lv_obj_set_style_border_color(s_cards[i].card, lv_color_hex(C_ACCENT), 0);
  }
}

void ui_move_selection(int dir) {
  if (!with_lock(300)) return;
  if (s_rendered <= 0) { unlock(); return; }
  int next = s_sel + dir;
  if (next < 0) next = 0;
  if (next > s_rendered - 1) next = s_rendered - 1;
  if (next != s_sel) {
    s_sel = next;
    apply_highlight();
    lv_obj_scroll_to_view(s_cards[s_sel].card, LV_ANIM_ON);
  }
  unlock();
}

int ui_selected_index(void) {
  return s_sel;
}

/* ---- 详情视图 ---- */

void ui_show_detail(const app_announcement_t *a) {
  if (!with_lock(500)) return;
  lv_label_set_text(s_d_title, a->title);
  char meta[64];
  time_t t = (time_t)a->created_at;
  struct tm tmv;
  localtime_r(&t, &tmv);
  strftime(meta, sizeof(meta), "%Y-%m-%d %H:%M", &tmv);
  if (a->priority >= 2) strlcat(meta, "  URGENT", sizeof(meta));
  else if (a->priority == 1) strlcat(meta, "  IMPORTANT", sizeof(meta));
  lv_label_set_text(s_d_meta, meta);
  lv_label_set_text(s_d_body, a->body[0] ? a->body : "(no content)");
  lv_obj_scroll_to_y(s_d_body_box, 0, LV_ANIM_OFF);
  lv_obj_add_flag(s_list, LV_OBJ_FLAG_HIDDEN); /* 隐藏列表，杜绝叠加 */
  lv_obj_clear_flag(s_detail, LV_OBJ_FLAG_HIDDEN);
  unlock();
}

void ui_hide_detail(void) {
  if (!with_lock(300)) return;
  lv_obj_add_flag(s_detail, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(s_list, LV_OBJ_FLAG_HIDDEN);
  unlock();
}

bool ui_detail_visible(void) {
  if (!with_lock(300)) return false;
  bool v = !lv_obj_has_flag(s_detail, LV_OBJ_FLAG_HIDDEN);
  unlock();
  return v;
}

void ui_scroll_detail(int dir) {
  if (!with_lock(300)) return;
  lv_obj_scroll_by(s_d_body_box, 0, dir > 0 ? -44 : 44, LV_ANIM_ON);
  unlock();
}

void ui_set_status(const char *device_id, bool has_secret, bool online) {
  if (!with_lock(500)) return;
  lv_label_set_text(s_net_label, online ? "WIFI" : "OFFLINE");
  lv_obj_set_style_text_color(s_net_label, lv_color_hex(online ? C_OK : C_BAD), 0);

  /* 三段式：左 网络 / 中 密钥 / 右 短 id（纯 ASCII，避免 Montserrat 缺字形） */
  lv_label_set_text(s_key_label, has_secret ? "KEY OK" : "NO KEY");
  lv_obj_set_style_text_color(s_key_label, lv_color_hex(has_secret ? C_OK : C_ACCENT), 0);

  char short_id[9] = "--------";
  if (device_id) {
    memcpy(short_id, device_id, 8);
    short_id[8] = '\0';
  }
  lv_label_set_text(s_status_label, short_id);

  int soc = bsp_battery_soc();
  if (soc >= 0) {
    if (soc > 100) soc = 100;
    char b[8];
    snprintf(b, sizeof(b), "%d%%", soc);
    lv_label_set_text(s_batt_label, b);
  } else {
    lv_label_set_text(s_batt_label, "--%");
  }
  unlock();
}

void ui_set_syncing(bool on) {
  if (!with_lock(500)) return;
  lv_label_set_text(s_sync_label, on ? "sync..." : "");
  unlock();
}

void ui_show_announcements(const app_announcement_list_t *list, long last_read) {
  if (!with_lock(800)) return;
  size_t n = list->count < CARD_MAX ? list->count : CARD_MAX;

  /* 调用方保证 items 为最新优先 */
  for (size_t i = 0; i < CARD_MAX; i++) {
    if (i < n) {
      const app_announcement_t *a = &list->items[i];
      char meta[64];
      time_t t = (time_t)a->created_at;
      struct tm tmv;
      localtime_r(&t, &tmv);
      strftime(meta, sizeof(meta), "%m-%d %H:%M", &tmv);
      if (a->priority >= 2) strlcat(meta, "  URGENT", sizeof(meta));
      else if (a->priority == 1) strlcat(meta, "  IMPORTANT", sizeof(meta));

      lv_label_set_text(s_cards[i].title, a->title);
      lv_label_set_text(s_cards[i].meta, meta);
      if (a->id > last_read) {
        lv_obj_clear_flag(s_cards[i].dot, LV_OBJ_FLAG_HIDDEN);
      } else {
        lv_obj_add_flag(s_cards[i].dot, LV_OBJ_FLAG_HIDDEN);
      }
      lv_obj_clear_flag(s_cards[i].card, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(s_cards[i].card, LV_OBJ_FLAG_HIDDEN);
    }
  }

  s_rendered = (int)n;
  if (s_sel > s_rendered - 1) s_sel = s_rendered > 0 ? s_rendered - 1 : 0;
  apply_highlight();
  unlock();
}
