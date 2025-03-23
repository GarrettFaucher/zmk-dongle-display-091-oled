/*
* Copyright (c) 2024 The ZMK Contributors
*
* SPDX-License-Identifier: MIT
*/

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/services/bas.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/battery.h>
#include <zmk/ble.h>
#include <zmk/display.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/event_manager.h>
#include <zmk/usb.h>

#include "battery_status.h"

#if IS_ENABLED(CONFIG_ZMK_DONGLE_DISPLAY_DONGLE_BATTERY)
    #define SOURCE_OFFSET 1
#else
    #define SOURCE_OFFSET 0
#endif

/* TOTAL_SLOTS covers the central (if any) plus all peripherals */
#define TOTAL_SLOTS (ZMK_SPLIT_BLE_PERIPHERAL_COUNT + SOURCE_OFFSET)

/* Global widget list */
static sys_slist_t widgets = SYS_SLIST_STATIC_INIT(&widgets);

/* This structure is used to store battery state for a given slot */
struct battery_state {
    uint8_t source;
    uint8_t level;
    bool usb_present;
};

/* Global persistent state for each battery slot */
static struct battery_state battery_states[TOTAL_SLOTS];
static bool battery_state_valid[TOTAL_SLOTS] = { false };

/* Buffer for drawing battery icons (one per slot) */
static lv_color_t battery_image_buffer[TOTAL_SLOTS][5 * 8];

/* Draw a battery icon on the provided canvas */
static void draw_battery(lv_obj_t *canvas, uint8_t level, bool usb_present)
{
    lv_canvas_fill_bg(canvas, lv_color_black(), LV_OPA_COVER);

    lv_draw_rect_dsc_t rect_fill_dsc;
    lv_draw_rect_dsc_init(&rect_fill_dsc);

    if (usb_present) {
        rect_fill_dsc.bg_opa = LV_OPA_TRANSP;
        rect_fill_dsc.border_color = lv_color_white();
        rect_fill_dsc.border_width = 1;
    }

    lv_canvas_set_px(canvas, 0, 0, lv_color_white());
    lv_canvas_set_px(canvas, 4, 0, lv_color_white());

    if (level <= 10 || usb_present) {
        lv_canvas_draw_rect(canvas, 1, 2, 3, 5, &rect_fill_dsc);
    } else if (level <= 30) {
        lv_canvas_draw_rect(canvas, 1, 2, 3, 4, &rect_fill_dsc);
    } else if (level <= 50) {
        lv_canvas_draw_rect(canvas, 1, 2, 3, 3, &rect_fill_dsc);
    } else if (level <= 70) {
        lv_canvas_draw_rect(canvas, 1, 2, 3, 2, &rect_fill_dsc);
    } else if (level <= 90) {
        lv_canvas_draw_rect(canvas, 1, 2, 3, 1, &rect_fill_dsc);
    }
}

