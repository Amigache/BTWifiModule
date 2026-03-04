/*
 * webserver.c
 * WiFi SoftAP + HTTP dashboard + OTA firmware update
 * AP is toggled via BOOT button (short press)
 */

#include "webserver.h"

#include <string.h>
#include <stdlib.h>

#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_heap_caps.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "nvs.h"

extern nvs_handle_t nvs_flsh_btw;

#include "bt.h"
#include "bt_client.h"
#include "bt_server.h"
#include "defines.h"
#include "terminal.h"

#define WEB_TAG      "WEBSERVER"
#define WIFI_SSID    "BTWifiModule"
#define WIFI_CHANNEL 6
#define WIFI_MAX_STA 4
#define OTA_BUF_SIZE 1024

/* AP state */
static volatile bool ap_active = false;

/* WebSocket state */
static httpd_handle_t web_server_handle  = NULL;
static int            ws_fd              = -1;
static uint8_t        ws_sent_dev_cnt    = 0;
static bool           ws_done_sent       = true;
static volatile bool  ws_send_info_now   = false;

/* Async WS send (called from any task via httpd_queue_work) */
typedef struct { httpd_handle_t hd; int fd; char *msg; } ws_work_t;
static void ws_send_cb(void *arg) {
    ws_work_t *w = (ws_work_t *)arg;
    httpd_ws_frame_t pkt = {
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)w->msg,
        .len = strlen(w->msg),
        .final = true,
    };
    if (httpd_ws_send_frame_async(w->hd, w->fd, &pkt) != ESP_OK)
        ws_fd = -1;
    free(w->msg);
    free(w);
}
static void ws_push(const char *msg) {
    if (ws_fd < 0 || web_server_handle == NULL) return;
    ws_work_t *w = malloc(sizeof(ws_work_t));
    if (!w) return;
    w->hd = web_server_handle;
    w->fd = ws_fd;
    w->msg = strdup(msg);
    if (!w->msg) { free(w); return; }
    if (httpd_queue_work(web_server_handle, ws_send_cb, w) != ESP_OK) {
        free(w->msg);
        free(w);
    }
}

/* -----------------------------------------------------------------------
 * HTML Dashboard (embedded)
 * ----------------------------------------------------------------------- */
