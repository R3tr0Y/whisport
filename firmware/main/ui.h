/**
 * ui — whisport 屏幕界面（LVGL 9.5）
 *
 * 全新设计的独立 UI（官方仓库规范：二次开发禁止沿用 demo 测试菜单/页面外壳）。
 * 视觉：深色卡片流 + 顶部状态条；不引用官方 ui_pixel 配色/组件。
 *
 * 字体注意（官方 ai-guide 中文字体检查清单）：默认 Montserrat 不含中文字形，
 * M1 界面文案使用英文/数字；接入中文前须完成字形覆盖与真机显示验收（M4）。
 *
 * 线程安全：所有 ui_* 函数内部持 bsp_lvgl_lock()，可在任意任务调用。
 */
#ifndef WHISPORT_UI_H
#define WHISPORT_UI_H

#include <stdbool.h>
#include <stddef.h>

#define ANN_TITLE_MAX 80
#define ANN_BODY_MAX 480

typedef struct {
  long id;
  char title[ANN_TITLE_MAX];
  char body[ANN_BODY_MAX];
  int priority;
  long created_at;
} app_announcement_t;

typedef struct {
  app_announcement_t *items;
  size_t count;
  long max_id;
} app_announcement_list_t;

void ui_init(void);

/* 刷新公告卡片列表（latest 优先，未读红点按 last_read 判定；空列表=清屏） */
void ui_show_announcements(const app_announcement_list_t *list, long last_read);

/* 顶部状态：网络/凭证/电量；device_id 取前 8 位展示 */
void ui_set_status(const char *device_id, bool has_secret, bool online);

/* 同步中提示（短暂显示在状态条右侧） */
void ui_set_syncing(bool on);

/* ---- 列表选择与详情视图 ---- */
void ui_move_selection(int dir);        /* 上/下移动高亮（夹紧边界） */
int ui_selected_index(void);
void ui_show_detail(const app_announcement_t *a); /* 打开详情（标题+正文+时间） */
void ui_hide_detail(void);
bool ui_detail_visible(void);
void ui_scroll_detail(int dir);         /* 详情内正文滚动：dir>0 向下 */

#endif /* WHISPORT_UI_H */