/* Update each widget's display by iterating over all battery slots */
static void update_widget_from_global_state(lv_obj_t *widget)
{
    for (int i = 0; i < TOTAL_SLOTS; i++) {
        /* Each battery slot is represented by two children:
        child index i*2: battery icon (canvas)
        child index i*2 + 1: battery percentage label */
        lv_obj_t *symbol = lv_obj_get_child(widget, i * 2);
        lv_obj_t *label = lv_obj_get_child(widget, i * 2 + 1);
        if (battery_state_valid[i]) {
            draw_battery(symbol, battery_states[i].level, battery_states[i].usb_present);
            lv_label_set_text_fmt(label, "%4u%%", battery_states[i].level);
            lv_obj_clear_flag(symbol, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(label, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(symbol, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(label, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

/* Callback that is called when battery events update the state.
Instead of using the passed state directly, we refresh the entire widget
from our global persistent battery_states array. */
void battery_status_update_cb(struct battery_state state)
{
    struct zmk_widget_dongle_battery_status *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) {
        update_widget_from_global_state(widget->obj);
    }
}

/* Process a peripheral battery event: update the corresponding slot.
Note: Use 'source' (not 'index') as defined in the event struct.
*/
static struct battery_state peripheral_battery_status_get_state(const zmk_event_t *eh)
{
    const struct zmk_peripheral_battery_state_changed *ev = as_zmk_peripheral_battery_state_changed(eh);
    if (ev) {
        uint8_t idx = ev->source + SOURCE_OFFSET;
        if (idx < TOTAL_SLOTS) {
            battery_states[idx].source = idx;
            battery_states[idx].level = ev->state_of_charge;
            battery_states[idx].usb_present = false;
            battery_state_valid[idx] = true;
            return battery_states[idx];
        }
    }
    return (struct battery_state){0};
}

/* Process a central battery event: update slot 0. */
static struct battery_state central_battery_status_get_state(const zmk_event_t *eh)
{
    const struct zmk_battery_state_changed *ev = as_zmk_battery_state_changed(eh);
    battery_states[0].source = 0;
    battery_states[0].level = (ev != NULL) ? ev->state_of_charge : zmk_battery_state_of_charge();
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
    battery_states[0].usb_present = zmk_usb_is_powered();
#endif /* IS_ENABLED(CONFIG_USB_DEVICE_STACK) */
    battery_state_valid[0] = true;
    return battery_states[0];
}

/* Depending on the event, update the corresponding slot. */
static struct battery_state battery_status_get_state(const zmk_event_t *eh)
{
    if (as_zmk_peripheral_battery_state_changed(eh) != NULL) {
        return peripheral_battery_status_get_state(eh);
    } else {
        return central_battery_status_get_state(eh);
    }
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_dongle_battery_status, struct battery_state,
                battery_status_update_cb, battery_status_get_state)

ZMK_SUBSCRIPTION(widget_dongle_battery_status, zmk_peripheral_battery_state_changed);
#if IS_ENABLED(CONFIG_ZMK_DONGLE_DISPLAY_DONGLE_BATTERY)
#if !IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
ZMK_SUBSCRIPTION(widget_dongle_battery_status, zmk_battery_state_changed);
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
ZMK_SUBSCRIPTION(widget_dongle_battery_status, zmk_usb_conn_state_changed);
#endif /* IS_ENABLED(CONFIG_USB_DEVICE_STACK) */
#endif /* !IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) */
#endif /* IS_ENABLED(CONFIG_ZMK_DONGLE_DISPLAY_DONGLE_BATTERY) */

int zmk_widget_dongle_battery_status_init(struct zmk_widget_dongle_battery_status *widget, lv_obj_t *parent)
{
    widget->obj = lv_obj_create(parent);
    lv_obj_set_size(widget->obj, LV_SIZE_CONTENT, LV_SIZE_CONTENT);

    /* Create display elements for each slot.
    Each slot gets two children: an image canvas and a label.
    Adjust the alignment as desired. */
    for (int i = 0; i < TOTAL_SLOTS; i++) {
        lv_obj_t *image_canvas = lv_canvas_create(widget->obj);
        lv_obj_t *battery_label = lv_label_create(widget->obj);

        lv_canvas_set_buffer(image_canvas, battery_image_buffer[i], 5, 8, LV_IMG_CF_TRUE_COLOR);

        /* Here we use a vertical offset (i * 10) so that each slot appears separately. */
        lv_obj_align(image_canvas, LV_ALIGN_TOP_RIGHT, 0, i * 10);
        lv_obj_align(battery_label, LV_ALIGN_TOP_RIGHT, -7, i * 10);

        lv_obj_add_flag(image_canvas, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(battery_label, LV_OBJ_FLAG_HIDDEN);
    }

    sys_slist_append(&widgets, &widget->node);

    /* Initialize the display listener */
    widget_dongle_battery_status_init();

    return 0;
}

lv_obj_t *zmk_widget_dongle_battery_status_obj(struct zmk_widget_dongle_battery_status *widget)
{
    return widget->obj;
}
