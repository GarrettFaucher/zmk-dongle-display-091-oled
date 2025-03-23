/*
* Copyright (c) 2024 The ZMK Contributors
*
* SPDX-License-Identifier: MIT
*/

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/sys/util.h>
#include <zephyr/sys/slist.h>
#include <lvgl.h>
#include <stdio.h>

#include <zmk/events/battery_state_changed.h>

#ifndef ZMK_SPLIT_BLE_PERIPHERAL_COUNT
#define ZMK_SPLIT_BLE_PERIPHERAL_COUNT 6
#endif

/* Persistent battery state for all peripherals */
struct battery_status_state {
    uint8_t level[ZMK_SPLIT_BLE_PERIPHERAL_COUNT];
};

static struct battery_status_state battery_state;

/* Array to track which peripheral slots are active */
static bool battery_active[ZMK_SPLIT_BLE_PERIPHERAL_COUNT] = { false };

/* For simplicity, we assume one set of LVGL label objects for battery display.
They are arranged horizontally, 35px apart. */
static lv_obj_t *battery_labels[ZMK_SPLIT_BLE_PERIPHERAL_COUNT];

/* Linked list of battery status widgets that need updating */
static sys_slist_t widgets = SYS_SLIST_STATIC_INIT(&widgets);

/* Update the battery icons/labels for all slots */
static void set_battery_symbols(lv_obj_t *container, const struct battery_status_state *state)
{
    for (int i = 0; i < ZMK_SPLIT_BLE_PERIPHERAL_COUNT; i++) {
        if (!battery_active[i]) {
            lv_label_set_text(battery_labels[i], "");
            continue;
        }
        char text[5];
        snprintf(text, sizeof(text), "%d%%", state->level[i]);
        lv_label_set_text(battery_labels[i], text);
    }
}

/* Event handler for peripheral battery events */
static int battery_status_event_handler(const zmk_event_t *eh)
{
    const struct zmk_peripheral_battery_state_changed *ev = as_zmk_peripheral_battery_state_changed(eh);
    if (ev) {
        uint8_t idx = ev->index;
        if (idx < ZMK_SPLIT_BLE_PERIPHERAL_COUNT) {
            battery_state.level[idx] = ev->level;
            battery_active[idx] = true;
        }

        struct zmk_widget_peripheral_battery_status *widget;
        SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) {
            set_battery_symbols(widget->obj, &battery_state);
        }
        return 0;
    }
    return -ENOTSUP;
}

/* Initialize the battery status widget */
int zmk_widget_peripheral_battery_status_init(struct zmk_widget_peripheral_battery_status *widget, lv_obj_t *parent)
{
    widget->obj = lv_obj_create(parent);
    lv_obj_set_size(widget->obj, LV_SIZE_CONTENT, LV_SIZE_CONTENT);

    /* Create labels for each battery slot arranged horizontally with 35px spacing */
    for (int i = 0; i < ZMK_SPLIT_BLE_PERIPHERAL_COUNT; i++) {
        battery_labels[i] = lv_label_create(widget->obj);
        lv_obj_align(battery_labels[i], LV_ALIGN_LEFT_MID, i * 35, 0);
        lv_label_set_text(battery_labels[i], "");
    }

    sys_slist_append(&widgets, &widget->node);
    return 0;
}

/* Cleanup the widget */
int zmk_widget_peripheral_battery_status_cleanup(struct zmk_widget_peripheral_battery_status *widget)
{
    sys_slist_find_and_remove(&widgets, &widget->node);
    lv_obj_del(widget->obj);
    return 0;
}

/* Register the event listener */
ZMK_LISTENER(widget_peripheral_battery_status, battery_status_event_handler);
ZMK_SUBSCRIPTION(widget_peripheral_battery_status, zmk_peripheral_battery_state_changed);