static const char *INDEX_HTML =
    "<!DOCTYPE html>"
    "<html><head><meta charset='utf-8'><title>BTWifiModule</title>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<style>"
    "body{font-family:sans-serif;max-width:620px;margin:40px auto;padding:0 16px;"
    "background:#0f0f1a;color:#e0e0e0}"
    "h1{color:#e94560;letter-spacing:2px}h2{color:#ccc;font-size:1em;text-transform:uppercase;"
    "letter-spacing:1px;margin-bottom:8px}"
    ".card{background:#1a1a2e;border:1px solid #2a2a4a;border-radius:10px;padding:18px;"
    "margin:14px 0}"
    ".row{display:flex;justify-content:space-between;align-items:center;padding:7px 0;"
    "border-bottom:1px solid #2a2a4a}"
    ".row:last-child{border-bottom:none}"
    ".lbl{color:#888;font-size:.9em}.val{font-weight:600;color:#e94560}"
    ".badge{padding:2px 8px;border-radius:12px;font-size:.8em}"
    ".on{background:#14532d;color:#4ade80}.off{background:#450a0a;color:#f87171}"
    "input[type=file]{color:#ccc;margin:10px 0;width:100%;background:#0f0f1a;"
    "border:1px solid #2a2a4a;padding:8px;border-radius:6px;box-sizing:border-box}"
    "button{background:#e94560;color:#fff;border:none;padding:11px;border-radius:7px;"
    "cursor:pointer;font-size:15px;width:100%;margin-top:4px;font-weight:600}"
    "button:disabled{background:#555;cursor:not-allowed}"
    "#prog-wrap{display:none;background:#2a2a4a;border-radius:6px;height:18px;margin:10px 0;"
    "overflow:hidden}"
    "#bar{background:#e94560;height:100%;width:0%;transition:width .2s;border-radius:6px}"
    "#status{min-height:20px;font-size:.9em;margin-top:6px}"
    ".ok{color:#4ade80}.err{color:#f87171}.info{color:#60a5fa}"
    "@keyframes spin{to{transform:rotate(360deg)}}"
    ".spinner{display:inline-block;width:12px;height:12px;border:2px solid #888;"
    "border-top-color:#4ade80;border-radius:50%;animation:spin .8s linear infinite;"
    "vertical-align:middle;margin-right:6px}"
    "</style></head><body>"
    "<h1>&#x1F4F6; BTWifiModule</h1>"

    "<div class='card'><h2>Bluetooth</h2>"
    "<div class='row'><span class='lbl'>Name</span><span class='val' id='btname'>…</span></div>"
    "<div class='row'><span class='lbl'>MAC Address</span><span class='val' id='btmac'>…</span></div>"
    "<div class='row'><span class='lbl'>Role</span><span class='val' id='role'>…</span></div>"
    "<div class='row'><span class='lbl'>BLE Connected</span>"
    "<span id='conn' class='badge off'>Not Connected</span></div>"
    "</div>"

    "<div class='card'><h2>BT Controls</h2>"
    "<div class='row'><span class='lbl'>Device Name</span>"
    "<div style='display:flex;gap:8px;align-items:center'>"
    "<input type='text' id='new-name' maxlength='30'"
    " style='background:#0f0f1a;border:1px solid #2a2a4a;color:#e0e0e0;"
    "padding:6px 10px;border-radius:6px;width:110px' placeholder='BT Name'>"
    "<button style='width:auto;padding:8px 14px;margin:0' onclick='saveName()'>Save</button>"
    "</div></div>"
    "<div class='row'><span class='lbl'>BT Role</span>"
    "<div style='display:flex;gap:8px;align-items:center'>"
    "<select id='sel-role' style='background:#0f0f1a;border:1px solid #2a2a4a;color:#e0e0e0;"
    "padding:6px 10px;border-radius:6px'>"
    "<option value='1' selected>Peripheral (Trainer TX)</option>"
    "<option value='2'>Central (Trainer RX)</option>"
    "<option value='3'>Telemetry</option>"
    "</select>"
    "<button id='btn-role' style='width:auto;padding:8px 14px;margin:0' onclick='toggleRole()'>Connect</button>"
    "</div></div>"
    "<div id='bt-status' style='min-height:16px;font-size:.85em;margin-top:6px'></div>"
    "</div>"

    "<div class='card' id='scan-card' style='display:none'><h2>BLE Scanner</h2>"
    "<div class='row'><span class='lbl'>Nearby Devices</span>"
    "<button id='btn-scan' style='width:auto;padding:8px 14px;margin:0' onclick='startScan()'>Scan</button>"
    "</div>"
    "<div id='scan-status' style='font-size:.85em;color:#888;min-height:16px;margin:6px 0'></div>"
    "<div id='device-list'></div>"
    "</div>"

    "<div class='card'><h2>OTA Firmware Update</h2>"
    "<p style='color:#888;font-size:.85em;margin:0 0 10px'>Select a .bin file built for "
    "C3SuperMini and press Flash.</p>"
    "<input type='file' id='fw-file' accept='.bin'>"
    "<button id='btn' onclick='upload()'>&#x26A1; Flash Firmware</button>"
    "<div id='prog-wrap'><div id='bar'></div></div>"
    "<div id='status'></div>"
    "</div>"

    "<div class='card'><h2>System</h2>"
    "<div class='row'><span class='lbl'>Free Heap</span><span class='val' id='heap'>&#8230;</span></div>"
    "<div class='row'><span class='lbl'>Firmware</span><span class='val' id='fw' title=''>&#8230;</span></div>"
    "<div class='row'><span class='lbl'>WiFi AP IP</span><span class='val'>192.168.4.1</span></div>"
    "<button onclick='doReboot()' style='background:#7f1d1d;margin-top:12px'>&#x1F504; Reboot Device</button>"
    "</div>"

    "<script>"
    "var _disconnecting=false,_connecting=false;"
    "function applyInfo(d){"
    "  document.getElementById('btname').textContent=d.bt_name;"
    "  document.getElementById('btmac').textContent=d.bt_mac;"
    "  document.getElementById('role').textContent=d.role;"
    "  const c=document.getElementById('conn');"
    "  if(_connecting&&!d.ble_connected){"
    "    c.innerHTML='<span class=spinner></span>Connecting\xe2\x80\xa6';c.className='badge off';"
    "  }else if(_disconnecting&&!d.ble_connected){"
    "    _disconnecting=false;c.textContent='Not Connected';c.className='badge off';"
    "  }else if(d.ble_connected){"
    "    _connecting=false;_disconnecting=false;"
    "    c.className='badge on';"
    "    c.innerHTML=d.conn_mac+\""
    " <span onclick='bleDisconnect()'"
    " style='cursor:pointer;color:#f87171;font-weight:bold;font-size:16px;"
    "padding:2px 8px;margin-left:8px'>"
    "\xc3\x97</span>\";"
    "  }else{c.textContent='Not Connected';c.className='badge off';}"
    "  document.getElementById('heap').textContent="
    "    Math.round(d.free_heap/1024)+'KB free of '+Math.round(d.total_heap/1024)+'KB';"
    "  const fw=document.getElementById('fw');"
    "  fw.textContent=d.fw_short;fw.title=d.fw_version;"
    "  const sel=document.getElementById('sel-role');"
    "  const btn=document.getElementById('btn-role');"
    "  const active=d.bt_role!==0;"
    "  if(active){sel.value=String(d.bt_role);}"
    "  sel.disabled=active;sel.style.opacity=active?'0.5':'1';"
    "  if(active){btn.textContent='Disconnect';btn.style.background='#7f1d1d';}"
    "  else{btn.textContent='Connect';btn.style.background='';}"
    "  document.getElementById('scan-card').style.display=d.bt_role===2?'':'none';"
    "}"
    "let _ws=null,_wsOk=true;"
    "function openWS(){"
    "  if(!_wsOk)return;"
    "  _ws=new WebSocket('ws://'+location.host+'/ws');"
    "  _ws.onmessage=function(e){"
    "    try{"
    "      const d=JSON.parse(e.data);"
    "      if(d.t==='info'){applyInfo(d);}"
    "      else if(d.t==='dev'){"
    "        const list=document.getElementById('device-list');"
    "        const row=document.createElement('div');"
    "        row.className='row';row.style.marginTop='4px';"
    "        row.style.flexWrap='wrap';"
    "        var lbl=d.mac;"
    "        if(d.name)lbl=d.name+' <span style=\"color:#888;font-size:.8em\">('+d.mac+')</span>';"
    "        lbl+=' <span style=\"color:#60a5fa;font-size:.8em\">'+d.rssi+'dBm</span>';"
    "        row.innerHTML=\"<span class='lbl' style='font-family:monospace;flex:1;min-width:0'>\"+lbl+\"</span>\""
    "          +\"<button style='width:auto;padding:6px 12px;margin:0;flex-shrink:0' onclick='connectTo(\\\"\"+d.mac+\"\\\")'> Connect</button>\";"
    "        list.appendChild(row);"
    "      }else if(d.t==='done'){"
    "        document.getElementById('scan-status').textContent=d.n+' device(s) found.';"
    "        document.getElementById('btn-scan').disabled=false;"
    "      }"
    "    }catch(ex){}"
    "  };"
    "  _ws.onclose=function(){_ws=null;if(_wsOk)setTimeout(openWS,3000);};"
    "  _ws.onerror=function(){_ws=null;};"
    "}"
    "async function upload(){"
    "  const f=document.getElementById('fw-file').files[0];"
    "  if(!f){alert('Select a firmware .bin file first');return;}"
    "  const btn=document.getElementById('btn');"
    "  const st=document.getElementById('status');"
    "  const pw=document.getElementById('prog-wrap');"
    "  const bar=document.getElementById('bar');"
    "  _wsOk=false;if(_ws){_ws.close();_ws=null;}"
    "  btn.disabled=true;pw.style.display='block';"
    "  st.className='info';st.textContent='Uploading '+Math.round(f.size/1024)+'KB\u2026';"
    "  const xhr=new XMLHttpRequest();"
    "  xhr.upload.onprogress=e=>{"
    "    if(e.lengthComputable){"
    "      const p=Math.round(e.loaded/e.total*100);"
    "      bar.style.width=p+'%';"
    "      st.textContent='Uploading\u2026 '+p+'%';"
    "    }"
    "  };"
    "  xhr.onload=()=>{"
    "    if(xhr.status===200){"
    "      bar.style.width='100%';"
    "      st.className='ok';"
    "      st.innerHTML='<b>&#x2705; Firmware flashed!</b> Device is rebooting&#x2026;<br>'"
    "        +'<span style=\"color:#60a5fa\">Press the <b>BOOT</b> button on the device, '"
    "        +'then reconnect to the <b>BTWifiModule</b> WiFi AP to return to this page.</span>';"
    "      setTimeout(()=>{_wsOk=true;location.reload();},15000);"
    "    }else{"
    "      st.className='err';st.textContent='Error '+xhr.status+': '+xhr.responseText;"
    "      btn.disabled=false;_wsOk=true;openWS();"
    "    }"
    "  };"
    "  xhr.onerror=()=>{st.className='err';st.textContent='Network error';btn.disabled=false;_wsOk=true;openWS();};"
    "  xhr.open('POST','/ota');"
    "  xhr.setRequestHeader('Content-Type','application/octet-stream');"
    "  xhr.send(f);"
    "}"
    "async function saveName(){"
    "  const n=document.getElementById('new-name').value.trim();"
    "  if(!n){return;}"
    "  const st=document.getElementById('bt-status');"
    "  const r=await fetch('/bt/name',{method:'POST',body:n});"
    "  if(r.ok){st.className='ok';st.textContent='Name saved! Reconnect BT.';}"
    "  else{st.className='err';st.textContent='Error: '+await r.text();}"
    "}"
    "function startScan(){"
    "  document.getElementById('device-list').innerHTML='';"
    "  document.getElementById('btn-scan').disabled=true;"
    "  document.getElementById('scan-status').textContent='Scanning\u2026';"
    "  fetch('/bt/scan',{method:'POST'});"
    "}"
    "async function connectTo(mac){"
    "  _connecting=true;"
    "  var c=document.getElementById('conn');"
    "  c.innerHTML='<span class=spinner></span>Connecting to '+mac+'\xe2\x80\xa6';c.className='badge off';"
    "  document.getElementById('scan-status').textContent='Connecting to '+mac+'\xe2\x80\xa6';"
    "  await fetch('/bt/connect',{method:'POST',body:mac});"
    "  document.getElementById('device-list').innerHTML='';"
    "  document.getElementById('scan-status').textContent='';"
    "}"
    "async function bleDisconnect(){"
    "  _disconnecting=true;"
    "  var c=document.getElementById('conn');"
    "  c.innerHTML='<span class=spinner></span>Disconnecting\xe2\x80\xa6';c.className='badge off';"
    "  await fetch('/bt/disconnect',{method:'POST'});"
    "}"
    "async function toggleRole(){"
    "  const sel=document.getElementById('sel-role');"
    "  const btn=document.getElementById('btn-role');"
    "  const st=document.getElementById('bt-status');"
    "  const disconnecting=btn.textContent==='Disconnect';"
    "  const role=disconnecting?'0':sel.value;"
    "  const r=await fetch('/bt/role',{method:'POST',body:role});"
    "  if(r.ok){"
    "    st.className='ok';"
    "    st.textContent=disconnecting?'BT stopped.':'Starting BT\u2026';"
    "    setTimeout(()=>{st.textContent='';},3000);"
    "  }else{st.className='err';st.textContent='Error: '+await r.text();}"
    "}"
    "async function doReboot(){"
    "  if(!confirm('Reboot the device?'))return;"
    "  await fetch('/reboot',{method:'POST'});"
    "  document.body.innerHTML='<div style=\"text-align:center;margin-top:30vh;font-family:sans-serif\">'"
    "    +'<h1 style=\"color:#e94560\">\xf0\x9f\x94\x84 Rebooting\u2026</h1>'"
    "    +'<p style=\"color:#60a5fa;font-size:1.1em\">Press the <b>BOOT</b> button on the device,<br>'"
    "    +'then reconnect to the <b>BTWifiModule</b> WiFi AP to return to this page.</p>'"
    "    +'</div>';"
    "}"
    "openWS();"
    "</script></body></html>";

