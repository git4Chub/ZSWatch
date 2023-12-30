/*
 * This file is part of ZSWatch project <https://github.com/jakkra/ZSWatch/>.
 * Copyright (c) 2023 Jakob Krantz.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "drivers/zsw_display_control.h"
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/pm/device.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/regulator.h>
#include <zephyr/drivers/display.h>
#include <zephyr/logging/log.h>
#include "lvgl.h"

LOG_MODULE_REGISTER(display_control, LOG_LEVEL_WRN);

static void lvgl_render(struct k_work *item);

typedef enum display_state {
    DISPLAY_STATE_AWAKE,
    DISPLAY_STATE_SLEEPING,
    DISPLAY_STATE_POWERED_OFF,
} display_state_t;

#if defined(CONFIG_BOARD_M5STACK_CORE2)
static const struct pwm_dt_spec display_blk = PWM_DT_SPEC_GET_OR(DT_ALIAS(display_blk), {});
static const struct device *const reg_dev = DEVICE_DT_GET_OR_NULL(DT_PATH(axp192_regulator));
static const struct device *display_dev = DEVICE_DT_GET_OR_NULL(DT_CHOSEN(zephyr_display));
static const struct device *touch_dev =  DEVICE_DT_GET_OR_NULL(DT_NODELABEL(ft5336_touch));
#else
static const struct pwm_dt_spec display_blk = PWM_DT_SPEC_GET_OR(DT_ALIAS(display_blk), {});
static const struct device *const reg_dev = DEVICE_DT_GET_OR_NULL(DT_PATH(regulator_3v3_ctrl));
static const struct device *display_dev = DEVICE_DT_GET_OR_NULL(DT_CHOSEN(zephyr_display));
static const struct device *touch_dev =  DEVICE_DT_GET_OR_NULL(DT_NODELABEL(cst816s));
#endif

K_WORK_DELAYABLE_DEFINE(lvgl_work, lvgl_render);

K_MUTEX_DEFINE(display_mutex);

static struct k_work_sync canel_work_sync;
static display_state_t display_state;
static bool first_render_since_poweron;
static uint8_t last_brightness = 30;

void zsw_display_control_init(void)
{
    if (!device_is_ready(display_dev)) {
        LOG_ERR("Device display not ready.");
    }
    if (!device_is_ready(display_blk.dev)) {
        LOG_WRN("Display brightness control not supported");
    }
    if (!device_is_ready(reg_dev)) {
        LOG_WRN("Display regulator control not supported");
    }
    if (!device_is_ready(touch_dev)) {
        LOG_WRN("Device touch not ready.");
    }

    display_state = DISPLAY_STATE_SLEEPING;
}

int zsw_display_control_sleep_ctrl(bool on)
{
    int res = -EALREADY;

    k_mutex_lock(&display_mutex, K_FOREVER);

    switch (display_state) {
        case DISPLAY_STATE_AWAKE:
            if (on) {
                LOG_DBG("Display already awake");
                res = -EALREADY;
            } else {
                LOG_DBG("Put display to sleep");
                display_state = DISPLAY_STATE_SLEEPING;
                display_blanking_on(display_dev);
                // Suspend the display and touch chip
                pm_device_action_run(display_dev, PM_DEVICE_ACTION_SUSPEND);
                if (device_is_ready(touch_dev)) {
                    pm_device_action_run(touch_dev, PM_DEVICE_ACTION_SUSPEND);
                }
                // Turn off PWM peripheral as it consumes like 200-250uA
                zsw_display_control_set_brightness(0);
                // Cancel pending call to lv_task_handler
                // Don't waste resosuces to rendering when display is off anyway.
                k_work_cancel_delayable_sync(&lvgl_work, &canel_work_sync);
                // Prepare for next call to lv_task_handler when screen is enabled again,
                // Since the display will have been powered off, we need to tell LVGL
                // to rerender the complete display.
                lv_obj_invalidate(lv_scr_act());
                res = 0;
            }
            break;
        case DISPLAY_STATE_SLEEPING:
            if (on) {
                LOG_DBG("Wake up display");
                display_state = DISPLAY_STATE_AWAKE;
                // Resume the display and touch chip
                pm_device_action_run(display_dev, PM_DEVICE_ACTION_RESUME);
                if (device_is_ready(touch_dev)) {
                    pm_device_action_run(touch_dev, PM_DEVICE_ACTION_RESUME);
                }
                // Turn backlight on, unless the display was off,
                // then wait to show content until rendering completes.
                // This avoids user seeing random pixel data for ~500ms
                if (!first_render_since_poweron) {
                    zsw_display_control_set_brightness(last_brightness);
                }
                display_blanking_off(display_dev);
                k_work_schedule(&lvgl_work, K_MSEC(250));
                res = 0;
            } else {
                LOG_DBG("Display already sleeping");
                res = -EALREADY;
            }
            break;
        case DISPLAY_STATE_POWERED_OFF:
            if (on) {
                LOG_DBG("Display is OFF, power on before exiting sleep");
            } else {
                LOG_DBG("Display is OFF, cannot enter sleep");
            }
            res = -EIO;
            break;
    }

    k_mutex_unlock(&display_mutex);

    return res;
}

int zsw_display_control_pwr_ctrl(bool on)
{
    int res = -EALREADY;

    k_mutex_lock(&display_mutex, K_FOREVER);

    switch (display_state) {
        case DISPLAY_STATE_AWAKE:
            if (on) {
                LOG_DBG("Display awake, power already on");
            } else {
                LOG_DBG("Display awake, sleep before power off");
            }
            break;
        case DISPLAY_STATE_SLEEPING:
            if (on) {
                LOG_DBG("Display sleeping, power already on");
            } else {
                LOG_DBG("Display sleeping, power off");
                if (device_is_ready(reg_dev)) {
                    display_state = DISPLAY_STATE_POWERED_OFF;
#ifndef CONFIG_BOARD_NATIVE_POSIX
                    regulator_disable(reg_dev);
#endif
                    pm_device_action_run(display_dev, PM_DEVICE_ACTION_TURN_OFF);
                    res = 0;
                }
            }
            break;
        case DISPLAY_STATE_POWERED_OFF:
            if (on) {
                LOG_DBG("Display is off, power already on");
                if (device_is_ready(reg_dev)) {
                    display_state = DISPLAY_STATE_SLEEPING;
#ifndef CONFIG_BOARD_NATIVE_POSIX
                    regulator_enable(reg_dev);
#endif
                    pm_device_action_run(display_dev, PM_DEVICE_ACTION_TURN_ON);
                    first_render_since_poweron = true;
                    res = 0;
                }
            } else {
                LOG_DBG("Display is off, power already off");
            }
            break;
    }

    k_mutex_unlock(&display_mutex);

    return res;
}

uint8_t zsw_display_control_get_brightness(void)
{
    return last_brightness;
}

void zsw_display_control_set_brightness(uint8_t percent)
{
    if (!device_is_ready(display_blk.dev)) {
        return;
    }
    __ASSERT(percent >= 0 && percent <= 100, "Invalid range for brightness, valid range 0-100, was %d", percent);
    int ret;

    k_mutex_lock(&display_mutex, K_FOREVER);

    // TODO this is not correct, the FAN5622SX LED driver have 32 different brightness levels
    // and we need to take that into consideration when choosing pwm period and pulse width.
    uint32_t pulse_width = percent * (display_blk.period / 100);

    if (display_state != DISPLAY_STATE_AWAKE && percent != 0) {
        LOG_WRN("Setting brightness when display is off may cause issues with active/inactive state, make sure you know what you are doing.");
    }

    if (percent != 0) {
        last_brightness = percent;
    }
    ret = pwm_set_pulse_dt(&display_blk, pulse_width);
    __ASSERT(ret == 0, "pwm error: %d for pulse: %d", ret, pulse_width);

    k_mutex_unlock(&display_mutex);
}

static void lvgl_render(struct k_work *item)
{
    const int64_t next_update_in_ms = lv_task_handler();
    if (first_render_since_poweron) {
        zsw_display_control_set_brightness(last_brightness);
        first_render_since_poweron = false;
    }
    k_work_schedule(&lvgl_work, K_MSEC(next_update_in_ms));
}