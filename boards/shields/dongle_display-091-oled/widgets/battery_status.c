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

/* 
* SOURCE_OFFSET reserves slot 0 for the central battery if needed.
* If using only peripherals, set this to 0.
*/
#if IS_ENABLED(CONFIG_ZMK_DONGLE_DISPLAY_DONGLE_BATTERY)
    #define SOURCE_OFFSET 1
#else
    #define SOURCE_OFFSET 0
#endif

/* TOTAL_SLOTS covers the central (if any) plus all peripherals */
#define TOTAL_SLOTS (ZMK_SPLIT_BLE_PERIPHERAL_COUNT + SOURCE_OFFSET)

/* Threshold (in percent) for matching battery levels */
#define BATTERY_THRESHOLD 5

/* Horizontal spacing (in pixels) between battery labels */
#define LABEL_SPACING 20

/* Global widget list */
static sys_slist_t widgets = SYS_SLIST_STATIC_INIT(&widgets);

/* This structure stores battery state for a given slot.
The 'source' field is now just the assigned slot index.
*/
struct battery_state {
    uint8_t source;
    uint8_t level;
    bool usb_present;
};

/* Global persistent state for each battery slot */
static struct battery_state battery_states[TOTAL_SLOTS];
static bool battery_state_valid[TOTAL_SLOTS] = { false };

/*
* Instead of drawing a battery icon, we only use a label to display
* the battery percentage.
*/

/* Update each widget's display by iterating over all battery slots.
Each slot is represented by a single LVGL label.
*/
static void update_widget_from_global_state(lv_obj_t *widget) {
    for (int i = 0; i < TOTAL_SLOTS; i++) {
        /* Get the label corresponding to this slot. */
        lv_obj_t *label = lv_obj_get_child(widget, i);
        if (battery_state_valid[i]) {
            lv_label_set_text_fmt(label, "%3u%%", battery_states[i].level);
            lv_obj_clear_flag(label, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(label, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

/* Callback invoked when battery events update the state.
This refreshes every widget with the current global battery state.
*/
void battery_status_update_cb(struct battery_state state) {
    struct zmk_widget_dongle_battery_status *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) {
        update_widget_from_global_state(widget->obj);
    }
}

/* 
* Helper: Find a matching slot if an existing battery level is within BATTERY_THRESHOLD.
* Searches from SOURCE_OFFSET onward (to reserve slot 0 for the central battery if needed).
*/
static int find_matching_slot(uint8_t new_level) {
    for (int i = SOURCE_OFFSET; i < TOTAL_SLOTS; i++) {
        if (battery_state_valid[i]) {
            int diff = (battery_states[i].level > new_level) ?
                        battery_states[i].level - new_level : new_level - battery_states[i].level;
            if (diff <= BATTERY_THRESHOLD) {
                return i;
            }
        }
    }
    return -1;
}

/* Helper: Find an empty slot for a new battery reading. */
static int find_empty_slot(void) {
    for (int i = SOURCE_OFFSET; i < TOTAL_SLOTS; i++) {
        if (!battery_state_valid[i]) {
            return i;
        }
    }
    return -1; // All slots in use.
}

/* Process a peripheral battery event:
Instead of using the event's 'source' field for the slot,
we match the battery level within a ±BATTERY_THRESHOLD range.
If no matching slot is found, we use an empty slot.
*/
static struct battery_state peripheral_battery_status_get_state(const zmk_event_t *eh) {
    const struct zmk_peripheral_battery_state_changed *ev = as_zmk_peripheral_battery_state_changed(eh);
    if (ev) {
        int idx = find_matching_slot(ev->state_of_charge);
        if (idx == -1) {
            idx = find_empty_slot();
            if (idx == -1) {
                /* Fallback: update the first peripheral slot */
                idx = SOURCE_OFFSET;
            }
        }
        battery_states[idx].source = idx;
        battery_states[idx].level = ev->state_of_charge;
        battery_states[idx].usb_present = false;
        battery_state_valid[idx] = true;
        return battery_states[idx];
    }
    return (struct battery_state){0};
}

/* Process a central battery event: update slot 0. */
static struct battery_state central_battery_status_get_state(const zmk_event_t *eh) {
    const struct zmk_battery_state_changed *ev = as_zmk_battery_state_changed(eh);
    battery_states[0].source = 0;
    battery_states[0].level = (ev != NULL) ? ev->state_of_charge : zmk_battery_state_of_charge();
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
    battery_states[0].usb_present = zmk_usb_is_powered();
#endif
    battery_state_valid[0] = true;
    return battery_states[0];
}

/* Depending on the event, select the correct battery state getter */
static struct battery_state battery_status_get_state(const zmk_event_t *eh) { 
    if (as_zmk_peripheral_battery_state_changed(eh) != NULL) {
        return peripheral_battery_status_get_state(eh);
    } else {
        return central_battery_status_get_state(eh);
    }
}

/*
* Register the widget listener and subscriptions.
* These macros hook into the ZMK event system.
*/
ZMK_DISPLAY_WIDGET_LISTENER(widget_dongle_battery_status, struct battery_state,
                            battery_status_update_cb, battery_status_get_state)

ZMK_SUBSCRIPTION(widget_dongle_battery_status, zmk_peripheral_battery_state_changed);
#if IS_ENABLED(CONFIG_ZMK_DONGLE_DISPLAY_DONGLE_BATTERY)
#if !IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
ZMK_SUBSCRIPTION(widget_dongle_battery_status, zmk_battery_state_changed);
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
ZMK_SUBSCRIPTION(widget_dongle_battery_status, zmk_usb_conn_state_changed);
#endif
#endif
#endif

/* Initialize the battery status widget:
- Create one LVGL label for each slot.
- Arrange them left-to-right such that the first battery (slot 0) is anchored at the top-right,
    and subsequent batteries appear to its left.
*/
int zmk_widget_dongle_battery_status_init(struct zmk_widget_dongle_battery_status *widget, lv_obj_t *parent) {
    widget->obj = lv_obj_create(parent);
    lv_obj_set_size(widget->obj, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    
    for (int i = 0; i < TOTAL_SLOTS; i++) {
        /* Create one label per slot */
        lv_obj_t *battery_label = lv_label_create(widget->obj);
        lv_label_set_text(battery_label, ""); // Start empty

        /* 
        * Arrange the labels horizontally.
        * Slot 0 is anchored to the top-right, and each subsequent slot is offset left.
        */
        lv_obj_align(battery_label, LV_ALIGN_TOP_RIGHT, -i * LABEL_SPACING, 0);
    }

    sys_slist_append(&widgets, &widget->node);

    /* Initialize the display listener */
    widget_dongle_battery_status_init();

    return 0;
}

lv_obj_t *zmk_widget_dongle_battery_status_obj(struct zmk_widget_dongle_battery_status *widget) {
    return widget->obj;
}