/* -----------------------------------------------------------------------
 * HTTP handlers
 * ----------------------------------------------------------------------- */

static esp_err_t handler_index(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, INDEX_HTML, strlen(INDEX_HTML));
    return ESP_OK;
}

static void build_info_json(char *buf, size_t len)
{
    char mac_str[13] = "000000000000";
    btaddrtostr(mac_str, localbtaddress);

    const char *roles[] = {"Unknown", "BLE Peripheral (Trainer TX)",
                            "BLE Central (Trainer RX)", "BLE Telemetry", "Advanced"};
    role_t cur = getCurRole();
    const char *role_str = (cur < ROLE_COUNT) ? roles[cur] : "Unknown";

    bool connected = (cur == ROLE_BLE_PERIPHERAL || cur == ROLE_BLE_TELEMETRY)
                         ? btp_connected : btc_connected;

    char rmt_mac[13] = "";
    if (connected) btaddrtostr(rmt_mac, rmtbtaddress);

    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_app_desc_t app_desc;
    const char *fw_ver = "unknown";
    char fw_buf[80];
    char fw_short[32] = "unknown";
    if (esp_ota_get_partition_description(running, &app_desc) == ESP_OK) {
        snprintf(fw_buf, sizeof(fw_buf), "%s (%s %s)",
                 app_desc.version, app_desc.date, app_desc.time);
        strncpy(fw_short, app_desc.version, sizeof(fw_short) - 1);
        fw_ver = fw_buf;
    }

    snprintf(buf, len,
             "{\"t\":\"info\","
             "\"bt_name\":\"%s\","
             "\"bt_mac\":\"%s\","
             "\"role\":\"%s\","
             "\"ble_connected\":%s,"
             "\"bt_enabled\":%s,"
             "\"bt_role\":%d,"
             "\"free_heap\":%lu,"
             "\"total_heap\":%lu,"
             "\"fw_version\":\"%s\","
             "\"fw_short\":\"%s\","
             "\"conn_mac\":\"%s\"}",
             btname, mac_str, role_str,
             connected ? "true" : "false",
             (cur != ROLE_UNKNOWN) ? "true" : "false",
             (int)cur,
             (unsigned long)esp_get_free_heap_size(),
             (unsigned long)heap_caps_get_total_size(MALLOC_CAP_DEFAULT),
             fw_ver, fw_short, rmt_mac);
}

