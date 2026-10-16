/**
 * ui — whisport 屏幕界面（LVGL 9.5）
 *
 * 独立设计的视觉体系（官方规范：二次开发不得沿用 demo 外壳）：
 *   深空底 + 琥珀强调 + 细线分隔 + 像素风开机画面/屏保。
 * 字体：Montserrat 20（标题）/14（正文）/12（元信息）。
 * 中文：默认字体不含 CJK 字形，界面文案保持英文（M4 按字体清单接入中文）。
 *
 * 线程安全：所有 ui_* 函数内部持 bsp_lvgl_lock()，可在任意任务调用。
 */
#include "ui.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "bsp_battery.h"
#include "bsp_display.h"
#include "lvgl.h"

/* 常用汉字子集字体（3500 高频字 + 标点，bpp4；脚本生成，见 scripts/gen_cjk_font.sh） */
LV_FONT_DECLARE(whisport_cjk_14);

/* 14px 正文字体的 RAM 副本：
 * lv_font_montserrat_14 是 const（位于 flash），给它的 .fallback 赋值会触发
 * Store access fault（往 flash 写）→ 必须用 RAM 副本承载 fallback 链 */
static lv_font_t s_font14;
static const lv_font_t *ui_font14(void) {
  return &s_font14;
}

/* ---------------- 视觉令牌 ---------------- */
#define C_BG        0x0d1117
#define C_CARD      0x1b2230
#define C_CARD_HI   0x232c3d
#define C_TEXT      0xe8ecf2
#define C_MUTED     0x7d8798
#define C_ACCENT    0xffb04d
#define C_ACCENT_D  0x8a5a1f
#define C_OK        0x5ec97b
#define C_BAD       0xe06c6c
#define C_DOT       0xff5d5d
#define C_LINE      0x2a3342
#define C_DARK      0x101418

#define CARD_MAX 6
#define CARD_H   60

/* ---------------- 状态 ---------------- */
static lv_obj_t *s_top;          /* 顶栏 */
static lv_obj_t *s_status_label; /* 右：短 device id */
static lv_obj_t *s_key_label;    /* 中：KEY OK / NO KEY */
static lv_obj_t *s_net_label;    /* 左：WIFI / OFFLINE */
static lv_obj_t *s_batt_label;
static lv_obj_t *s_sync_label;

static lv_obj_t *s_list;
static struct {
  lv_obj_t *card;
  lv_obj_t *bar;   /* 优先级色条 */
  lv_obj_t *dot;
  lv_obj_t *title;
  lv_obj_t *meta;
  bool used;
} s_cards[CARD_MAX];
static int s_sel = 0;
static int s_rendered = 0;
static lv_obj_t *s_empty;

static lv_obj_t *s_detail;
static lv_obj_t *s_d_title;
static lv_obj_t *s_d_meta;
static lv_obj_t *s_d_body_box;
static lv_obj_t *s_d_body;

#define MENU_MAX 6
static lv_obj_t *s_menu;
static lv_obj_t *s_menu_rows[MENU_MAX];
static lv_obj_t *s_menu_icons[MENU_MAX];
static lv_obj_t *s_menu_labels[MENU_MAX];
static lv_obj_t *s_menu_arrows[MENU_MAX];
static int s_menu_sel = 0;

static lv_obj_t *s_info;
static lv_obj_t *s_info_title;
static lv_obj_t *s_info_lines[4];

static lv_obj_t *s_splash;
static bool s_splash_on = false;

static ui_screen_t s_screen = UI_SCR_MENU;

static bool with_lock(int timeout_ms) {
  return bsp_lvgl_lock(timeout_ms);
}

static void unlock(void) {
  bsp_lvgl_unlock();
}

/* ---------------- 像素风小组件 ---------------- */

static lv_obj_t *pixel(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color) {
  lv_obj_t *o = lv_obj_create(parent);
  lv_obj_remove_style_all(o);
  lv_obj_set_size(o, w, h);
  lv_obj_set_pos(o, x, y);
  lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
  lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
  lv_obj_set_scroll_dir(o, LV_DIR_NONE);
  return o;
}

