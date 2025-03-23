#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <sys/util.h>
#include <sys/slist.h>
#include <lvgl.h>
#include <stdio.h>

#include <zmk/events/peripheral_battery_state_changed.h>  // Event for peripheral battery updates
//#include <zmk/events/battery_state_changed.h> // (if needed for central battery, disabled in this case)

struct battery_status_state {
    uint8_t level[ZMK_SPLIT_BLE_PERIPHERAL_COUNT];
};

static struct battery_status_state battery_state;                   /** Persistent battery levels for all peripherals */
static bool battery_active[ZMK_SPLIT_BLE_PERIPHERAL_COUNT] = {0};    /** Flags for whether a given index has an active battery to show */

struct battery_widget_object {
    lv_obj_t *battery_label;
};

/* Create label objects for each potential peripheral slot */
static struct battery_widget_object battery_widget_objects[ZMK_SPLIT_BLE_PERIPHERAL_COUNT];

/* Linked list of widget instances (in case multiple displays use this widget) */
static sys_slist_t widgets = SYS_SLIST_STATIC_INIT(&widgets);

static void set_battery_symbol(lv_obj_t *container, const struct battery_status_state *state) {
    /* Update each battery label based on the stored state.
       Iterate from highest index down to 0 so that higher index batteries align left (earlier) on screen&#8203;:contentReference[oaicite:4]{index=4}. */
    for (int i = ZMK_SPLIT_BLE_PERIPHERAL_COUNT - 1; i >= 0; i--) {
        lv_obj_t *label = battery_widget_objects[i].battery_label;
        uint8_t level = state->level[i];
        if (!battery_active[i]) {
            // No active peripheral for this slot, ensure it's blank
            lv_label_set_text(label, "");
            continue;
        }

        #if IS_ENABLED(CONFIG_ZMK_WIDGET_BATTERY_STATUS_SHOW_PERCENTAGE)
        /* Display numeric percentage (e.g., "75%") */
        char text[5];
        snprintf(text, sizeof(text), "%d%%", level);
        lv_label_set_text(label, text);
        #else
        /* Display battery icon based on level */
        const char *icon;
        if (level >= 95) {
            icon = LV_SYMBOL_BATTERY_FULL;      // Full battery icon
        } else if (level >= 70) {
            icon = LV_SYMBOL_BATTERY_3;         // 3/4 battery icon
        } else if (level >= 45) {
            icon = LV_SYMBOL_BATTERY_2;         // 1/2 battery icon
        } else if (level >= 20) {
            icon = LV_SYMBOL_BATTERY_1;         // 1/4 battery icon
        } else {
            icon = LV_SYMBOL_BATTERY_EMPTY;     // Empty battery icon (<=20%)
        }
        lv_label_set_text(label, icon);
        #endif
    }
}

static int battery_status_event_handler(const zmk_event_t *eh) {
    /* Handle peripheral battery level change events */
    const struct zmk_peripheral_battery_state_changed *bat_ev = as_zmk_peripheral_battery_state_changed(eh);
    if (bat_ev) {
        uint8_t idx = bat_ev->index;
        if (idx < ZMK_SPLIT_BLE_PERIPHERAL_COUNT) {
            battery_state.level[idx] = bat_ev->level;
            battery_active[idx] = true;
        }
        /* Update all registered battery status widgets */
        struct zmk_widget_peripheral_battery_status *widget;
        SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) {
            set_battery_symbol(widget->obj, &battery_state);
        }
        return 0;  // event handled
    }

    /* (If central battery events were enabled, we would handle them here, 
        e.g., to possibly show the dongle's own battery if configured.) */
    return -ENOTSUP;
}

int zmk_widget_peripheral_battery_status_init(struct zmk_widget_peripheral_battery_status *widget, lv_obj_t *parent) {
    /* Create a container for the battery icons/labels */
    widget->obj = lv_obj_create(parent);
    lv_obj_set_size(widget->obj, LV_SIZE_CONTENT, LV_SIZE_CONTENT);

    /* Calculate initial offset so that the rightmost label (index 0) starts at x=0,
       and each additional label shifts 35px to the left from the previous&#8203;:contentReference[oaicite:5]{index=5}. */
    int initial_x_offset = (ZMK_SPLIT_BLE_PERIPHERAL_COUNT - 1) * 35;
    for (int i = ZMK_SPLIT_BLE_PERIPHERAL_COUNT - 1; i >= 0; i--) {
        /* Create a label for each peripheral battery slot */
        battery_widget_objects[i].battery_label = lv_label_create(widget->obj);
        lv_obj_align(battery_widget_objects[i].battery_label, LV_ALIGN_LEFT_MID, 
                     initial_x_offset - i * 35, 0);
        lv_label_set_text(battery_widget_objects[i].battery_label, "");  // start empty
    }

    /* Add this widget instance to the list for event updates */
    sys_slist_append(&widgets, &widget->node);
    return 0;
}

int zmk_widget_peripheral_battery_status_cleanup(struct zmk_widget_peripheral_battery_status *widget) {
    /* Remove the widget instance from the linked list */
    sys_slist_find_and_remove(&widgets, &widget->node);
    /* (Optionally delete LVGL objects if needed; if the entire screen is being torn down elsewhere, this may be handled there) */
    lv_obj_del(widget->obj);
    return 0;
}

/* Register the event listener (for peripheral battery events) */
ZMK_LISTENER(widget_peripheral_battery_status, battery_status_event_handler);
ZMK_SUBSCRIPTION(widget_peripheral_battery_status, zmk_peripheral_battery_state_changed);
