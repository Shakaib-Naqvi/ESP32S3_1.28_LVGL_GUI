
/*Using LVGL with Arduino requires some extra steps:
 *Be sure to read the docs here: https://docs.lvgl.io/master/get-started/platforms/arduino.html  */

#include <lvgl.h>
#include <TFT_eSPI.h>
#include "lv_conf.h"
#include <demos/lv_demos.h>
#include <WiFi.h>
#include "time.h"
#include <string.h>
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"

#include "CST816S.h"
#include "ui.h"
#include "wifi_connect.h"

/*To use the built-in examples and demos of LVGL uncomment the includes below respectively.
 *You also need to copy `lvgl/examples` to `lvgl/src/examples`. Similarly for the demos `lvgl/demos` to `lvgl/src/demos`.
 Note that the `lv_examples` library is for LVGL v7 and you shouldn't install it for this version (since LVGL v8)
 as the examples and demos are now part of the main LVGL library. */

#define EXAMPLE_LVGL_TICK_PERIOD_MS    2
#define UI_LOOP_DELAY_MS                1
#define UI_PERF_UPDATE_MS               1000
#define LVGL_DRAW_BUFFER_LINES          60
#define TIME_UPDATE_MS                  1000
#define BATTERY_UPDATE_MS               5000
#define BATTERY_ADC_SAMPLES             8
#define BAT_ADC_PIN                     1
#define BAT_ADC_UNIT                    ADC_UNIT_1
#define BAT_ADC_CHANNEL                 ADC_CHANNEL_0
#define BAT_ADC_ATTEN                   ADC_ATTEN_DB_12
#define BAT_ADC_DIVIDER_RATIO           3.0f
#define BAT_ADC_FALLBACK_REF_MV         3300.0f
#define BAT_ADC_RAW_STEPS               4096.0f
#define BATTERY_PRESENT_MIN_VOLTAGE     2.50f
#define BATTERY_MIN_VOLTAGE             3.20f
#define BATTERY_MAX_VOLTAGE             4.20f
#define BACKLIGHT_PWM_FREQ              5000
#define BACKLIGHT_PWM_RESOLUTION        8
#define BACKLIGHT_MIN_PERCENT           5
#define BACKLIGHT_DEFAULT_PERCENT       50

#ifndef TFT_BACKLIGHT_ON
#define TFT_BACKLIGHT_ON                HIGH
#endif

/*Change to your screen resolution*/
static const uint16_t screenWidth  = 240;
static const uint16_t screenHeight = 240;

static lv_disp_draw_buf_t draw_buf;

static lv_color_t buf1[screenWidth * LVGL_DRAW_BUFFER_LINES];
static lv_color_t buf2[screenWidth * LVGL_DRAW_BUFFER_LINES];

TFT_eSPI tft = TFT_eSPI(screenWidth, screenHeight); /* TFT instance */
CST816S touch(6, 7, 13, 5);	// sda, scl, rst, irq
static float batteryVoltage = 0.0f;
static uint8_t batteryPercent = 0;
static bool batteryPresent = false;
static adc_oneshot_unit_handle_t batteryAdcHandle = NULL;
static adc_cali_handle_t batteryAdcCaliHandle = NULL;
static bool batteryAdcReady = false;
static bool batteryAdcCalibrated = false;
static bool backlightPwmReady = false;
static uint8_t screenBrightnessPercent = BACKLIGHT_DEFAULT_PERCENT;

static uint8_t clampBrightnessPercent(int32_t percent)
{
    if (percent < BACKLIGHT_MIN_PERCENT) return BACKLIGHT_MIN_PERCENT;
    if (percent > 100) return 100;
    return (uint8_t)percent;
}

static uint32_t brightnessPercentToDuty(uint8_t percent)
{
    const uint32_t maxDuty = (1UL << BACKLIGHT_PWM_RESOLUTION) - 1;
    uint32_t duty = (maxDuty * percent) / 100;

#if TFT_BACKLIGHT_ON == LOW
    duty = maxDuty - duty;
#endif

    return duty;
}

static void updateBrightnessLabel()
{
    if (ui_brightnessvalue == NULL) return;

    char text[8];
    snprintf(text, sizeof(text), "%u%%", screenBrightnessPercent);
    lv_label_set_text(ui_brightnessvalue, text);
}

static void setScreenBrightness(uint8_t percent, bool updateSlider)
{
    screenBrightnessPercent = clampBrightnessPercent(percent);

    if (backlightPwmReady) {
        ledcWrite(TFT_BL, brightnessPercentToDuty(screenBrightnessPercent));
    }

    if (updateSlider && ui_BrightnessSlider != NULL) {
        lv_slider_set_value(ui_BrightnessSlider, screenBrightnessPercent, LV_ANIM_OFF);
    }
    updateBrightnessLabel();
}

static void brightnessSliderEvent(lv_event_t * e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;

    lv_obj_t * slider = lv_event_get_target(e);
    setScreenBrightness((uint8_t)lv_slider_get_value(slider), false);
}

static void setupBacklightPwm()
{
    pinMode(TFT_BL, OUTPUT);
    backlightPwmReady = ledcAttach(TFT_BL, BACKLIGHT_PWM_FREQ, BACKLIGHT_PWM_RESOLUTION);
    if (!backlightPwmReady) {
        Serial.println("Backlight PWM attach failed.");
    }
    setScreenBrightness(screenBrightnessPercent, false);
}

static void setupBrightnessControls()
{
    if (ui_BrightnessSlider == NULL) return;

    lv_slider_set_range(ui_BrightnessSlider, BACKLIGHT_MIN_PERCENT, 100);
    lv_obj_add_event_cb(ui_BrightnessSlider, brightnessSliderEvent, LV_EVENT_VALUE_CHANGED, NULL);
    setScreenBrightness(screenBrightnessPercent, true);
}

void updateWatchTime()
{
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo, 5)) {
        return;
    }

    char hour[3];
    char minute[3];
    char weekday[10];
    char day[4];
    char month[4];
    char year[5];

    strftime(hour, sizeof(hour), "%H", &timeinfo);
    strftime(minute, sizeof(minute), "%M", &timeinfo);
    strftime(weekday, sizeof(weekday), "%A", &timeinfo);
    strftime(day, sizeof(day), "%d", &timeinfo);
    strftime(month, sizeof(month), "%b", &timeinfo);
    strftime(year, sizeof(year), "%Y", &timeinfo);

    if (ui_Label2 != NULL) lv_label_set_text(ui_Label2, hour);
    if (ui_Label1 != NULL) lv_label_set_text(ui_Label1, minute);
    if (ui_Label6 != NULL) lv_label_set_text(ui_Label6, weekday);
    if (ui_Label5 != NULL) lv_label_set_text(ui_Label5, day);
    if (ui_Label3 != NULL) lv_label_set_text(ui_Label3, month);
    if (ui_Label7 != NULL) lv_label_set_text(ui_Label7, year);
}

static uint8_t voltageToBatteryPercent(float voltage)
{
    if (voltage <= BATTERY_MIN_VOLTAGE) return 0;
    if (voltage >= BATTERY_MAX_VOLTAGE) return 100;

    return (uint8_t)(((voltage - BATTERY_MIN_VOLTAGE) * 100.0f) /
                     (BATTERY_MAX_VOLTAGE - BATTERY_MIN_VOLTAGE));
}

