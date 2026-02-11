SET(SOURCES components/ui_comp_batterygroup.c
    components/ui_comp_buttondown.c
    components/ui_comp_buttonround.c
    components/ui_comp_buttontop.c
    components/ui_comp_dailymissiongroup.c
    components/ui_comp_dategroup.c
    components/ui_comp_forecastgroup.c
    components/ui_comp_pulsegroup.c
    components/ui_comp_stepgroup.c
    components/ui_comp_titlegroup.c
    components/ui_comp_todayweathergroup.c
    components/ui_comp_weathergroup1.c
    components/ui_comp.c
    screens/ui_watch_digital.c
    screens/ui_watch_analog.c
    ui.c
    components/ui_comp_hook.c
    ui_helpers.c
    images/ui_img_bg1_png.c
    images/ui_img_sec_dot_png.c
    images/ui_img_clockwise_sec_png.c
    images/ui_img_clockwise_min_png.c
    images/ui_img_clockwise_hour_png.c
    fonts/ui_font_H1.c
    fonts/ui_font_Number_big.c
    fonts/ui_font_Number_extra.c
    fonts/ui_font_Subtitle.c
    fonts/ui_font_Title.c)

add_library(ui ${SOURCES})
