/**
 * LVGL Configuration for ESP32-C6-LCD-1.47
 * ST7789 172x320 display in landscape mode (320x172)
 */

#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>

/*====================
   COLOR SETTINGS
 *====================*/
#define LV_COLOR_DEPTH 16
#define LV_COLOR_16_SWAP 1

/*====================
   MEMORY SETTINGS
 *====================*/
#define LV_MEM_CUSTOM 0
#define LV_MEM_SIZE (48U * 1024U)
#define LV_MEM_ADR 0
#define LV_MEM_POOL_INCLUDE <stdlib.h>
#define LV_MEM_POOL_ALLOC malloc
#define LV_MEM_POOL_FREE free

/*====================
   HAL SETTINGS
 *====================*/
#define LV_TICK_CUSTOM 1
#define LV_TICK_CUSTOM_INCLUDE "esp_timer.h"
#define LV_TICK_CUSTOM_SYS_TIME_EXPR ((esp_timer_get_time() / 1000ULL))

/*====================
   FEATURE SETTINGS
 *====================*/
#define LV_USE_LOG 0
#define LV_USE_ASSERT_NULL 1
#define LV_USE_ASSERT_MALLOC 1
#define LV_USE_ASSERT_STYLE 0
#define LV_USE_ASSERT_MEM_INTEGRITY 0
#define LV_USE_ASSERT_OBJ 0

#define LV_USE_PERF_MONITOR 0
#define LV_USE_MEM_MONITOR 0

/*====================
   FONT USAGE
 *====================*/
#define LV_FONT_MONTSERRAT_8 0
#define LV_FONT_MONTSERRAT_10 0
#define LV_FONT_MONTSERRAT_12 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_18 0
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_22 0
#define LV_FONT_MONTSERRAT_24 1
#define LV_FONT_MONTSERRAT_26 0
#define LV_FONT_MONTSERRAT_28 0
#define LV_FONT_MONTSERRAT_30 0
#define LV_FONT_MONTSERRAT_32 1
#define LV_FONT_MONTSERRAT_34 0
#define LV_FONT_MONTSERRAT_36 0
#define LV_FONT_MONTSERRAT_38 0
#define LV_FONT_MONTSERRAT_40 0
#define LV_FONT_MONTSERRAT_42 0
#define LV_FONT_MONTSERRAT_44 0
#define LV_FONT_MONTSERRAT_46 0
#define LV_FONT_MONTSERRAT_48 0

#define LV_FONT_DEFAULT &lv_font_montserrat_14

#define LV_FONT_FMT_TXT_LARGE 0
#define LV_USE_FONT_COMPRESSED 0
#define LV_USE_FONT_SUBPX 0
#define LV_FONT_SUBPX_BGR 0

/*====================
   WIDGET USAGE
 *====================*/
#define LV_USE_ARC 1
#define LV_USE_BAR 1
#define LV_USE_BTN 1
#define LV_USE_BTNMATRIX 0
#define LV_USE_CANVAS 0
#define LV_USE_CHECKBOX 0
#define LV_USE_DROPDOWN 0
#define LV_USE_IMG 1
#define LV_USE_LABEL 1
#define LV_USE_LINE 1
#define LV_USE_ROLLER 0
#define LV_USE_SLIDER 0
#define LV_USE_SWITCH 0
#define LV_USE_TEXTAREA 0
#define LV_USE_TABLE 0

/*====================
   EXTRA WIDGETS
 *====================*/
#define LV_USE_ANIMIMG 0
#define LV_USE_CALENDAR 0
#define LV_USE_CHART 0
#define LV_USE_COLORWHEEL 0
#define LV_USE_IMGBTN 0
#define LV_USE_KEYBOARD 0
#define LV_USE_LED 1
#define LV_USE_LIST 0
#define LV_USE_MENU 0
#define LV_USE_METER 0
#define LV_USE_MSGBOX 0
#define LV_USE_SPAN 0
#define LV_USE_SPINBOX 0
#define LV_USE_SPINNER 1
#define LV_USE_TABVIEW 0
#define LV_USE_TILEVIEW 0
#define LV_USE_WIN 0

/*====================
   THEMES
 *====================*/
#define LV_USE_THEME_DEFAULT 1
#define LV_THEME_DEFAULT_DARK 1
#define LV_THEME_DEFAULT_GROW 0
#define LV_THEME_DEFAULT_TRANSITION_TIME 80

/*====================
   LAYOUTS
 *====================*/
#define LV_USE_FLEX 1
#define LV_USE_GRID 0

/*====================
   ANIMATIONS
 *====================*/
#define LV_USE_ANIM 1
#define LV_ANIM_DEF_TIME 300

#endif /* LV_CONF_H */