static esp_err_t handler_info(httpd_req_t *req)
{
    char json[680];
    build_info_json(json, sizeof(json));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, json, strlen(json));
    return ESP_OK;
}

static esp_err_t handler_ota(httpd_req_t *req)
{
    ESP_LOGI(WEB_TAG, "OTA update started, content length: %d", req->content_len);

    if (req->content_len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No firmware data");
        return ESP_FAIL;
    }

    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (!update_partition) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No OTA partition found");
        return ESP_FAIL;
    }
    ESP_LOGI(WEB_TAG, "Writing to partition: %s at offset 0x%lx", update_partition->label,
             update_partition->address);

    esp_ota_handle_t ota_handle;
    esp_err_t err = esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(WEB_TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA begin failed");
        return ESP_FAIL;
    }

    char *buf = (char *)malloc(OTA_BUF_SIZE);
    if (!buf) {
        esp_ota_abort(ota_handle);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }

    int remaining = req->content_len;
    int received  = 0;
    bool error    = false;

    while (remaining > 0) {
        int chunk = httpd_req_recv(req, buf, MIN(remaining, OTA_BUF_SIZE));
        if (chunk < 0) {
            if (chunk == HTTPD_SOCK_ERR_TIMEOUT) continue;
            ESP_LOGE(WEB_TAG, "httpd_req_recv error: %d", chunk);
            error = true;
            break;
        }
        err = esp_ota_write(ota_handle, buf, chunk);
        if (err != ESP_OK) {
            ESP_LOGE(WEB_TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
            error = true;
            break;
        }
        remaining -= chunk;
        received  += chunk;
        ESP_LOGI(WEB_TAG, "OTA progress: %d / %d bytes", received, req->content_len);
    }

    free(buf);

    if (error) {
        esp_ota_abort(ota_handle);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Write error");
        return ESP_FAIL;
    }

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(WEB_TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA validation failed");
        return ESP_FAIL;
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(WEB_TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Set boot partition failed");
        return ESP_FAIL;
    }

    ESP_LOGI(WEB_TAG, "OTA complete. Rebooting...");
    httpd_resp_send(req, "OK", 2);

    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();

    return ESP_OK;
}

static esp_err_t handler_reboot(httpd_req_t *req)
{
    ESP_LOGI(WEB_TAG, "Reboot requested from web UI");
    httpd_resp_send(req, "OK", 2);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

/* -----------------------------------------------------------------------
 * BT control handlers
 * ----------------------------------------------------------------------- */

static esp_err_t handler_bt_name(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > 30) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Name must be 1-30 chars");
        return ESP_FAIL;
    }
    char name[32] = {0};
    int ret = httpd_req_recv(req, name, req->content_len);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Recv error");
        return ESP_FAIL;
    }
    name[ret] = '\0';
    btSetName(name);
    nvs_set_str(nvs_flsh_btw, "btname", name);
    nvs_commit(nvs_flsh_btw);
    ESP_LOGI(WEB_TAG, "BT name set to: %s", name);
    httpd_resp_send(req, "OK", 2);
    return ESP_OK;
}

