/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/services/bas.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/display.h>
#include <zmk/display/widgets/battery_status.h>
#include <zmk/usb.h>
#include <zmk/ble.h>
#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/event_manager.h>
#include <zmk/events/battery_state_changed.h>

#include "battery_status.h"

static sys_slist_t widgets = SYS_SLIST_STATIC_INIT(&widgets);

struct battery_status_state {
    uint8_t level[ZMK_SPLIT_BLE_PERIPHERAL_COUNT];
} battery_state;

struct battery_widget_object {
    lv_obj_t *battery_label;
};

struct battery_widget_object battery_widget_objects[ZMK_SPLIT_BLE_PERIPHERAL_COUNT];

static void set_battery_symbol(lv_obj_t *widget, struct battery_status_state state) {
    for (int i = ZMK_SPLIT_BLE_PERIPHERAL_COUNT - 1; i >= 0; i--) {  // Iterate backwards
        uint8_t level = state.level[i];
        lv_obj_t *label = battery_widget_objects[i].battery_label;
        
        char text[5] = {};

        if (level > 0) {
            snprintf(text, sizeof(text), "%3u%%", level);
        }

        lv_label_set_text(label, text);
    }
}

void battery_status_update_cb(struct battery_status_state state) {
    struct zmk_widget_battery_status *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) { set_battery_symbol(widget->obj, state); }
}

static struct battery_status_state battery_status_get_state(const zmk_event_t *eh) {
    const struct zmk_peripheral_battery_state_changed *ev = as_zmk_peripheral_battery_state_changed(eh);
    battery_state.level[ev->source] = ev->state_of_charge;
    return battery_state;
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_battery_status, struct battery_status_state,
                            battery_status_update_cb, battery_status_get_state)

ZMK_SUBSCRIPTION(widget_battery_status, zmk_peripheral_battery_state_changed);

int zmk_widget_peripheral_battery_status_init(struct zmk_widget_peripheral_battery_status *widget, lv_obj_t *parent) {
    widget->obj = lv_obj_create(parent);

    lv_obj_set_size(widget->obj, LV_SIZE_CONTENT, LV_SIZE_CONTENT);

    // Calculate initial x-offset for the rightmost label
    int initial_x_offset = (ZMK_SPLIT_BLE_PERIPHERAL_COUNT - 1) * 35;

    for (int i = ZMK_SPLIT_BLE_PERIPHERAL_COUNT - 1; i >= 0; i--) {  // Iterate backwards
        battery_widget_objects[i].battery_label = lv_label_create(widget->obj);

        lv_obj_align(battery_widget_objects[i].battery_label, LV_ALIGN_LEFT_MID, initial_x_offset - i * 35, 0);
    }

    sys_slist_append(&widgets, &widget->node);

    widget_battery_status_init();
    return 0;
}

lv_obj_t *zmk_widget_peripheral_battery_status_obj(struct zmk_widget_peripheral_battery_status *widget) {
    return widget->obj;
}
