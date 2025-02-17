#ifndef __NOTIFICATION_UI_H
#define __NOTIFICATION_UI_H

#include <inttypes.h>
#include <lvgl.h>

#include "managers/zsw_notification_manager.h"

typedef void(*on_notification_remove_cb_t)(uint32_t id);

void notifications_page_init(on_notification_remove_cb_t not_removed_cb);

void notifications_page_create(zsw_not_mngr_notification_t *notifications, uint8_t num_notifications,
                               lv_group_t *input_group);

void notifications_page_close(void);
#endif // __NOTIFICATION_UI_H