static esp_err_t handler_bt_role(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > 2) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Expected role 0-3");
        return ESP_FAIL;
    }
    char buf[4] = {0};
    httpd_req_recv(req, buf, req->content_len);
    int r = atoi(buf);
    if (r < ROLE_UNKNOWN || r >= ROLE_COUNT) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Role out of range");
        return ESP_FAIL;
    }
    ESP_LOGI(WEB_TAG, "Web role change -> %d", r);
    setRole((role_t)r);
    httpd_resp_send(req, "OK", 2);
    return ESP_OK;
}

/* -----------------------------------------------------------------------
 * BLE Scan / Connect handlers
 * ----------------------------------------------------------------------- */

static esp_err_t handler_bt_scan_start(httpd_req_t *req)
{
    if (getCurRole() != ROLE_BLE_CENTRAL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Not in Central mode");
        return ESP_FAIL;
    }
    ws_sent_dev_cnt = 0;
    ws_done_sent    = false;
    webStartScan();
    httpd_resp_send(req, "OK", 2);
    return ESP_OK;
}

static esp_err_t handler_ws(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        ws_fd = httpd_req_to_sockfd(req);
        ws_send_info_now = true;  // push info immediately on connect
        ESP_LOGI(WEB_TAG, "WS client connected fd=%d", ws_fd);
        return ESP_OK;
    }
    httpd_ws_frame_t pkt = {.type = HTTPD_WS_TYPE_TEXT};
    uint8_t buf[16] = {0};
    pkt.payload = buf;
    esp_err_t ret = httpd_ws_recv_frame(req, &pkt, sizeof(buf) - 1);
    if (ret != ESP_OK || pkt.type == HTTPD_WS_TYPE_CLOSE) {
        ws_fd = -1;
        ESP_LOGI(WEB_TAG, "WS client disconnected");
    }
    return ESP_OK;
}