static void setupBatteryAdc()
{
    adc_oneshot_unit_init_cfg_t unitConfig = {
        .unit_id = BAT_ADC_UNIT,
        .clk_src = ADC_RTC_CLK_SRC_DEFAULT,
        .ulp_mode = ADC_ULP_MODE_DISABLE
    };

    esp_err_t result = adc_oneshot_new_unit(&unitConfig, &batteryAdcHandle);
    if (result != ESP_OK) {
        Serial.print("BAT ADC unit init failed: ");
        Serial.println(esp_err_to_name(result));
        batteryAdcReady = false;
        return;
    }

    adc_oneshot_chan_cfg_t channelConfig = {
        .atten = BAT_ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_12
    };

    result = adc_oneshot_config_channel(batteryAdcHandle, BAT_ADC_CHANNEL, &channelConfig);
    if (result != ESP_OK) {
        Serial.print("BAT ADC channel config failed: ");
        Serial.println(esp_err_to_name(result));
        batteryAdcReady = false;
        return;
    }

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t caliConfig = {
        .unit_id = BAT_ADC_UNIT,
        .chan = BAT_ADC_CHANNEL,
        .atten = BAT_ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_12
    };
    result = adc_cali_create_scheme_curve_fitting(&caliConfig, &batteryAdcCaliHandle);
    batteryAdcCalibrated = result == ESP_OK;
#else
    batteryAdcCalibrated = false;
#endif

    batteryAdcReady = true;
}

static float readBatteryVoltage()
{
    if (!batteryAdcReady || batteryAdcHandle == NULL) {
        return 0.0f;
    }

    uint32_t millivolts = 0;
    uint8_t validSamples = 0;

    for (uint8_t i = 0; i < BATTERY_ADC_SAMPLES; i++) {
        int adcMillivolts = 0;
        esp_err_t result = ESP_FAIL;

        if (batteryAdcCalibrated && batteryAdcCaliHandle != NULL) {
            result = adc_oneshot_get_calibrated_result(batteryAdcHandle, batteryAdcCaliHandle, BAT_ADC_CHANNEL,
                                                       &adcMillivolts);
        } else {
            int rawValue = 0;
            result = adc_oneshot_read(batteryAdcHandle, BAT_ADC_CHANNEL, &rawValue);
            adcMillivolts = (int)((rawValue * BAT_ADC_FALLBACK_REF_MV) / BAT_ADC_RAW_STEPS);
        }

        if (result == ESP_OK) {
            millivolts += adcMillivolts;
            validSamples++;
        }
    }

    if (validSamples == 0) return 0.0f;

    float adcVoltage = (millivolts / (float)validSamples) / 1000.0f;
    return adcVoltage * BAT_ADC_DIVIDER_RATIO;
}

static void updateBatteryReading()
{
    batteryVoltage = readBatteryVoltage();
    batteryPresent = batteryVoltage >= BATTERY_PRESENT_MIN_VOLTAGE;
    batteryPercent = batteryPresent ? voltageToBatteryPercent(batteryVoltage) : 0;
}

static void updateBatteryUi()
{
    updateBatteryReading();

    if (ui_Arc1 != NULL) {
        lv_arc_set_value(ui_Arc1, batteryPercent);
        lv_color_t arcColor = lv_color_hex(0x4EF5B1);
        if (batteryPercent <= 20) {
            arcColor = lv_color_hex(0xFF3B30);
        } else if (batteryPercent <= 45) {
            arcColor = lv_color_hex(0xFFD60A);
        }
        lv_obj_set_style_arc_color(ui_Arc1, arcColor, LV_PART_INDICATOR);
    }

    if (ui_Label8 != NULL) {
        char batteryText[18];
        if (batteryPresent) {
            lv_snprintf(batteryText, sizeof(batteryText), "%u%% %.2fV", batteryPercent, batteryVoltage);
        } else {
            lv_snprintf(batteryText, sizeof(batteryText), "No Bat");
        }
        lv_label_set_text(ui_Label8, batteryText);
    }
}

static const uint8_t WIFI_SCAN_MAX_RESULTS = 20;
static String scannedWifiSsids[WIFI_SCAN_MAX_RESULTS];
static int32_t scannedWifiRssi[WIFI_SCAN_MAX_RESULTS];
static wifi_auth_mode_t scannedWifiAuth[WIFI_SCAN_MAX_RESULTS];
static uint8_t scannedWifiCount = 0;
static String selectedWifiSsid;
static lv_obj_t * wifiResultsScreen = NULL;
static lv_obj_t * wifiResultsStatus = NULL;
static lv_obj_t * wifiResultsList = NULL;
static lv_obj_t * wifiPasswordOverlay = NULL;
static lv_obj_t * wifiPasswordTextArea = NULL;
static lv_obj_t * wifiPasswordKeyboard = NULL;

static void closeWifiPasswordOverlay();

static lv_obj_t * game2048Screen = NULL;
static lv_obj_t * game2048BoardObj = NULL;
static lv_obj_t * game2048ScoreLabel = NULL;
static lv_obj_t * game2048StatusLabel = NULL;
static lv_obj_t * game2048Tiles[4][4];
static uint16_t game2048Board[4][4];
static uint32_t game2048Score = 0;

static lv_obj_t * uiPerfLabel = NULL;
static uint32_t uiFlushCount = 0;
static uint32_t uiLastPerfMillis = 0;
static uint32_t uiLastFlushCount = 0;