static void divider(lv_obj_t *parent, int x, int y, int w, uint32_t color) {
  pixel(parent, x, y, w, 1, color);
}

/* ---------------- 初始化 ---------------- */

static void build_topbar(lv_obj_t *scr) {
  s_top = lv_obj_create(scr);
  lv_obj_remove_style_all(s_top);
  lv_obj_set_size(s_top, 240, 30);
  lv_obj_set_pos(s_top, 0, 0);

  lv_obj_t *title = lv_label_create(s_top);
  lv_label_set_text(title, "whisport");
  lv_obj_set_style_text_color(title, lv_color_hex(C_ACCENT), 0);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_letter_space(title, 1, 0);
  lv_obj_align(title, LV_ALIGN_LEFT_MID, 10, 0);

  s_batt_label = lv_label_create(s_top);
  lv_label_set_text(s_batt_label, "--%");
  lv_obj_set_style_text_color(s_batt_label, lv_color_hex(C_MUTED), 0);
  lv_obj_set_style_text_font(s_batt_label, &lv_font_montserrat_12, 0);
  lv_obj_align(s_batt_label, LV_ALIGN_RIGHT_MID, -8, -7); /* 电量右上角（官方约定） */

  s_sync_label = lv_label_create(s_top);
  lv_label_set_text(s_sync_label, "");
  lv_obj_set_style_text_color(s_sync_label, lv_color_hex(C_ACCENT), 0);
  lv_obj_set_style_text_font(s_sync_label, &lv_font_montserrat_12, 0);
  lv_obj_align(s_sync_label, LV_ALIGN_RIGHT_MID, -8, 8);

  divider(scr, 8, 31, 224, C_LINE);
}

static void build_status_row(lv_obj_t *scr) {
  lv_obj_t *row = lv_obj_create(scr);
  lv_obj_remove_style_all(row);
  lv_obj_set_size(row, 240, 18);
  lv_obj_set_pos(row, 0, 34);

  s_net_label = lv_label_create(row);
  lv_label_set_text(s_net_label, "OFFLINE");
  lv_obj_set_style_text_color(s_net_label, lv_color_hex(C_BAD), 0);
  lv_obj_set_style_text_font(s_net_label, &lv_font_montserrat_12, 0);
  lv_obj_align(s_net_label, LV_ALIGN_LEFT_MID, 10, 0);

  s_key_label = lv_label_create(row);
  lv_label_set_text(s_key_label, "NO KEY");
  lv_obj_set_style_text_color(s_key_label, lv_color_hex(C_ACCENT), 0);
  lv_obj_set_style_text_font(s_key_label, &lv_font_montserrat_12, 0);
  lv_obj_align(s_key_label, LV_ALIGN_CENTER, 0, 0);

  s_status_label = lv_label_create(row);
  lv_label_set_text(s_status_label, "--------");
  lv_obj_set_style_text_color(s_status_label, lv_color_hex(C_MUTED), 0);
  lv_obj_set_style_text_font(s_status_label, &lv_font_montserrat_12, 0);
  lv_obj_align(s_status_label, LV_ALIGN_RIGHT_MID, -10, 0);
}