static void scan_monitor_task(void *arg)
{
    TickType_t last_info = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(200));
        if (ws_fd < 0) continue;

        /* Push info: immediately on connect, then every 2s */
        TickType_t now = xTaskGetTickCount();
        if (ws_send_info_now || (now - last_info) >= pdMS_TO_TICKS(2000)) {
            char *info = malloc(1024);
            if (info) {
                build_info_json(info, 1024);
                ws_push(info);
                free(info);
            }
            last_info = now;
            ws_send_info_now = false;
        }

        if (ws_done_sent) continue;

        /* Push newly discovered devices */
        while (ws_sent_dev_cnt < bt_scanned_address_cnt) {
            esp_bt_addr_t_rp *d = &btc_scanned_addresses[ws_sent_dev_cnt];
            char mac[13];
            btaddrtostr(mac, d->addr);
            char msg[120];
            snprintf(msg, sizeof(msg),
                "{\"t\":\"dev\",\"mac\":\"%s\",\"rssi\":%d,\"name\":\"%s\"}",
                mac, d->rssi, d->name);
            ws_push(msg);
            ws_sent_dev_cnt++;
        }

        /* Push scan complete */
        if (btc_scan_complete) {
            char msg[40];
            snprintf(msg, sizeof(msg), "{\"t\":\"done\",\"n\":%d}", bt_scanned_address_cnt);
            ws_push(msg);
            ws_done_sent = true;
        }
    }
}