static void themeRoundBackground(lv_obj_t * obj)
{
    if (obj == NULL) return;

    lv_obj_set_style_bg_color(obj, lv_color_hex(0x010409), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(obj, lv_color_hex(0x14283A), LV_PART_MAIN);
    lv_obj_set_style_bg_main_stop(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_grad_stop(obj, 240, LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(obj, LV_GRAD_DIR_VER, LV_PART_MAIN);
    lv_obj_set_style_border_width(obj, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(obj, lv_color_hex(0x235168), LV_PART_MAIN);
    lv_obj_set_style_border_opa(obj, 150, LV_PART_MAIN);
}

static void themeGlassButton(lv_obj_t * obj)
{
    if (obj == NULL) return;

    lv_obj_set_style_radius(obj, 12, LV_PART_MAIN);
    lv_obj_set_style_bg_color(obj, lv_color_hex(0x1D2B36), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(obj, lv_color_hex(0x405867), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(obj, LV_GRAD_DIR_VER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(obj, 238, LV_PART_MAIN);
    lv_obj_set_style_border_color(obj, lv_color_hex(0x6DE6FF), LV_PART_MAIN);
    lv_obj_set_style_border_opa(obj, 120, LV_PART_MAIN);
    lv_obj_set_style_border_width(obj, 1, LV_PART_MAIN);
    lv_obj_set_style_bg_color(obj, lv_color_hex(0x16212B), LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_bg_grad_color(obj, lv_color_hex(0x2C3B47), LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(obj, 255, LV_PART_MAIN | LV_STATE_PRESSED);
}

static void themeTileIcon(lv_obj_t * obj)
{
    if (obj == NULL) return;

    lv_obj_set_style_radius(obj, 11, LV_PART_MAIN);
    lv_obj_set_style_bg_color(obj, lv_color_hex(0x243743), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(obj, lv_color_hex(0x516B78), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(obj, LV_GRAD_DIR_VER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(obj, 168, LV_PART_MAIN);
    lv_obj_set_style_border_color(obj, lv_color_hex(0x8BEAFF), LV_PART_MAIN);
    lv_obj_set_style_border_opa(obj, 145, LV_PART_MAIN);
    lv_obj_set_style_border_width(obj, 1, LV_PART_MAIN);
    lv_obj_set_style_img_recolor(obj, lv_color_hex(0xE8F8FF), LV_PART_MAIN);
    lv_obj_set_style_img_recolor_opa(obj, 125, LV_PART_MAIN);
}

static void themePlainIcon(lv_obj_t * obj)
{
    if (obj == NULL) return;

    lv_obj_set_style_img_recolor(obj, lv_color_hex(0xE8F8FF), LV_PART_MAIN);
    lv_obj_set_style_img_recolor_opa(obj, 105, LV_PART_MAIN);
}

static void themePrimaryText(lv_obj_t * obj)
{
    if (obj == NULL) return;

    lv_obj_set_style_text_color(obj, lv_color_hex(0xF7FCFF), LV_PART_MAIN);
}

static void themeSecondaryText(lv_obj_t * obj)
{
    if (obj == NULL) return;

    lv_obj_set_style_text_color(obj, lv_color_hex(0xB8D8E7), LV_PART_MAIN);
}

static void themeObjectList(lv_obj_t * const objects[], void (*themeFn)(lv_obj_t *))
{
    for (uint8_t i = 0; objects[i] != NULL; i++) {
        themeFn(objects[i]);
    }
}

static void applyWatchTheme()
{
    lv_obj_t * const roundBackgrounds[] = {
        ui_Container5, ui_Container4, ui_Container6, ui_Container10, ui_Container9, ui_Container12,
        ui_Container16, ui_Container17, ui_Container20, ui_Container21, ui_Container13, NULL
    };
    themeObjectList(roundBackgrounds, themeRoundBackground);

    lv_obj_t * const glassButtons[] = {
        ui_Container11, ui_Container15, ui_Container19, ui_settings1, ui_alarm, ui_timer, ui_menu6,
        ui_wifi5, ui_home5, ui_menu2, ui_menu3, ui_bluetooth, ui_games, ui_menu4, ui_menu5,
        ui_Container14, ui_WifiOption1, ui_WifiOption2, ui_WifiOption3, ui_WifiOption4,
        ui_home4, ui_home1, ui_home3, NULL
    };
    themeObjectList(glassButtons, themeGlassButton);

    lv_obj_t * const tileIcons[] = {
        ui_brightness, ui_about, ui_about1, ui_games1, ui_brightness2, ui_About2, NULL
    };
    themeObjectList(tileIcons, themeTileIcon);

    lv_obj_t * const plainIcons[] = {
        ui_Image1, ui_Image24, ui_Image3, ui_Image9, ui_Image17, ui_Image22, ui_Image6,
        ui_Image4, ui_Image2, ui_Image7, ui_Image8, NULL
    };
    themeObjectList(plainIcons, themePlainIcon);

    lv_obj_t * const primaryLabels[] = {
        ui_Label1, ui_Label2, ui_Label4, ui_Label6, ui_Label8, ui_WifiStatus,
        ui_WifiOptionLabel1, ui_WifiOptionLabel2, ui_WifiOptionLabel3, ui_WifiOptionLabel4,
        ui_brightnessvalue, ui_Label13, NULL
    };
    themeObjectList(primaryLabels, themePrimaryText);

    lv_obj_t * const secondaryLabels[] = {
        ui_Label3, ui_Label5, ui_Label7, ui_Label10, ui_Label11, ui_Label12, ui_Label14, NULL
    };
    themeObjectList(secondaryLabels, themeSecondaryText);

    if (ui_Arc1 != NULL) {
        lv_obj_set_style_arc_color(ui_Arc1, lv_color_hex(0x23414B), LV_PART_MAIN);
        lv_obj_set_style_arc_opa(ui_Arc1, 125, LV_PART_MAIN);
        lv_obj_set_style_arc_width(ui_Arc1, 4, LV_PART_MAIN);
        lv_obj_set_style_arc_color(ui_Arc1, lv_color_hex(0x4EF5B1), LV_PART_INDICATOR);
        lv_obj_set_style_arc_width(ui_Arc1, 4, LV_PART_INDICATOR);
    }

    if (ui_BrightnessSlider != NULL) {
        lv_obj_set_style_radius(ui_BrightnessSlider, 14, LV_PART_MAIN);
        lv_obj_set_style_bg_color(ui_BrightnessSlider, lv_color_hex(0x162631), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(ui_BrightnessSlider, 255, LV_PART_MAIN);
        lv_obj_set_style_bg_color(ui_BrightnessSlider, lv_color_hex(0x74DFFF), LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(ui_BrightnessSlider, 255, LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(ui_BrightnessSlider, lv_color_hex(0xF7FCFF), LV_PART_KNOB);
        lv_obj_set_style_bg_opa(ui_BrightnessSlider, 55, LV_PART_KNOB);
    }

    if (ui_Spinner1 != NULL) {
        lv_obj_set_style_arc_color(ui_Spinner1, lv_color_hex(0x23414B), LV_PART_MAIN);
        lv_obj_set_style_arc_color(ui_Spinner1, lv_color_hex(0x4EF5B1), LV_PART_INDICATOR);
    }
}

static lv_obj_t * wifiOptionObj(uint8_t index)
{
    switch (index) {
        case 0: return ui_WifiOption1;
        case 1: return ui_WifiOption2;
        case 2: return ui_WifiOption3;
        case 3: return ui_WifiOption4;
        default: return NULL;
    }
}

static lv_obj_t * wifiOptionLabel(uint8_t index)
{
    switch (index) {
        case 0: return ui_WifiOptionLabel1;
        case 1: return ui_WifiOptionLabel2;
        case 2: return ui_WifiOptionLabel3;
        case 3: return ui_WifiOptionLabel4;
        default: return NULL;
    }
}

static void setWifiStatus(const char * text)
{
    if (ui_WifiStatus != NULL) {
        lv_label_set_text(ui_WifiStatus, text);
    }
    if (wifiResultsStatus != NULL) {
        lv_label_set_text(wifiResultsStatus, text);
    }
}

static const char * wifiSecurityText(wifi_auth_mode_t authMode)
{
    return authMode == WIFI_AUTH_OPEN ? "Open" : "Locked";
}

static const char * wifiSignalText(int32_t rssi)
{
    if (rssi >= -55) return "Strong";
    if (rssi >= -70) return "Good";
    return "Weak";
}

static void clearWifiOptions()
{
    scannedWifiCount = 0;
    for (uint8_t i = 0; i < WIFI_SCAN_MAX_RESULTS; i++) {
        scannedWifiSsids[i] = "";
        scannedWifiRssi[i] = 0;
        scannedWifiAuth[i] = WIFI_AUTH_OPEN;

        if (i >= 4) continue;

        lv_obj_t * option = wifiOptionObj(i);
        lv_obj_t * label = wifiOptionLabel(i);
        if (option != NULL) lv_obj_add_flag(option, LV_OBJ_FLAG_HIDDEN);
        if (label != NULL) lv_label_set_text(label, "");
    }
}

static void setWifiOption(uint8_t index, const String & ssid, int32_t rssi, wifi_auth_mode_t authMode)
{
    scannedWifiSsids[index] = ssid;
    scannedWifiRssi[index] = rssi;
    scannedWifiAuth[index] = authMode;

    lv_obj_t * option = wifiOptionObj(index);
    lv_obj_t * label = wifiOptionLabel(index);
    if (option == NULL || label == NULL) return;

    String displayText = String(LV_SYMBOL_WIFI) + " " + ssid + "\n" +
                         wifiSignalText(rssi) + "  " + wifiSecurityText(authMode);
    lv_label_set_text(label, displayText.c_str());
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
    lv_obj_clear_flag(option, LV_OBJ_FLAG_HIDDEN);
}

static void styleWifiList()
{
    for (uint8_t i = 0; i < 4; i++) {
        lv_obj_t * option = wifiOptionObj(i);
        lv_obj_t * label = wifiOptionLabel(i);
        if (option == NULL || label == NULL) continue;

        lv_obj_set_size(option, 184, 32);
        lv_obj_set_x(option, 0);
        lv_obj_set_y(option, -18 + (i * 36));
        lv_obj_set_style_radius(option, 8, LV_PART_MAIN);
        lv_obj_set_style_pad_left(option, 10, LV_PART_MAIN);
        lv_obj_set_style_pad_right(option, 8, LV_PART_MAIN);
        lv_obj_set_style_bg_color(option, lv_color_hex(0x1A2A35), LV_PART_MAIN);
        lv_obj_set_style_bg_grad_color(option, lv_color_hex(0x38505E), LV_PART_MAIN);
        lv_obj_set_style_bg_grad_dir(option, LV_GRAD_DIR_HOR, LV_PART_MAIN);
        lv_obj_set_style_border_color(option, lv_color_hex(0x6DE6FF), LV_PART_MAIN);
        lv_obj_set_style_border_opa(option, 95, LV_PART_MAIN);
        lv_obj_set_style_border_width(option, 1, LV_PART_MAIN);

        lv_obj_set_width(label, 164);
        lv_obj_set_align(label, LV_ALIGN_CENTER);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_10, LV_PART_MAIN);
        lv_obj_set_style_text_line_space(label, 1, LV_PART_MAIN);
    }
}

static void closeWifiResultsScreen(lv_event_t * e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;

    lv_obj_t * oldScreen = wifiResultsScreen;
    wifiResultsScreen = NULL;
    wifiResultsStatus = NULL;
    wifiResultsList = NULL;
    closeWifiPasswordOverlay();

    lv_scr_load_anim(ui_Screen4, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 180, 0, false);
    if (oldScreen != NULL) {
        lv_obj_del_async(oldScreen);
    }
    setWifiStatus("Select network");
}

static void createWifiResultRow(uint8_t index)
{
    if (wifiResultsList == NULL || index >= scannedWifiCount) return;

    lv_obj_t * row = lv_btn_create(wifiResultsList);
    lv_obj_set_size(row, 188, 42);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(row, 9, LV_PART_MAIN);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x172733), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(row, lv_color_hex(0x344C5B), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(row, LV_GRAD_DIR_HOR, LV_PART_MAIN);
    lv_obj_set_style_border_color(row, lv_color_hex(0x6DE6FF), LV_PART_MAIN);
    lv_obj_set_style_border_opa(row, 95, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 1, LV_PART_MAIN);
    lv_obj_set_style_pad_left(row, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_right(row, 8, LV_PART_MAIN);
    lv_obj_add_event_cb(row, connect_wifi_network_ui, LV_EVENT_ALL, (void *)(uintptr_t)index);

    lv_obj_t * label = lv_label_create(row);
    lv_obj_set_width(label, 164);
    lv_obj_set_align(label, LV_ALIGN_CENTER);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_10, LV_PART_MAIN);
    lv_obj_set_style_text_line_space(label, 2, LV_PART_MAIN);

    String ssidText = scannedWifiSsids[index].length() > 0 ? scannedWifiSsids[index] : String("Hidden network");
    String displayText = String(LV_SYMBOL_WIFI) + " " + ssidText + "\n" +
                         wifiSignalText(scannedWifiRssi[index]) + "  " +
                         wifiSecurityText(scannedWifiAuth[index]) + "  " +
                         String(scannedWifiRssi[index]) + "dBm";
    lv_label_set_text(label, displayText.c_str());
}

static void showWifiResultsScreen()
{
    closeWifiPasswordOverlay();
    if (wifiResultsScreen != NULL) {
        lv_obj_del(wifiResultsScreen);
        wifiResultsScreen = NULL;
        wifiResultsStatus = NULL;
        wifiResultsList = NULL;
    }

    wifiResultsScreen = lv_obj_create(NULL);
    lv_obj_clear_flag(wifiResultsScreen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(wifiResultsScreen, lv_color_hex(0x010409), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(wifiResultsScreen, lv_color_hex(0x14283A), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(wifiResultsScreen, LV_GRAD_DIR_VER, LV_PART_MAIN);

    lv_obj_t * title = lv_label_create(wifiResultsScreen);
    lv_obj_set_width(title, 132);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 14);
    lv_label_set_text(title, "WiFi Networks");
    lv_obj_set_style_text_color(title, lv_color_hex(0xF4FAFF), LV_PART_MAIN);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, LV_PART_MAIN);

    wifiResultsStatus = lv_label_create(wifiResultsScreen);
    lv_obj_set_width(wifiResultsStatus, 140);
    lv_obj_align(wifiResultsStatus, LV_ALIGN_TOP_MID, 0, 35);
    lv_obj_set_style_text_color(wifiResultsStatus, lv_color_hex(0xB8D8E7), LV_PART_MAIN);
    lv_obj_set_style_text_align(wifiResultsStatus, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_font(wifiResultsStatus, &lv_font_montserrat_10, LV_PART_MAIN);

    lv_obj_t * closeBtn = lv_btn_create(wifiResultsScreen);
    lv_obj_set_size(closeBtn, 32, 32);
    lv_obj_align(closeBtn, LV_ALIGN_TOP_LEFT, 20, 13);
    lv_obj_set_style_radius(closeBtn, 16, LV_PART_MAIN);
    lv_obj_set_style_bg_color(closeBtn, lv_color_hex(0x1D2B36), LV_PART_MAIN);
    lv_obj_add_event_cb(closeBtn, closeWifiResultsScreen, LV_EVENT_ALL, NULL);

    lv_obj_t * closeLabel = lv_label_create(closeBtn);
    lv_label_set_text(closeLabel, LV_SYMBOL_CLOSE);
    lv_obj_center(closeLabel);

    wifiResultsList = lv_obj_create(wifiResultsScreen);
    lv_obj_remove_style_all(wifiResultsList);
    lv_obj_set_size(wifiResultsList, 202, 164);
    lv_obj_align(wifiResultsList, LV_ALIGN_BOTTOM_MID, 0, -16);
    lv_obj_set_flex_flow(wifiResultsList, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(wifiResultsList, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(wifiResultsList, 7, LV_PART_MAIN);
    lv_obj_set_style_pad_top(wifiResultsList, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(wifiResultsList, 20, LV_PART_MAIN);
    lv_obj_set_scroll_dir(wifiResultsList, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(wifiResultsList, LV_SCROLLBAR_MODE_AUTO);

    for (uint8_t i = 0; i < scannedWifiCount; i++) {
        createWifiResultRow(i);
    }

    String statusText = String(scannedWifiCount) + " found";
    lv_label_set_text(wifiResultsStatus, statusText.c_str());
    lv_scr_load_anim(wifiResultsScreen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 180, 0, false);
}

static void closeWifiPasswordOverlay()
{
    if (wifiPasswordOverlay != NULL) {
        lv_obj_del(wifiPasswordOverlay);
        wifiPasswordOverlay = NULL;
        wifiPasswordTextArea = NULL;
        wifiPasswordKeyboard = NULL;
    }
}

static void connectToSelectedWifi(const char * typedPassword)
{
    if (selectedWifiSsid.length() == 0) return;

    String wifiPassword = typedPassword != NULL ? String(typedPassword) : "";

    closeWifiPasswordOverlay();
    if (ui_Label10 != NULL) lv_label_set_text(ui_Label10, "Wait");
    setWifiStatus("Connecting...");
    lv_timer_handler();

    Serial.print("Connecting to ");
    Serial.print(selectedWifiSsid);
    Serial.print(" with password length: ");
    Serial.println(wifiPassword.length());

    WiFi.mode(WIFI_STA);
    WiFi.disconnect(true);
    delay(30);
    WiFi.begin(selectedWifiSsid.c_str(), wifiPassword.c_str());

    unsigned long startTime = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startTime < 15000) {
        lv_timer_handler();
        delay(30);
        Serial.print(".");
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("");
        Serial.println("WiFi connected.");
        setWifiStatus("Connected");
        if (ui_Label10 != NULL) lv_label_set_text(ui_Label10, "Scan");
        configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
        updateWatchTime();
    } else {
        Serial.println("");
        Serial.println("WiFi connect failed.");
        setWifiStatus("Connect failed");
        if (ui_Label10 != NULL) lv_label_set_text(ui_Label10, "Rescan");
    }
}

static void wifiPasswordKeyboardEvent(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY) {
        connectToSelectedWifi(lv_textarea_get_text(wifiPasswordTextArea));
    } else if (code == LV_EVENT_CANCEL) {
        closeWifiPasswordOverlay();
        setWifiStatus("Select network");
    }
}

static void wifiPasswordConnectEvent(lv_event_t * e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        connectToSelectedWifi(lv_textarea_get_text(wifiPasswordTextArea));
    }
}

static void wifiPasswordCloseEvent(lv_event_t * e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        closeWifiPasswordOverlay();
        setWifiStatus("Select network");
    }
}

static void showWifiPasswordOverlay()
{
    closeWifiPasswordOverlay();

    wifiPasswordOverlay = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(wifiPasswordOverlay);
    lv_obj_set_size(wifiPasswordOverlay, 240, 240);
    lv_obj_set_align(wifiPasswordOverlay, LV_ALIGN_CENTER);
    lv_obj_set_style_radius(wifiPasswordOverlay, 120, LV_PART_MAIN);
    lv_obj_set_style_bg_color(wifiPasswordOverlay, lv_color_hex(0x030A10), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(wifiPasswordOverlay, 250, LV_PART_MAIN);
    lv_obj_set_style_pad_top(wifiPasswordOverlay, 10, LV_PART_MAIN);

    lv_obj_t * closeBtn = lv_btn_create(wifiPasswordOverlay);
    lv_obj_set_size(closeBtn, 30, 30);
    lv_obj_align(closeBtn, LV_ALIGN_TOP_LEFT, 24, 12);
    lv_obj_add_event_cb(closeBtn, wifiPasswordCloseEvent, LV_EVENT_ALL, NULL);
    lv_obj_t * closeLabel = lv_label_create(closeBtn);
    lv_label_set_text(closeLabel, LV_SYMBOL_CLOSE);
    lv_obj_center(closeLabel);

    lv_obj_t * title = lv_label_create(wifiPasswordOverlay);
    lv_obj_set_width(title, 140);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 12);
    lv_label_set_text(title, selectedWifiSsid.c_str());
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(title, lv_color_hex(0xF4FAFF), LV_PART_MAIN);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_12, LV_PART_MAIN);

    wifiPasswordTextArea = lv_textarea_create(wifiPasswordOverlay);
    lv_obj_set_size(wifiPasswordTextArea, 142, 34);
    lv_obj_align(wifiPasswordTextArea, LV_ALIGN_TOP_MID, -18, 43);
    lv_textarea_set_one_line(wifiPasswordTextArea, true);
    lv_textarea_set_password_mode(wifiPasswordTextArea, true);
    lv_textarea_set_placeholder_text(wifiPasswordTextArea, "Password");
    lv_obj_set_style_text_font(wifiPasswordTextArea, &lv_font_montserrat_12, LV_PART_MAIN);

    lv_obj_t * connectBtn = lv_btn_create(wifiPasswordOverlay);
    lv_obj_set_size(connectBtn, 44, 34);
    lv_obj_align(connectBtn, LV_ALIGN_TOP_RIGHT, -28, 43);
    lv_obj_add_event_cb(connectBtn, wifiPasswordConnectEvent, LV_EVENT_ALL, NULL);
    lv_obj_t * connectLabel = lv_label_create(connectBtn);
    lv_label_set_text(connectLabel, LV_SYMBOL_OK);
    lv_obj_center(connectLabel);

    wifiPasswordKeyboard = lv_keyboard_create(wifiPasswordOverlay);
    lv_obj_set_size(wifiPasswordKeyboard, 204, 130);
    lv_obj_align(wifiPasswordKeyboard, LV_ALIGN_BOTTOM_MID, 0, -12);
    lv_keyboard_set_mode(wifiPasswordKeyboard, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_keyboard_set_textarea(wifiPasswordKeyboard, wifiPasswordTextArea);
    lv_obj_add_event_cb(wifiPasswordKeyboard, wifiPasswordKeyboardEvent, LV_EVENT_ALL, NULL);

    lv_obj_add_state(wifiPasswordTextArea, LV_STATE_FOCUSED);
    setWifiStatus("Enter password");
}

extern "C" void scan_wifi_networks_ui(lv_event_t * e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;

    clearWifiOptions();
    if (ui_Label10 != NULL) lv_label_set_text(ui_Label10, "Wait");
    setWifiStatus("Scanning...");
    lv_timer_handler();

    WiFi.mode(WIFI_STA);
    int networkCount = WiFi.scanNetworks(false, true);

    if (networkCount <= 0) {
        if (ui_Label10 != NULL) lv_label_set_text(ui_Label10, "Scan");
        setWifiStatus("No WiFi found");
        WiFi.scanDelete();
        showWifiResultsScreen();
        return;
    }

    int shownCount = networkCount < WIFI_SCAN_MAX_RESULTS ? networkCount : WIFI_SCAN_MAX_RESULTS;
    scannedWifiCount = shownCount;
    for (int i = 0; i < shownCount; i++) {
        scannedWifiSsids[i] = WiFi.SSID(i);
        scannedWifiRssi[i] = WiFi.RSSI(i);
        scannedWifiAuth[i] = WiFi.encryptionType(i);

        if (i < 4) {
            setWifiOption(i, WiFi.SSID(i), WiFi.RSSI(i), WiFi.encryptionType(i));
        }
    }

    if (ui_Label10 != NULL) lv_label_set_text(ui_Label10, "Rescan");
    setWifiStatus("Select network");
    WiFi.scanDelete();
    showWifiResultsScreen();
}

extern "C" void connect_wifi_network_ui(lv_event_t * e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;

    uint8_t index = (uintptr_t)lv_event_get_user_data(e);
    if (index >= scannedWifiCount || scannedWifiSsids[index].length() == 0) return;

    selectedWifiSsid = scannedWifiSsids[index];
    showWifiPasswordOverlay();
}

static lv_color_t game2048TileColor(uint16_t value)
{
    switch (value) {
        case 0: return lv_color_hex(0x172533);
        case 2: return lv_color_hex(0xEEE4DA);
        case 4: return lv_color_hex(0xEDE0C8);
        case 8: return lv_color_hex(0xF2B179);
        case 16: return lv_color_hex(0xF59563);
        case 32: return lv_color_hex(0xF67C5F);
        case 64: return lv_color_hex(0xF65E3B);
        case 128: return lv_color_hex(0xEDCF72);
        case 256: return lv_color_hex(0xEDCC61);
        case 512: return lv_color_hex(0xEDC850);
        case 1024: return lv_color_hex(0xEDC53F);
        default: return lv_color_hex(0xEDC22E);
    }
}

static bool game2048HasMoves()
{
    for (uint8_t r = 0; r < 4; r++) {
        for (uint8_t c = 0; c < 4; c++) {
            if (game2048Board[r][c] == 0) return true;
            if (c < 3 && game2048Board[r][c] == game2048Board[r][c + 1]) return true;
            if (r < 3 && game2048Board[r][c] == game2048Board[r + 1][c]) return true;
        }
    }
    return false;
}

static void game2048UpdateUi()
{
    char scoreText[24];
    lv_snprintf(scoreText, sizeof(scoreText), "Score %lu", (unsigned long)game2048Score);
    if (game2048ScoreLabel != NULL) lv_label_set_text(game2048ScoreLabel, scoreText);

    for (uint8_t r = 0; r < 4; r++) {
        for (uint8_t c = 0; c < 4; c++) {
            lv_obj_t * tile = game2048Tiles[r][c];
            if (tile == NULL) continue;

            uint16_t value = game2048Board[r][c];
            lv_obj_set_style_bg_color(tile, game2048TileColor(value), LV_PART_MAIN);
            lv_obj_set_style_text_color(tile, value <= 4 ? lv_color_hex(0x3C3530) : lv_color_hex(0xF4FAFF), LV_PART_MAIN);

            char valueText[8];
            if (value == 0) {
                valueText[0] = '\0';
            } else {
                lv_snprintf(valueText, sizeof(valueText), "%u", value);
            }
            lv_label_set_text(tile, valueText);
        }
    }

    if (game2048StatusLabel != NULL) {
        lv_label_set_text(game2048StatusLabel, game2048HasMoves() ? "Swipe" : "Game Over");
    }
}

static void game2048AddRandomTile()
{
    uint8_t emptyCells[16];
    uint8_t emptyCount = 0;

    for (uint8_t r = 0; r < 4; r++) {
        for (uint8_t c = 0; c < 4; c++) {
            if (game2048Board[r][c] == 0) {
                emptyCells[emptyCount++] = (r * 4) + c;
            }
        }
    }

    if (emptyCount == 0) return;

    uint8_t picked = emptyCells[random(emptyCount)];
    game2048Board[picked / 4][picked % 4] = random(10) == 0 ? 4 : 2;
}

static bool game2048SlideLine(uint16_t line[4])
{
    uint16_t original[4];
    uint16_t compacted[4] = {0, 0, 0, 0};
    uint16_t merged[4] = {0, 0, 0, 0};
    uint8_t compactedCount = 0;
    uint8_t mergedCount = 0;

    for (uint8_t i = 0; i < 4; i++) {
        original[i] = line[i];
        if (line[i] != 0) compacted[compactedCount++] = line[i];
    }

    for (uint8_t i = 0; i < compactedCount; i++) {
        if (i + 1 < compactedCount && compacted[i] == compacted[i + 1]) {
            merged[mergedCount] = compacted[i] * 2;
            game2048Score += merged[mergedCount];
            mergedCount++;
            i++;
        } else {
            merged[mergedCount++] = compacted[i];
        }
    }

    bool changed = false;
    for (uint8_t i = 0; i < 4; i++) {
        line[i] = merged[i];
        if (line[i] != original[i]) changed = true;
    }

    return changed;
}

static bool game2048Move(lv_dir_t direction)
{
    bool changed = false;
    uint16_t line[4];

    for (uint8_t i = 0; i < 4; i++) {
        for (uint8_t j = 0; j < 4; j++) {
            if (direction == LV_DIR_LEFT) line[j] = game2048Board[i][j];
            else if (direction == LV_DIR_RIGHT) line[j] = game2048Board[i][3 - j];
            else if (direction == LV_DIR_TOP) line[j] = game2048Board[j][i];
            else line[j] = game2048Board[3 - j][i];
        }

        bool lineChanged = game2048SlideLine(line);
        changed = changed || lineChanged;

        for (uint8_t j = 0; j < 4; j++) {
            if (direction == LV_DIR_LEFT) game2048Board[i][j] = line[j];
            else if (direction == LV_DIR_RIGHT) game2048Board[i][3 - j] = line[j];
            else if (direction == LV_DIR_TOP) game2048Board[j][i] = line[j];
            else game2048Board[3 - j][i] = line[j];
        }
    }

    if (changed) {
        game2048AddRandomTile();
        game2048UpdateUi();
    }

    return changed;
}

static void game2048Reset()
{
    for (uint8_t r = 0; r < 4; r++) {
        for (uint8_t c = 0; c < 4; c++) {
            game2048Board[r][c] = 0;
        }
    }

    game2048Score = 0;
    game2048AddRandomTile();
    game2048AddRandomTile();
    game2048UpdateUi();
}

static void game2048CloseEvent(lv_event_t * e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;

    lv_obj_t * oldScreen = game2048Screen;
    game2048Screen = NULL;
    game2048BoardObj = NULL;
    game2048ScoreLabel = NULL;
    game2048StatusLabel = NULL;
    memset(game2048Tiles, 0, sizeof(game2048Tiles));

    lv_scr_load_anim(ui_Screen5, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 180, 0, false);
    if (oldScreen != NULL) lv_obj_del_async(oldScreen);
}

static void game2048ResetEvent(lv_event_t * e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        game2048Reset();
    }
}

static void game2048GestureEvent(lv_event_t * e)
{
    if (lv_event_get_code(e) != LV_EVENT_GESTURE || !game2048HasMoves()) return;

    lv_dir_t direction = lv_indev_get_gesture_dir(lv_indev_get_act());
    if (direction == LV_DIR_LEFT || direction == LV_DIR_RIGHT || direction == LV_DIR_TOP || direction == LV_DIR_BOTTOM) {
        lv_indev_wait_release(lv_indev_get_act());
        game2048Move(direction);
    }
}

static void show2048Game(lv_event_t * e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;

    if (game2048Screen != NULL) {
        lv_scr_load_anim(game2048Screen, LV_SCR_LOAD_ANIM_FADE_ON, 80, 0, false);
        return;
    }

    game2048Screen = lv_obj_create(NULL);
    lv_obj_clear_flag(game2048Screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(game2048Screen, game2048GestureEvent, LV_EVENT_ALL, NULL);
    lv_obj_set_style_bg_color(game2048Screen, lv_color_hex(0x010409), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(game2048Screen, lv_color_hex(0x14283A), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(game2048Screen, LV_GRAD_DIR_VER, LV_PART_MAIN);

    lv_obj_t * closeBtn = lv_btn_create(game2048Screen);
    lv_obj_set_size(closeBtn, 28, 28);
    lv_obj_align(closeBtn, LV_ALIGN_TOP_LEFT, 52, 22);
    lv_obj_set_style_radius(closeBtn, 14, LV_PART_MAIN);
    lv_obj_set_style_bg_color(closeBtn, lv_color_hex(0x1D2B36), LV_PART_MAIN);
    lv_obj_add_event_cb(closeBtn, game2048CloseEvent, LV_EVENT_ALL, NULL);
    lv_obj_t * closeLabel = lv_label_create(closeBtn);
    lv_label_set_text(closeLabel, LV_SYMBOL_CLOSE);
    lv_obj_center(closeLabel);

    lv_obj_t * resetBtn = lv_btn_create(game2048Screen);
    lv_obj_set_size(resetBtn, 28, 28);
    lv_obj_align(resetBtn, LV_ALIGN_TOP_RIGHT, -52, 22);
    lv_obj_set_style_radius(resetBtn, 14, LV_PART_MAIN);
    lv_obj_set_style_bg_color(resetBtn, lv_color_hex(0x1D2B36), LV_PART_MAIN);
    lv_obj_add_event_cb(resetBtn, game2048ResetEvent, LV_EVENT_ALL, NULL);
    lv_obj_t * resetLabel = lv_label_create(resetBtn);
    lv_label_set_text(resetLabel, LV_SYMBOL_REFRESH);
    lv_obj_center(resetLabel);

    lv_obj_t * title = lv_label_create(game2048Screen);
    lv_obj_set_width(title, 120);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);
    lv_label_set_text(title, "2048");
    lv_obj_set_style_text_color(title, lv_color_hex(0xF4FAFF), LV_PART_MAIN);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_18, LV_PART_MAIN);

    game2048ScoreLabel = lv_label_create(game2048Screen);
    lv_obj_set_width(game2048ScoreLabel, 100);
    lv_obj_align(game2048ScoreLabel, LV_ALIGN_TOP_MID, 0, 36);
    lv_obj_set_style_text_color(game2048ScoreLabel, lv_color_hex(0xB8D8E7), LV_PART_MAIN);
    lv_obj_set_style_text_align(game2048ScoreLabel, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_font(game2048ScoreLabel, &lv_font_montserrat_10, LV_PART_MAIN);

    game2048StatusLabel = lv_label_create(game2048Screen);
    lv_obj_set_width(game2048StatusLabel, 100);
    lv_obj_align(game2048StatusLabel, LV_ALIGN_BOTTOM_MID, 0, -7);
    lv_obj_set_style_text_color(game2048StatusLabel, lv_color_hex(0xB8D8E7), LV_PART_MAIN);
    lv_obj_set_style_text_align(game2048StatusLabel, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_font(game2048StatusLabel, &lv_font_montserrat_10, LV_PART_MAIN);

    game2048BoardObj = lv_obj_create(game2048Screen);
    lv_obj_remove_style_all(game2048BoardObj);
    lv_obj_set_size(game2048BoardObj, 154, 154);
    lv_obj_align(game2048BoardObj, LV_ALIGN_CENTER, 0, 24);
    lv_obj_clear_flag(game2048BoardObj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(game2048BoardObj, game2048GestureEvent, LV_EVENT_ALL, NULL);
    lv_obj_set_style_bg_color(game2048BoardObj, lv_color_hex(0x07111A), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(game2048BoardObj, 255, LV_PART_MAIN);
    lv_obj_set_style_radius(game2048BoardObj, 10, LV_PART_MAIN);

    for (uint8_t r = 0; r < 4; r++) {
        for (uint8_t c = 0; c < 4; c++) {
            game2048Tiles[r][c] = lv_label_create(game2048BoardObj);
            lv_obj_set_size(game2048Tiles[r][c], 34, 34);
            lv_obj_set_pos(game2048Tiles[r][c], 7 + (c * 37), 7 + (r * 37));
            lv_label_set_text(game2048Tiles[r][c], "");
            lv_obj_set_style_radius(game2048Tiles[r][c], 6, LV_PART_MAIN);
            lv_obj_set_style_bg_opa(game2048Tiles[r][c], 255, LV_PART_MAIN);
            lv_obj_set_style_text_align(game2048Tiles[r][c], LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
            lv_obj_set_style_text_font(game2048Tiles[r][c], &lv_font_montserrat_14, LV_PART_MAIN);
            lv_obj_set_style_pad_top(game2048Tiles[r][c], 8, LV_PART_MAIN);
        }
    }

    game2048Reset();
    lv_scr_load_anim(game2048Screen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 180, 0, false);
}

static void setupGameScreenInteractions()
{
    if (ui_games1 == NULL) return;

    lv_obj_add_flag(ui_games1, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(ui_games1, show2048Game, LV_EVENT_ALL, NULL);
}

static void setupPerformanceOverlay()
{
    uiPerfLabel = lv_label_create(lv_layer_top());
    lv_label_set_text(uiPerfLabel, "FPS --");
    lv_obj_set_size(uiPerfLabel, 54, 18);
    lv_obj_align(uiPerfLabel, LV_ALIGN_TOP_MID, 0, 6);
    lv_obj_clear_flag(uiPerfLabel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_radius(uiPerfLabel, 9, LV_PART_MAIN);
    lv_obj_set_style_bg_color(uiPerfLabel, lv_color_hex(0x05080D), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(uiPerfLabel, 135, LV_PART_MAIN);
    lv_obj_set_style_pad_top(uiPerfLabel, 2, LV_PART_MAIN);
    lv_obj_set_style_text_color(uiPerfLabel, lv_color_hex(0xF4FAFF), LV_PART_MAIN);
    lv_obj_set_style_text_align(uiPerfLabel, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_font(uiPerfLabel, &lv_font_montserrat_10, LV_PART_MAIN);
}

static void updatePerformanceOverlay()
{
    if (uiPerfLabel == NULL) return;

    uint32_t now = millis();
    if (now - uiLastPerfMillis < UI_PERF_UPDATE_MS) return;

    uint32_t currentFlushCount = uiFlushCount;
    uint32_t elapsed = now - uiLastPerfMillis;
    uint32_t fps = ((currentFlushCount - uiLastFlushCount) * 1000UL) / elapsed;

    char perfText[12];
    lv_snprintf(perfText, sizeof(perfText), "FPS %lu", (unsigned long)fps);
    lv_label_set_text(uiPerfLabel, perfText);

    uiLastFlushCount = currentFlushCount;
    uiLastPerfMillis = now;
}

#if LV_USE_LOG != 0
/* Serial debugging */
void my_print(const char * buf)
{
    Serial.printf(buf);
    Serial.flush();
}
#endif

/* Display flushing */
void my_disp_flush( lv_disp_drv_t *disp_drv, const lv_area_t *area, lv_color_t *color_p )
{
    uiFlushCount++;

    uint32_t w = ( area->x2 - area->x1 + 1 );
    uint32_t h = ( area->y2 - area->y1 + 1 );

    tft.startWrite();
    tft.setAddrWindow( area->x1, area->y1, w, h );
    tft.pushColors( ( uint16_t * )&color_p->full, w * h, true );
    tft.endWrite();

    lv_disp_flush_ready( disp_drv );
}

void example_increase_lvgl_tick(void *arg)
{
    /* Tell LVGL how many milliseconds has elapsed */
    lv_tick_inc(EXAMPLE_LVGL_TICK_PERIOD_MS);
}

static uint8_t count=0;
void example_increase_reboot(void *arg)
{
  count++;
  if(count==30){
    // esp_restart();
  }
    
}

/*Read the touchpad*/
void my_touchpad_read( lv_indev_drv_t * indev_drv, lv_indev_data_t * data )
{
    // uint16_t touchX, touchY;

    bool touched = touch.available();
    // touch.read_touch();
    if( !touched )
    // if( 0!=touch.data.points )
    {
        data->state = LV_INDEV_STATE_REL;
    }
    else
    {
        data->state = LV_INDEV_STATE_PR;

        /*Set the coordinates*/
        data->point.x = touch.data.x;
        data->point.y = touch.data.y;
        // Serial.print( "Data x " );
        // Serial.println( touch.data.x );

        // Serial.print( "Data y " );
        // Serial.println( touch.data.y );
    }
}

void setup()
{
    Serial.begin( 115200 ); /* prepare for possible serial debug */

    String LVGL_Arduino = "Hello Arduino! ";
    LVGL_Arduino += String('V') + lv_version_major() + "." + lv_version_minor() + "." + lv_version_patch();

    Serial.println( LVGL_Arduino );
    Serial.println( "I am LVGL_Arduino" );

    lv_init();
#if LV_USE_LOG != 0
    lv_log_register_print_cb( my_print ); /* register print function for debugging */
#endif

    tft.begin();          /* TFT init */
    tft.setRotation( 0 ); /* Landscape orientation, flipped */
    
    /*Set the touchscreen calibration data,
     the actual data for your display can be acquired using
     the Generic -> Touch_calibrate example from the TFT_eSPI library*/
    // uint16_t calData[5] = { 275, 3620, 264, 3532, 1 };
    // tft.setTouch( calData );
    touch.begin();
    setupBacklightPwm();
    setupBatteryAdc();

    lv_disp_draw_buf_init( &draw_buf, buf1, buf2, screenWidth * LVGL_DRAW_BUFFER_LINES );

    /*Initialize the display*/
    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init( &disp_drv );
    /*Change the following line to your display resolution*/
    disp_drv.hor_res = screenWidth;
    disp_drv.ver_res = screenHeight;
    disp_drv.flush_cb = my_disp_flush;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register( &disp_drv );

    /*Initialize the (dummy) input device driver*/
    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init( &indev_drv );
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = my_touchpad_read;
    lv_indev_drv_register( &indev_drv );
  
    /* Create simple label */
    lv_obj_t *label = lv_label_create( lv_scr_act() );
    lv_label_set_text( label, "Hello Ardino and LVGL!");
    lv_obj_align( label, LV_ALIGN_CENTER, 0, 0 );
 
    /* Try an example. See all the examples 
     * online: https://docs.lvgl.io/master/examples.html
     * source codes: https://github.com/lvgl/lvgl/tree/e7f88efa5853128bf871dde335c0ca8da9eb7731/examples */
     //lv_example_btn_1();
   
    const esp_timer_create_args_t lvgl_tick_timer_args = {
      .callback = &example_increase_lvgl_tick,
      .name = "lvgl_tick"
    };

    const esp_timer_create_args_t reboot_timer_args = {
      .callback = &example_increase_reboot,
      .name = "reboot"
    };

    esp_timer_handle_t lvgl_tick_timer = NULL;
    esp_timer_create(&lvgl_tick_timer_args, &lvgl_tick_timer);
    esp_timer_start_periodic(lvgl_tick_timer, EXAMPLE_LVGL_TICK_PERIOD_MS * 1000);

    esp_timer_handle_t reboot_timer = NULL;
    esp_timer_create(&reboot_timer_args, &reboot_timer);
    esp_timer_start_periodic(reboot_timer, 2000 * 1000);

     /*Or try out a demo. Don't forget to enable the demos in lv_conf.h. E.g. LV_USE_DEMOS_WIDGETS*/
    // lv_demo_widgets();               
    // lv_demo_benchmark();          
    // lv_demo_keypad_encoder();     
    // lv_demo_music();              
    // lv_demo_printer();
    // lv_demo_stress();
    ui_init();
    applyWatchTheme();
    styleWifiList();
    setupGameScreenInteractions();
    setupPerformanceOverlay();
    setupBrightnessControls();
    uiLastPerfMillis = millis();
    randomSeed((uint32_t)micros());
    updateBatteryUi();

    Serial.print("Connecting to ");
    Serial.println(ssid);
    setWifiStatus("Auto connect...");
    WiFi.begin(ssid, password);
    unsigned long setupWifiStart = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - setupWifiStart < 8000) {
        lv_timer_handler();
        delay(100);
        Serial.print(".");
    }
    Serial.println("");
    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("WiFi connected.");
        setWifiStatus("Connected");

        // Init and get the time
        configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
        struct tm timeinfo;
        if (getLocalTime(&timeinfo, 5000)) {
            updateWatchTime();
            printLocalTime();
        } else {
            Serial.println("NTP sync pending.");
        }
    } else {
        Serial.println("Auto WiFi connect skipped.");
        setWifiStatus("Tap Scan");
    }
    
    Serial.println( "Setup done" );
    
}
unsigned long lastTimeUpdate = 0;
unsigned long lastBatteryUpdate = 0;
void loop()
{
    uint32_t nextTimerMs = lv_timer_handler(); /* let the GUI do its work */
    updatePerformanceOverlay();

    unsigned long now = millis();
    if (now - lastTimeUpdate >= TIME_UPDATE_MS) {
        updateWatchTime();
        lastTimeUpdate = now;
    }
    if (now - lastBatteryUpdate >= BATTERY_UPDATE_MS) {
        updateBatteryUi();
        lastBatteryUpdate = now;
    }

    if (nextTimerMs > UI_LOOP_DELAY_MS) nextTimerMs = UI_LOOP_DELAY_MS;
    delay(nextTimerMs == 0 ? 1 : nextTimerMs);
}
