#ifndef WIFI_CONNECT_H
#define WIFI_CONNECT_H

// #include <time.h>

#include <WiFi.h>
#include "time.h"


const char* ssid_     = "eSpark Consulting";
const char* password_ = "47356880";

const char* ntpServer_ = "pool.ntp.org";
const long  gmtOffset_sec_ = 0;
const int   daylightOffset_sec_ = 3600;

void scan_wifi();
void printLocalTime();


#endif