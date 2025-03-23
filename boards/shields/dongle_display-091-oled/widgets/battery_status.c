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

/* Increase the horizontal spacing so the labels do not overlap.
Adjust this value as needed.
*/
#define LABEL_SPACING 40

/* Global widget list */
static sys_slist_t widgets = SYS_SLIST_STATIC_INIT(&widgets);

/* 
* Timer to refresh the display more frequently in case one half is missing.
*/
static struct k_timer battery_poll_timer;
static bool battery_poll_timer_started = false;

/* This structure stores battery state for a given slot.
* The 'source' field is simply the assigned slot index.
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
* In this version we display only the battery percentage.
* Update each widget's display by iterating over all battery slots.
* Each slot is represented by a single LVGL label.
*/
static void update_widget_from_global_state(lv_obj_t *widget) {
    for (int i = 0; i < TOTAL_SLOTS; i++) {
        lv_obj_t *label = lv_obj_get_child(widget, i);
        if (battery_state_valid[i] && battery_states[i].level != 0) {
            lv_label_set_text_fmt(label, "%3u%%", battery_states[i].level);
            lv_obj_clear_flag(label, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(label, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

/* Callback invoked when battery events update the state.
* This refreshes every widget with the current global battery state.
*/
void battery_status_update_cb(struct battery_state state) {
    struct zmk_widget_dongle_battery_status *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) {
        update_widget_from_global_state(widget->obj);
    }
}

/*
* Timer handler: Called every second to force a refresh.
* This will help “scan” more quickly if both halves are not yet present.
*/
static void battery_poll_timer_handler(struct k_timer *timer) {
    struct zmk_widget_dongle_battery_status *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) {
        update_widget_from_global_state(widget->obj);
    }
}

/*
* Helper: Find a matching slot if an existing battery level is within BATTERY_THRESHOLD percent.
* Searches from SOURCE_OFFSET onward (to reserve slot 0 for central battery if needed).
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

/*
* Helper: Find an empty slot for a new battery reading.
* Searches from SOURCE_OFFSET onward.
*/
static int find_empty_slot(void) {
    for (int i = SOURCE_OFFSET; i < TOTAL_SLOTS; i++) {
        if (!battery_state_valid[i]) {
            return i;
        }
    }
    return -1; // No empty slot found.
}

/*
* Process a peripheral battery event.
* Instead of using the event's 'source' field directly, we use the reported battery level.
* If the reading is 0%, we clear that slot (do not display it).
*/
static struct battery_state peripheral_battery_status_get_state(const zmk_event_t *eh) {
    const struct zmk_peripheral_battery_state_changed *ev = as_zmk_peripheral_battery_state_changed(eh);
    if (ev) {
        if (ev->state_of_charge == 0) {
            uint8_t idx = ev->source + SOURCE_OFFSET;
            if (idx < TOTAL_SLOTS) {
                battery_state_valid[idx] = false;
            }
            return (struct battery_state){0};
        }
        int idx = find_matching_slot(ev->state_of_charge);
        if (idx == -1) {
            idx = find_empty_slot();
            if (idx == -1) {
                idx = SOURCE_OFFSET;  /* Fallback: update the first peripheral slot */
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

/*
* Process a central battery event: update slot 0.
* If the battery level is 0, clear the slot.
*/
static struct battery_state central_battery_status_get_state(const zmk_event_t *eh) {
    const struct zmk_battery_state_changed *ev = as_zmk_battery_state_changed(eh);
    if (ev && ev->state_of_charge == 0) {
        battery_state_valid[0] = false;
        return (struct battery_state){0};
    }
    battery_states[0].source = 0;
    battery_states[0].level = (ev != NULL) ? ev->state_of_charge : zmk_battery_state_of_charge();
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
    battery_states[0].usb_present = zmk_usb_is_powered();
#endif
    battery_state_valid[0] = (battery_states[0].level != 0);
    return battery_states[0];
}

/*
* Depending on the event, select the correct battery state getter.
*/
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

/*
* Initialize the battery status widget:
* - Create one LVGL label for each slot.
* - Arrange them horizontally from right to left, so slot 0 is at the top-right,
*   and subsequent batteries appear to its left.
* - Start a timer to poll (refresh) the display every second if both halves are not present.
*/
int zmk_widget_dongle_battery_status_init(struct zmk_widget_dongle_battery_status *widget, lv_obj_t *parent) {
    widget->obj = lv_obj_create(parent);
    lv_obj_set_size(widget->obj, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    
    for (int i = 0; i < TOTAL_SLOTS; i++) {
        lv_obj_t *battery_label = lv_label_create(widget->obj);
        lv_label_set_text(battery_label, ""); // Start empty

        /* Align labels horizontally: slot 0 is at the top-right,
        * and each subsequent slot is shifted left by LABEL_SPACING pixels.
        */
        lv_obj_align(battery_label, LV_ALIGN_TOP_RIGHT, -i * LABEL_SPACING, 0);
    }

    sys_slist_append(&widgets, &widget->node);

    /* Initialize the display listener */
    widget_dongle_battery_status_init();

    /* Start the polling timer if it hasn't been started yet.
    This timer forces a display refresh every second.
    */
    if (!battery_poll_timer_started) {
        k_timer_init(&battery_poll_timer, battery_poll_timer_handler, NULL);
        k_timer_start(&battery_poll_timer, K_SECONDS(1), K_SECONDS(1));
        battery_poll_timer_started = true;
    }

    return 0;
}

lv_obj_t *zmk_widget_dongle_battery_status_obj(struct zmk_widget_dongle_battery_status *widget) {
    return widget->obj;
}
