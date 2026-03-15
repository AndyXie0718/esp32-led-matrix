#ifndef PANEL_CONFIG_H
#define PANEL_CONFIG_H

#define PANEL_WIDTH 16
#define PANEL_HEIGHT 16
#define PANEL_LED_COUNT (PANEL_WIDTH * PANEL_HEIGHT)

// Column-major straight wiring: each column advances in +y order.
#define PANEL_COLUMN_MAJOR 1
#define PANEL_SERPENTINE 0  // 1: odd rows reversed, 0: normal row-major.


// Unified max value for LED channels used by all simulators.
#define PANEL_LED_VALUE_MAX 20

static inline int panel_led_index(int x, int y) {
#if PANEL_COLUMN_MAJOR
    return x * PANEL_HEIGHT + y;
#endif

#if PANEL_SERPENTINE
    if (y & 1) 
        return y * PANEL_WIDTH + (PANEL_WIDTH - 1 - x);
#endif

    return y * PANEL_WIDTH + x;
}

#endif