static esp_err_t handler_bt_disconnect(httpd_req_t *req)
{
    webDisconnect();
    ESP_LOGI(WEB_TAG, "Web: BLE disconnect requested");
    httpd_resp_send(req, "OK", 2);
    return ESP_OK;
}

static esp_err_t handler_bt_connect(httpd_req_t *req)
{
    if (req->content_len != 12) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Expected 12-char MAC (no separators)");
        return ESP_FAIL;
    }
    char mac[13] = {0};
    httpd_req_recv(req, mac, 12);
    mac[12] = '\0';
    webConnect(mac);
    httpd_resp_send(req, "OK", 2);
    return ESP_OK;
}

/* -----------------------------------------------------------------------
 * HTTP server start
 * ----------------------------------------------------------------------- */

static void start_http_server(void)
{
    httpd_config_t config   = HTTPD_DEFAULT_CONFIG();
    config.stack_size       = 8192;
    config.recv_wait_timeout = 30;
    config.send_wait_timeout = 30;
    config.max_open_sockets  = 7;
    config.max_uri_handlers  = 14;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(WEB_TAG, "Failed to start HTTP server");
        return;
    }
    web_server_handle = server;

    static const httpd_uri_t uri_index = {
        .uri = "/", .method = HTTP_GET, .handler = handler_index};
    static const httpd_uri_t uri_info = {
        .uri = "/info", .method = HTTP_GET, .handler = handler_info};
    static const httpd_uri_t uri_ota = {
        .uri = "/ota", .method = HTTP_POST, .handler = handler_ota};
    static const httpd_uri_t uri_bt_name = {
        .uri = "/bt/name", .method = HTTP_POST, .handler = handler_bt_name};
    static const httpd_uri_t uri_bt_toggle = {
        .uri = "/bt/role", .method = HTTP_POST, .handler = handler_bt_role};
    static const httpd_uri_t uri_scan_start = {
        .uri = "/bt/scan", .method = HTTP_POST, .handler = handler_bt_scan_start};
    static const httpd_uri_t uri_bt_connect = {
        .uri = "/bt/connect", .method = HTTP_POST, .handler = handler_bt_connect};
    static const httpd_uri_t uri_bt_disconnect = {
        .uri = "/bt/disconnect", .method = HTTP_POST, .handler = handler_bt_disconnect};
    static const httpd_uri_t uri_reboot = {
        .uri = "/reboot", .method = HTTP_POST, .handler = handler_reboot};
    static const httpd_uri_t uri_ws = {
        .uri = "/ws", .method = HTTP_GET,
        .handler = handler_ws, .is_websocket = true};

    httpd_register_uri_handler(server, &uri_index);
    httpd_register_uri_handler(server, &uri_info);
    httpd_register_uri_handler(server, &uri_ota);
    httpd_register_uri_handler(server, &uri_bt_name);
    httpd_register_uri_handler(server, &uri_bt_toggle);
    httpd_register_uri_handler(server, &uri_scan_start);
    httpd_register_uri_handler(server, &uri_bt_connect);
    httpd_register_uri_handler(server, &uri_bt_disconnect);
    httpd_register_uri_handler(server, &uri_reboot);
    httpd_register_uri_handler(server, &uri_ws);

    ESP_LOGI(WEB_TAG, "HTTP server started at http://192.168.4.1");
}

/* -----------------------------------------------------------------------
 * WiFi SoftAP — start / stop
 * ----------------------------------------------------------------------- */

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *e = (wifi_event_ap_staconnected_t *)data;
        ESP_LOGI(WEB_TAG, "Station connected, AID=%d", e->aid);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *e = (wifi_event_ap_stadisconnected_t *)data;
        ESP_LOGI(WEB_TAG, "Station disconnected, AID=%d", e->aid);
    }
}

static bool wifi_inited = false;
static esp_netif_t *ap_netif = NULL;

