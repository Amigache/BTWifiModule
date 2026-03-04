#pragma once

/**
 * @brief Start the WiFi SoftAP and HTTP web server.
 *
 * SSID    : BTWifiModule
 * Password: (open – no password)
 * IP      : 192.168.4.1
 *
 * Routes:
 *   GET  /       – Dashboard HTML (BT info + OTA upload form)
 *   GET  /info   – JSON with BT name, MAC, role, heap
 *   POST /ota    – Raw binary OTA upload (Content-Type: application/octet-stream)
 */
void webserver_start(void);