static void build_ann_cards(lv_obj_t *scr) {
  s_list = lv_obj_create(scr);
  lv_obj_remove_style_all(s_list);
  lv_obj_set_size(s_list, 224, 252);
  lv_obj_align(s_list, LV_ALIGN_BOTTOM_MID, 0, -8);
  lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(s_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
  lv_obj_set_style_pad_row(s_list, 8, 0);
  lv_obj_set_scroll_dir(s_list, LV_DIR_VER);

  for (int i = 0; i < CARD_MAX; i++) {
    lv_obj_t *card = lv_obj_create(s_list);
    lv_obj_set_width(card, 224);
    lv_obj_set_height(card, CARD_H);
    lv_obj_set_style_bg_color(card, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_set_scroll_dir(card, LV_DIR_NONE);
    s_cards[i].card = card;

    /* 优先级色条（左侧 4px） */
    s_cards[i].bar = pixel(card, 0, 0, 4, CARD_H, C_LINE);

    /* 未读红点 */
    s_cards[i].dot = pixel(card, 12, 14, 8, 8, C_DOT);

    s_cards[i].title = lv_label_create(card);
    lv_label_set_long_mode(s_cards[i].title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_cards[i].title, 188);
    lv_obj_set_style_text_color(s_cards[i].title, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_text_font(s_cards[i].title, ui_font14(), 0);
    lv_obj_align(s_cards[i].title, LV_ALIGN_TOP_LEFT, 24, 11);

    s_cards[i].meta = lv_label_create(card);
    lv_label_set_long_mode(s_cards[i].meta, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_cards[i].meta, 188);
    lv_obj_set_style_text_color(s_cards[i].meta, lv_color_hex(C_MUTED), 0);
    lv_obj_set_style_text_font(s_cards[i].meta, &lv_font_montserrat_12, 0);
    lv_obj_align(s_cards[i].meta, LV_ALIGN_BOTTOM_LEFT, 24, -10);

    s_cards[i].used = false;
    lv_obj_add_flag(card, LV_OBJ_FLAG_HIDDEN);
  }

  s_empty = lv_label_create(s_list);
  lv_label_set_text(s_empty, "No announcements yet");
  lv_obj_set_style_text_color(s_empty, lv_color_hex(C_MUTED), 0);
  lv_obj_set_style_text_font(s_empty, ui_font14(), 0);
  lv_obj_align(s_empty, LV_ALIGN_CENTER, 0, 0);
  lv_obj_add_flag(s_empty, LV_OBJ_FLAG_HIDDEN);
}

static void build_detail(lv_obj_t *scr) {
  s_detail = lv_obj_create(scr);
  lv_obj_remove_style_all(s_detail);
  lv_obj_set_size(s_detail, 224, 252);
  lv_obj_align(s_detail, LV_ALIGN_BOTTOM_MID, 0, -8);
  lv_obj_set_style_bg_color(s_detail, lv_color_hex(C_CARD), 0);
  lv_obj_set_style_bg_opa(s_detail, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(s_detail, 0, 0);
  lv_obj_set_style_radius(s_detail, 12, 0);
  lv_obj_set_style_pad_all(s_detail, 12, 0);
  lv_obj_set_scroll_dir(s_detail, LV_DIR_NONE);

  s_d_title = lv_label_create(s_detail);
  lv_label_set_long_mode(s_d_title, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(s_d_title, 198);
  lv_obj_set_style_text_color(s_d_title, lv_color_hex(C_TEXT), 0);
  lv_obj_set_style_text_font(s_d_title, ui_font14(), 0);
  lv_obj_align(s_d_title, LV_ALIGN_TOP_LEFT, 0, 0);

  s_d_meta = lv_label_create(s_detail);
  lv_obj_set_width(s_d_meta, 198);
  lv_label_set_long_mode(s_d_meta, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_color(s_d_meta, lv_color_hex(C_ACCENT), 0);
  lv_obj_set_style_text_font(s_d_meta, &lv_font_montserrat_12, 0);
  lv_obj_align(s_d_meta, LV_ALIGN_TOP_LEFT, 0, 36);

  divider(s_detail, 0, 52, 198, C_LINE);

  s_d_body_box = lv_obj_create(s_detail);
  lv_obj_remove_style_all(s_d_body_box);
  lv_obj_set_size(s_d_body_box, 200, 158);
  lv_obj_align(s_d_body_box, LV_ALIGN_TOP_LEFT, 0, 58);
  lv_obj_set_scroll_dir(s_d_body_box, LV_DIR_VER);
  lv_obj_set_style_pad_right(s_d_body_box, 4, 0);

  s_d_body = lv_label_create(s_d_body_box);
  lv_label_set_long_mode(s_d_body, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(s_d_body, 192);
  lv_obj_set_style_text_color(s_d_body, lv_color_hex(C_TEXT), 0);
  lv_obj_set_style_text_font(s_d_body, ui_font14(), 0);

  lv_obj_t *hint = lv_label_create(s_detail);
  lv_label_set_text(hint, "UP/DN scroll  OK back");
  lv_obj_set_style_text_color(hint, lv_color_hex(C_MUTED), 0);
  lv_obj_set_style_text_font(hint, &lv_font_montserrat_12, 0);
  lv_obj_align(hint, LV_ALIGN_BOTTOM_LEFT, 0, 0);

  lv_obj_add_flag(s_detail, LV_OBJ_FLAG_HIDDEN);
}

static void build_menu(lv_obj_t *scr) {
  static const char *icons[MENU_MAX] = {
      LV_SYMBOL_BELL, LV_SYMBOL_AUDIO, LV_SYMBOL_SETTINGS, LV_SYMBOL_LIST, LV_SYMBOL_SETTINGS, LV_SYMBOL_LIST,
  };

  s_menu = lv_obj_create(scr);
  lv_obj_remove_style_all(s_menu);
  lv_obj_set_size(s_menu, 224, 252);
  lv_obj_align(s_menu, LV_ALIGN_BOTTOM_MID, 0, -8);
  lv_obj_set_flex_flow(s_menu, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(s_menu, 8, 0);
  lv_obj_set_scroll_dir(s_menu, LV_DIR_NONE);

  for (int i = 0; i < MENU_MAX; i++) {
    lv_obj_t *row = lv_obj_create(s_menu);
    lv_obj_set_width(row, 224);
    lv_obj_set_height(row, 47);
    lv_obj_set_style_bg_color(row, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_radius(row, 12, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_scroll_dir(row, LV_DIR_NONE);
    s_menu_rows[i] = row;

    s_menu_icons[i] = lv_label_create(row);
    lv_label_set_text(s_menu_icons[i], icons[i]);
    lv_obj_set_style_text_color(s_menu_icons[i], lv_color_hex(C_ACCENT), 0);
    lv_obj_set_style_text_font(s_menu_icons[i], ui_font14(), 0);
    lv_obj_align(s_menu_icons[i], LV_ALIGN_LEFT_MID, 12, 0);

    s_menu_labels[i] = lv_label_create(row);
    lv_label_set_text(s_menu_labels[i], "");
    lv_obj_set_style_text_color(s_menu_labels[i], lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_text_font(s_menu_labels[i], ui_font14(), 0);
    lv_obj_align(s_menu_labels[i], LV_ALIGN_LEFT_MID, 40, 0);

    s_menu_arrows[i] = lv_label_create(row);
    lv_label_set_text(s_menu_arrows[i], LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(s_menu_arrows[i], lv_color_hex(C_MUTED), 0);
    lv_obj_set_style_text_font(s_menu_arrows[i], ui_font14(), 0);
    lv_obj_align(s_menu_arrows[i], LV_ALIGN_RIGHT_MID, -12, 0);

    lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
  }
}

static void build_info(lv_obj_t *scr) {
  s_info = lv_obj_create(scr);
  lv_obj_remove_style_all(s_info);
  lv_obj_set_size(s_info, 224, 252);
  lv_obj_align(s_info, LV_ALIGN_BOTTOM_MID, 0, -8);
  lv_obj_set_style_bg_color(s_info, lv_color_hex(C_CARD), 0);
  lv_obj_set_style_bg_opa(s_info, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(s_info, 0, 0);
  lv_obj_set_style_radius(s_info, 12, 0);
  lv_obj_set_style_pad_all(s_info, 12, 0);
  lv_obj_set_scroll_dir(s_info, LV_DIR_NONE);

  s_info_title = lv_label_create(s_info);
  lv_label_set_text(s_info_title, "");
  lv_obj_set_style_text_color(s_info_title, lv_color_hex(C_ACCENT), 0);
  lv_obj_set_style_text_font(s_info_title, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_letter_space(s_info_title, 1, 0);
  lv_obj_align(s_info_title, LV_ALIGN_TOP_LEFT, 0, 0);

  divider(s_info, 0, 32, 198, C_LINE);

  for (int i = 0; i < 4; i++) {
    s_info_lines[i] = lv_label_create(s_info);
    lv_label_set_long_mode(s_info_lines[i], LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_info_lines[i], 198);
    lv_label_set_text(s_info_lines[i], "");
    lv_obj_set_style_text_color(s_info_lines[i], lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_text_font(s_info_lines[i], ui_font14(), 0);
    lv_obj_align(s_info_lines[i], LV_ALIGN_TOP_LEFT, 0, 42 + i * 34);
  }
  lv_obj_add_flag(s_info, LV_OBJ_FLAG_HIDDEN);
}

/* 像素风开机画面 / 屏保 */
static void splash_cursor_anim(void *var, int32_t v) {
  lv_obj_set_style_opa((lv_obj_t *)var, (lv_opa_t)v, 0);
}

static void build_splash(lv_obj_t *scr) {
  s_splash = lv_obj_create(scr);
  lv_obj_remove_style_all(s_splash);
  lv_obj_set_size(s_splash, 240, 320);
  lv_obj_set_pos(s_splash, 0, 0);
  lv_obj_set_style_bg_color(s_splash, lv_color_hex(C_BG), 0);
  lv_obj_set_style_bg_opa(s_splash, LV_OPA_COVER, 0);
  lv_obj_set_scroll_dir(s_splash, LV_DIR_NONE);

  /* 像素边框：四边 + 四角 */
  pixel(s_splash, 0, 0, 240, 3, C_ACCENT_D);
  pixel(s_splash, 0, 317, 240, 3, C_ACCENT_D);
  pixel(s_splash, 0, 0, 3, 320, C_ACCENT_D);
  pixel(s_splash, 237, 0, 3, 320, C_ACCENT_D);
  pixel(s_splash, 0, 0, 9, 9, C_ACCENT);
  pixel(s_splash, 231, 0, 9, 9, C_ACCENT);
  pixel(s_splash, 0, 311, 9, 9, C_ACCENT);
  pixel(s_splash, 231, 311, 9, 9, C_ACCENT);

  /* 像素徽标：信号塔（递增高柱 + 顶部点） */
  pixel(s_splash, 96, 126, 8, 10, C_ACCENT_D);
  pixel(s_splash, 108, 120, 8, 16, C_ACCENT);
  pixel(s_splash, 120, 112, 8, 24, C_ACCENT);
  pixel(s_splash, 132, 104, 8, 32, C_ACCENT);
  pixel(s_splash, 128, 92, 6, 6, C_ACCENT);

  lv_obj_t *name = lv_label_create(s_splash);
  lv_label_set_text(name, "WHISPORT");
  lv_obj_set_style_text_color(name, lv_color_hex(C_TEXT), 0);
  lv_obj_set_style_text_font(name, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_letter_space(name, 4, 0);
  lv_obj_align(name, LV_ALIGN_TOP_MID, 0, 176);

  lv_obj_t *tag = lv_label_create(s_splash);
  lv_label_set_text(tag, "DARK GROUP");
  lv_obj_set_style_text_color(tag, lv_color_hex(C_ACCENT), 0);
  lv_obj_set_style_text_font(tag, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_letter_space(tag, 6, 0);
  lv_obj_align(tag, LV_ALIGN_TOP_MID, 0, 210);

  lv_obj_t *tag2 = lv_label_create(s_splash);
  lv_label_set_text(tag2, "COMMUNICATOR");
  lv_obj_set_style_text_color(tag2, lv_color_hex(C_MUTED), 0);
  lv_obj_set_style_text_font(tag2, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_letter_space(tag2, 3, 0);
  lv_obj_align(tag2, LV_ALIGN_TOP_MID, 0, 228);

  /* 像素光标（闪烁） */
  lv_obj_t *cursor = pixel(s_splash, 104, 262, 10, 12, C_ACCENT);
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, cursor);
  lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_20);
  lv_anim_set_time(&a, 700);
  lv_anim_set_playback_time(&a, 700);
  lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
  lv_anim_set_exec_cb(&a, splash_cursor_anim);
  lv_anim_start(&a);

  lv_obj_t *ver = lv_label_create(s_splash);
  lv_label_set_text(ver, "M1.5 · ESP32-C3");
  lv_obj_set_style_text_color(ver, lv_color_hex(C_MUTED), 0);
  lv_obj_set_style_text_font(ver, &lv_font_montserrat_12, 0);
  lv_obj_align(ver, LV_ALIGN_BOTTOM_MID, 0, -18);

  lv_obj_add_flag(s_splash, LV_OBJ_FLAG_HIDDEN);
}

void ui_init(void) {
  /* 建立 RAM 字体副本 + 中文字形回退（Montserrat 无 CJK 字形 → 回退到常用汉字子集） */
  memcpy(&s_font14, &lv_font_montserrat_14, sizeof(lv_font_t));
  s_font14.fallback = &whisport_cjk_14;

  if (!with_lock(1000)) return;
  lv_obj_t *scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), 0);

  build_topbar(scr);
  build_status_row(scr);
  build_ann_cards(scr);
  build_detail(scr);
  build_menu(scr);
  build_info(scr);
  build_splash(scr); /* 最后创建 → 位于最上层 */

  unlock();
}

/* 只显示指定容器（其余全部隐藏），杜绝叠加 */
static void show_only(lv_obj_t *target) {
  lv_obj_t *all[] = { s_menu, s_list, s_detail, s_info, s_splash };
  for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
    if (all[i] == target) lv_obj_clear_flag(all[i], LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(all[i], LV_OBJ_FLAG_HIDDEN);
  }
  s_splash_on = (target == s_splash);
  /* 全屏强制重绘：隐藏/显示切换后局部失效标记可能留下陈旧像素（表现为缺字母） */
  lv_obj_invalidate(lv_scr_act());
}

ui_screen_t ui_current_screen(void) {
  return s_screen;
}

/* ---------------- 屏保 ---------------- */

void ui_show_splash(void) {
  if (!with_lock(400)) return;
  show_only(s_splash);
  s_splash_on = true;
  unlock();
}

void ui_hide_splash(void) {
  if (!with_lock(400)) return;
  s_splash_on = false;
  if (s_screen == UI_SCR_MENU) show_only(s_menu);
  else if (s_screen == UI_SCR_ANN) show_only(s_list);
  else if (s_screen == UI_SCR_DETAIL) show_only(s_detail);
  else show_only(s_info);
  unlock();
}

bool ui_splash_visible(void) {
  return s_splash_on;
}

/* ---------------- 状态 ---------------- */

void ui_set_status(const char *device_id, bool has_secret, bool online) {
  if (!with_lock(500)) return;
  lv_label_set_text(s_net_label, online ? "WIFI" : "OFFLINE");
  lv_obj_set_style_text_color(s_net_label, lv_color_hex(online ? C_OK : C_BAD), 0);

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
    lv_obj_set_style_text_color(s_batt_label, lv_color_hex(soc <= 15 ? C_BAD : C_MUTED), 0);
  } else {
    lv_label_set_text(s_batt_label, "--%");
  }
  unlock();
}

void ui_set_syncing(bool on) {
  if (!with_lock(500)) return;
  lv_label_set_text(s_sync_label, on ? "syncing" : "");
  unlock();
}

/* ---------------- 公告列表 ---------------- */

static void apply_highlight(void) {
  for (int i = 0; i < CARD_MAX; i++) {
    bool on = (i == s_sel) && (i < s_rendered);
    lv_obj_set_style_bg_color(s_cards[i].card, lv_color_hex(on ? C_CARD_HI : C_CARD), 0);
    lv_obj_set_style_border_width(s_cards[i].card, on ? 1 : 0, 0);
    lv_obj_set_style_border_color(s_cards[i].card, lv_color_hex(C_ACCENT), 0);
  }
}

void ui_show_announcements(const app_announcement_list_t *list, long last_read) {
  if (!with_lock(800)) return;
  size_t n = list->count < CARD_MAX ? list->count : CARD_MAX;

  for (size_t i = 0; i < CARD_MAX; i++) {
    if (i < n) {
      const app_announcement_t *a = &list->items[i];
      char meta[56];
      time_t t = (time_t)a->created_at;
      struct tm tmv;
      localtime_r(&t, &tmv);
      strftime(meta, sizeof(meta), "%m-%d %H:%M", &tmv);
      if (a->priority >= 2) strlcat(meta, "  ! URGENT", sizeof(meta));
      else if (a->priority == 1) strlcat(meta, "  ! IMPORTANT", sizeof(meta));

      lv_label_set_text(s_cards[i].title, a->title);
      lv_label_set_text(s_cards[i].meta, meta);
      lv_obj_set_style_bg_color(s_cards[i].bar,
                                lv_color_hex(a->priority >= 2 ? C_BAD
                                             : a->priority == 1 ? C_ACCENT
                                                                : C_LINE),
                                0);
      if (a->id > last_read) lv_obj_clear_flag(s_cards[i].dot, LV_OBJ_FLAG_HIDDEN);
      else lv_obj_add_flag(s_cards[i].dot, LV_OBJ_FLAG_HIDDEN);

      lv_obj_clear_flag(s_cards[i].card, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(s_cards[i].card, LV_OBJ_FLAG_HIDDEN);
    }
  }

  s_rendered = (int)n;
  if (s_rendered == 0) lv_obj_clear_flag(s_empty, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_add_flag(s_empty, LV_OBJ_FLAG_HIDDEN);
  if (s_sel > s_rendered - 1) s_sel = s_rendered > 0 ? s_rendered - 1 : 0;
  apply_highlight();
  unlock();
}

void ui_show_ann_list(void) {
  if (!with_lock(300)) return;
  show_only(s_list);
  s_screen = UI_SCR_ANN;
  unlock();
}

void ui_move_selection(int dir) {
  if (!with_lock(300)) return;
  if (s_rendered <= 0) {
    unlock();
    return;
  }
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

/* ---------------- 详情 ---------------- */

void ui_show_detail(const app_announcement_t *a) {
  if (!with_lock(500)) return;
  lv_label_set_text(s_d_title, a->title);
  char meta[64];
  time_t t = (time_t)a->created_at;
  struct tm tmv;
  localtime_r(&t, &tmv);
  strftime(meta, sizeof(meta), "%Y-%m-%d %H:%M", &tmv);
  if (a->priority >= 2) strlcat(meta, "  ! URGENT", sizeof(meta));
  else if (a->priority == 1) strlcat(meta, "  ! IMPORTANT", sizeof(meta));
  lv_label_set_text(s_d_meta, meta);
  lv_label_set_text(s_d_body, a->body[0] ? a->body : "(no content)");
  lv_obj_scroll_to_y(s_d_body_box, 0, LV_ANIM_OFF);
  show_only(s_detail);
  s_screen = UI_SCR_DETAIL;
  unlock();
}

void ui_hide_detail(void) {
  /* 释放详情正文/标题占用的 LVGL 池（480B 级文本反复进出会加剧碎片） */
  if (with_lock(300)) {
    lv_label_set_text(s_d_title, "");
    lv_label_set_text(s_d_meta, "");
    lv_label_set_text(s_d_body, "");
    unlock();
  }
  ui_show_ann_list();
}

bool ui_detail_visible(void) {
  return s_screen == UI_SCR_DETAIL;
}

void ui_scroll_detail(int dir) {
  if (!with_lock(300)) return;
  lv_obj_scroll_by(s_d_body_box, 0, dir > 0 ? -44 : 44, LV_ANIM_ON);
  unlock();
}

/* ---------------- 主菜单 ---------------- */

static void apply_menu_highlight(void) {
  for (int i = 0; i < MENU_MAX; i++) {
    bool on = (i == s_menu_sel) && !lv_obj_has_flag(s_menu_rows[i], LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_color(s_menu_rows[i], lv_color_hex(on ? C_ACCENT : C_CARD), 0);
    lv_obj_set_style_text_color(s_menu_labels[i], lv_color_hex(on ? C_DARK : C_TEXT), 0);
    lv_obj_set_style_text_color(s_menu_icons[i], lv_color_hex(on ? C_DARK : C_ACCENT), 0);
    lv_obj_set_style_text_color(s_menu_arrows[i], lv_color_hex(on ? C_DARK : C_MUTED), 0);
  }
}

void ui_show_menu(const char *const *items, int count, int sel, int unread) {
  if (!with_lock(500)) return;
  (void)unread;
  if (count > MENU_MAX) count = MENU_MAX;
  s_menu_sel = sel;
  for (int i = 0; i < MENU_MAX; i++) {
    if (i < count) {
      lv_label_set_text(s_menu_labels[i], items[i]);
      lv_obj_clear_flag(s_menu_rows[i], LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(s_menu_rows[i], LV_OBJ_FLAG_HIDDEN);
    }
  }
  apply_menu_highlight();
  show_only(s_menu);
  s_screen = UI_SCR_MENU;
  unlock();
}

void ui_menu_highlight(int sel) {
  if (!with_lock(300)) return;
  s_menu_sel = sel;
  apply_menu_highlight();
  unlock();
}

int ui_menu_selected(void) {
  return s_menu_sel;
}

/* ---------------- 信息屏 ---------------- */

void ui_show_info(const char *title, const char *l1, const char *l2, const char *l3, const char *l4) {
  if (!with_lock(500)) return;
  lv_label_set_text(s_info_title, title);
  const char *lines[4] = { l1, l2, l3, l4 };
  for (int i = 0; i < 4; i++) {
    lv_label_set_text(s_info_lines[i], lines[i] ? lines[i] : "");
  }
  show_only(s_info);
  s_screen = UI_SCR_INFO;
  unlock();
}

void ui_show_intercom(void) {
  ui_show_info("Intercom", "PTT voice calls arrive in M2.", "Planned: persistent channels,", "1:1 calls, PIN-locked rooms.", NULL);
}

/* ---------------- 诊断 ---------------- */

void ui_mem_info(char *buf, size_t len) {
  if (!with_lock(500)) {
    snprintf(buf, len, "lvgl lock busy");
    return;
  }
  lv_mem_monitor_t mon;
  lv_mem_monitor(&mon);
  snprintf(buf, len, "lvgl pool: total=%u free=%u biggest=%u frag=%u%% used=%u%%",
           (unsigned)mon.total_size, (unsigned)mon.free_size,
           (unsigned)mon.free_biggest_size, (unsigned)mon.frag_pct, (unsigned)mon.used_pct);
  unlock();
}

void ui_debug_dump(char *buf, size_t len) {
  if (!with_lock(800)) {
    snprintf(buf, len, "lvgl lock busy");
    return;
  }
  size_t used = 0;
  const char *scr_names[] = { "MENU", "ANN", "DETAIL", "INFO" };
  used += snprintf(buf + used, len - used, "screen=%s splash=%d\n", scr_names[s_screen], s_splash_on);
  for (int i = 0; i < MENU_MAX; i++) {
    const char *t = lv_label_get_text(s_menu_labels[i]);
    bool hidden = lv_obj_has_flag(s_menu_rows[i], LV_OBJ_FLAG_HIDDEN);
    used += snprintf(buf + used, len - used, "row%d%s=[%s]\n", i, hidden ? "(H)" : "", t ? t : "NULL");
    if (used >= len - 8) break;
  }
  for (int i = 0; i < 4; i++) {
    const char *t = lv_label_get_text(s_info_lines[i]);
    used += snprintf(buf + used, len - used, "info%d=[%s]\n", i, t ? t : "NULL");
    if (used >= len - 8) break;
  }
  unlock();
}