static void ap_start(void)
{
    if (ap_active) return;

    if (!wifi_inited) {
        ESP_ERROR_CHECK(esp_netif_init());
        ESP_ERROR_CHECK(esp_event_loop_create_default());
        ap_netif = esp_netif_create_default_wifi_ap();
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&cfg));
        ESP_ERROR_CHECK(esp_event_handler_instance_register(
            WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
        wifi_inited = true;
    }

    wifi_config_t wifi_cfg = {
        .ap = {
            .ssid           = WIFI_SSID,
            .ssid_len       = strlen(WIFI_SSID),
            .channel        = WIFI_CHANNEL,
            .password       = "",
            .max_connection = WIFI_MAX_STA,
            .authmode       = WIFI_AUTH_OPEN,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ap_active = true;
    ESP_LOGI(WEB_TAG, "SoftAP started  SSID: %s  (open)  CH: %d", WIFI_SSID, WIFI_CHANNEL);

    start_http_server();
}

static void ap_stop(void)
{
    if (!ap_active) return;

    /* Close WS */
    ws_fd = -1;

    /* Stop HTTP server */
    if (web_server_handle) {
        httpd_stop(web_server_handle);
        web_server_handle = NULL;
    }

    esp_wifi_stop();
    ap_active = false;
    ESP_LOGI(WEB_TAG, "SoftAP stopped");
}

/* -----------------------------------------------------------------------
 * BOOT button task — short press toggles AP
 * ----------------------------------------------------------------------- */

#if defined(BOOT_BTN_PIN)
static void boot_button_task(void *arg)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BOOT_BTN_PIN),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    bool last_level = true;  /* button released (pulled up) */
    ESP_LOGI(WEB_TAG, "BOOT button task started, GPIO%d initial=%d", BOOT_BTN_PIN, gpio_get_level(BOOT_BTN_PIN));

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(50));
        bool cur = gpio_get_level(BOOT_BTN_PIN);
        if (last_level && !cur) {
            /* Falling edge: button just pressed — debounce */
            vTaskDelay(pdMS_TO_TICKS(50));
            cur = gpio_get_level(BOOT_BTN_PIN);
            if (!cur) {
                /* Still pressed — toggle AP */
                ESP_LOGI(WEB_TAG, "BOOT button pressed, ap_active=%d -> toggling", ap_active);
                if (ap_active) ap_stop();
                else           ap_start();
            }
        }
        last_level = cur;
    }
}
#endif

/* -----------------------------------------------------------------------
 * LED task — fast blink in AP mode, solid if BLE connected, off otherwise
 * ----------------------------------------------------------------------- */

#if defined(LED_PIN)
static inline void led_set(bool on)
{
#if defined(LED_ACTIVE_LOW) && LED_ACTIVE_LOW
    gpio_set_level(LED_PIN, on ? 0 : 1);
#else
    gpio_set_level(LED_PIN, on ? 1 : 0);
#endif
}

static void led_task(void *arg)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << LED_PIN),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    led_set(false);

    for (;;) {
        if (ap_active) {
            /* Fast blink */
            led_set(true);
            vTaskDelay(pdMS_TO_TICKS(80));
            led_set(false);
            vTaskDelay(pdMS_TO_TICKS(80));
        } else {
            bool connected = btp_connected || btc_connected;
            led_set(connected);
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }
}
#endif

/* -----------------------------------------------------------------------
 * webserver_start — called from app_main
 * ----------------------------------------------------------------------- */

void webserver_start(void)
{
    // Restore saved BT name from NVS
    char saved_name[32] = {0};
    size_t name_len = sizeof(saved_name);
    if (nvs_get_str(nvs_flsh_btw, "btname", saved_name, &name_len) == ESP_OK
            && saved_name[0] != '\0') {
        btSetName(saved_name);
        ESP_LOGI(WEB_TAG, "Restored BT name: %s", saved_name);
    }

    /* AP is NOT started here — user must press BOOT button */
    ESP_LOGI(WEB_TAG, "AP off. Press BOOT button to enable WiFi AP.");

    /* Start scan monitor task (runs regardless of AP) */
    xTaskCreate(scan_monitor_task, "scan_mon", 4096, NULL, tskIDLE_PRIORITY + 1, NULL);

#if defined(BOOT_BTN_PIN)
    xTaskCreate(boot_button_task, "boot_btn", 8192, NULL, tskIDLE_PRIORITY + 1, NULL);
#endif

#if defined(LED_PIN)
    xTaskCreate(led_task, "led", 2048, NULL, tskIDLE_PRIORITY, NULL);
#endif
}
