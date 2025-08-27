/*
 Name:		esp32_wifi_thermostat.ino
 Author:	DIYLESS
*/
#include <WiFi.h>
#include <WiFiClient.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include "time.h"

#include "secrets.h"
#include "RingBuffer.h"
#include <OpenTherm.h>
#include "ATC_MiThermometer.h"

const char* ssid = WIFI_SSID;
const char* password = WIFI_PASSWORD;

IPAddress metricsServer(192,168,178,209);
WiFiClient client;

//Master OpenTherm Shield pins configuration
const int OT_IN_PIN = 21;  //4 for ESP8266 (D2), 21 for ESP32
const int OT_OUT_PIN = 22; //5 for ESP8266 (D1), 22 for ESP32

// known mac address of mi sensor
std::vector<std::string> miMacAddresses = { MI_MAC_ADDR };
ATC_MiThermometer miThermometer(miMacAddresses);
const int scanTime = 3; // BLE scan time in seconds
const int ble_poll_frequency = 15; // run a scan every five seconds

OpenTherm ot(OT_IN_PIN, OT_OUT_PIN);
WebServer server(80);

const char HTTP_HEAD_BEGIN[] PROGMEM = "<!DOCTYPE html><html lang=\"en\"><head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\"/><title>{v}</title>";
const char HTTP_STYLE[] PROGMEM = "<style>\
body {text-align:center;font-family:verdana;}\
tr:nth-child(odd) {background-color:#eee;}\
td{text-align:center;}\
.s{position:relative;display:inline-block;width:48px;height:16px;top:1px;}\
.s input{display:none;padding:0px;margin:0px;}\
.sl{position:absolute;cursor:pointer;top:0;left:0;right:0;bottom:0;background-color:#ccc;-webkit-transition:.4s;transition:.4s;border-radius:16px;}\
.sl:before{position:absolute;content:\"\";height:14px;width:14px;left:4px;bottom:1px;background-color:white;-webkit-transition:.4s;transition:.4s;border-radius:50%;}\
input:checked + .sl{background-color:#2196F3;}\
input.e:checked + .sl{background-color:red;}\
input:focus + .sl{box-shadow:0 0 1px #2196F3;}\
input:checked + .sl:before{-webkit-transform:translateX(26px);-ms-transform:translateX(26px);transform:translateX(26px);}\
.p {border:1px solid #2196F3;position:relative;}\
.p div{background-color:#2196F3;}\
.p span{position:absolute;width:100%;height:100%;top:0;left:0;}\
</style>";

const char HTTP_SCRIPT_VARS[] PROGMEM = "var drag=false,clickY,moveY=0,lastMoveY=0,room_setpoint=0;";
const char HTTP_SCRIPT_START_DRAG[] PROGMEM = "function startDrag(t){t.preventDefault(),drag=true,clickY=\"touchstart\"===t.type?t.changedTouches[0].clientY:t.clientY;}";
const char HTTP_SCRIPT_DO_DRAG[] PROGMEM = "function doDrag(e){if(e.preventDefault(),drag){var t=\"touchmove\"===e.type?e.changedTouches[0].clientY:e.clientY;moveY=Math.round(lastMoveY+(t-clickY));var o=parseInt(document.getElementById(\"roomL\").getAttribute(\"y1\"));moveY>0?moveY>286-o&&(moveY=286-o):moveY<186-o&&(moveY=186-o),document.getElementById(\"roomSp\").setAttribute(\"transform\",\"translate(0, \"+moveY+\")\"),room_setpoint=(286-o-moveY)/4+5,document.getElementById(\"roomTxt\").textContent=room_setpoint+\" \\u2103\";}}";
const char HTTP_SCRIPT_STOP_DRAG[] PROGMEM = "function stopDrag(e){if(e.preventDefault(),drag){drag=!1,lastMoveY=moveY;document.getElementById('info').innerHTML = document.getElementById('info').innerHTML;attachEvents(0);var t = new XMLHttpRequest;t.open(\"POST\", \"/room_setpoint\", !0), t.setRequestHeader(\"Content-type\", \"application/x-www-form-urlencoded\"), t.send(\"value=\" + room_setpoint)}}";
const char HTTP_SCRIPT_SEND_STATUS[] PROGMEM = "function sendStatus(){for(var e=document.getElementsByTagName(\"input\"),t=0,n=0;n<e.length;n++)if(e[n].checked){var a=parseInt(e[n].id);a>=0&&a<=7&&(t|=1<<a)}var s=new XMLHttpRequest;s.open(\"POST\",\"/status\",!0),s.setRequestHeader(\"Content-type\",\"application/x-www-form-urlencoded\"),s.send(\"value=\"+t)}";
const char HTTP_SCRIPT_ATTACH_EVENTS[] PROGMEM = "function attachEvents(reset){if(reset){lastMoveY=0;} for(var t=document.getElementsByTagName(\"input\"),e=0;e<t.length;e++)t[e].addEventListener(\"change\",sendStatus);var n=document.getElementById(\"roomT\");n.addEventListener(\"touchstart\",startDrag,!1),n.addEventListener(\"touchmove\",doDrag,!1),n.addEventListener(\"touchend\",stopDrag,!1),n.addEventListener(\"touchcancel\",stopDrag,!1)}";
const char HTTP_SCRIPT_UPDATE[] PROGMEM = "window.setInterval(\"update()\", 3000);function update(){if (drag) return;var xhr=new XMLHttpRequest();xhr.open(\"GET\", \"/info\", true);xhr.onreadystatechange = function () {if (drag || xhr.readyState != XMLHttpRequest.DONE || xhr.status != 200) return;document.getElementById('info').innerHTML=xhr.responseText;room_setpoint=parseFloat(document.getElementById('rsp').value);attachEvents(1);};xhr.setRequestHeader(\"Content-type\",\"application/x-www-form-urlencoded\");xhr.send();}"; //\"value=\"+room_setpoint

const char HTTP_HEAD_END[] PROGMEM = "</head><body onload=\"attachEvents(1)\"><div style=\"text-align:left;display:inline-block;\">";
const char HTTP_END[] PROGMEM = "</div></body></html>";
const char HTTP_INFO[] PROGMEM = "<table width=\"100%\">\
<tr><th>Central Heating</th><td><label class=\"s\"><input type=\"checkbox\" id=\"0\" {s} /><div class=\"sl\"></div></label></td></tr>\
<tr><th>Direct bluetooth sensor</th><td><label class=\"s\"><input type=\"checkbox\" id=\"1\" {d} /><div class=\"sl\"></div></label></td></tr>\
<tr><th>Cooling</th><td><label class=\"s\"><input type=\"checkbox\" id=\"2\" {c} /><div class=\"sl\"></div></label></td></tr>\
<tr><th>Fault</th><td><label class=\"s\"><input class=\"e\" type=\"checkbox\" disabled {0} /><div class=\"sl\"></div></label></td></tr>\
<tr><th>Diagnostic</th><td><label class=\"s\"><input class=\"e\" type=\"checkbox\" disabled {6} /><div class=\"sl\"></div></label></td></tr>\
<tr><th>CH temperature</th><td><div class=\"p\"><div style=\"width:{t}%\">&nbsp;</div><span>{t} &#8451;</span></div></td></tr>\
<tr><th>Relative Modulation Level</th><td width=\"86px\"><div class=\"p\"><div style=\"width:{m}%\">&nbsp;</div><span>{m} %</span></div></td></tr>\
<tr><th>Room Temperature</th><td><div class=\"p\"><div style=\"width:{rp}%\">&nbsp;</div><span>{r} &#8451;</span></div></td></tr>\
</table><input type=\"hidden\" id=\"rsp\" value=\"{rsp}\"/>";


OpenThermMessageID requests[] = {
  OpenThermMessageID::Status,
  OpenThermMessageID::TSet,
  OpenThermMessageID::Tboiler,
  OpenThermMessageID::RelModLevel,
};
const byte requests_count = sizeof(requests) / sizeof(uint8_t);

#define MASTER_STATUS_CH_ENABLED 0x1
#define DIRECT_BLUETOOTH_ENABLED 0x2
#define MASTER_STATUS_COOLING_ENABLED 0x4

uint8_t CHEnabled = 0, DirectBluetoothEnabled = 0, CoolingEnabled = 0;

uint8_t boiler_status = 0;
float ch_temperature = 0;
float ch_setpoint = 0;
float room_temperature = 18;
float room_setpoint = 18;
float modulation_level = 0;
unsigned long marked_min = 0;

bool hysteresis = true;
float threshold = 0.2;  // margin
float restartDelta = 0.5;  // prevents rapid switching

float Kp = 30.0;         // Proportional gain
float Ki = 0.1;          // Integral gain
float Kd = 5.0;          // Derivative gain
float P = 0.0;
float I = 0.0;
float D = 0.0;

float output = 0.0;

float prev_error = 0.0;
float integral = 0.0;
unsigned long last_time = 0;

const float minWaterTemp = 15.0;  // Minimum allowed boiler temp
const float maxWaterTemp = 70.0;  // Maximum allowed boiler temp

unsigned long ts = 0, new_ts = 0; //timestamp
unsigned long ble_ts = 0, new_ble_ts = 0; //timestamp

RingBuffer<ChartItem, 300> chart_items;
byte req_idx = 0;
ChartItem* curr_item = NULL;

// NTP server to request epoch time
const char* ntpServer = "pool.ntp.org";
// Variable to save current epoch time
unsigned long epochTime; 
// Function that gets current epoch time
unsigned long getTime() {
  time_t now;
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    //Serial.println("Failed to obtain time");
    return(0);
  }
  time(&now);
  return now;
}

void ICACHE_RAM_ATTR handleInterrupt() {
    ot.handleInterrupt();
}

float getTemperature() {
  if (DirectBluetoothEnabled == 1) {
    miThermometer.resetData();
  
    // Get sensor data - run BLE scan for <scanTime>
    unsigned found = miThermometer.getData(scanTime);

    if (miThermometer.data[0].valid) {
      Serial.printf("%.2f°C\n", miThermometer.data[0].temperature/100.0);
      Serial.println();
      // Delete results from BLEScan buffer to release memory
      miThermometer.clearScanResults();
      return miThermometer.data[0].temperature / 100.0;
    } else {
      Serial.println("Mi sensor not found during this scan.");
    }
    Serial.println();
  } else {
    Serial.println("Using external API temperature source.");
  }
  return room_temperature;
}

float computeBoilerWaterSetpoint(float setpoint, float roomTemp, float dt) {

    float error = setpoint - roomTemp;  // Temperature difference
    // Anti-windup: Stop integrating if no heating is needed
    if (error < 0) {
      integral = 0;
    } else {
      integral += error * dt;  // Accumulate error
    }
    float derivative = (error - prev_error) / dt;
    prev_error = error;

    P = (Kp * error);
    I = (Ki * integral);
    D = (Kd * derivative);

    float out = P + I + D;
    
    // Clamp output to valid water temperature range
    output = constrain(out, minWaterTemp, maxWaterTemp);

    Serial.println("room setpoint:" + String(setpoint) + " room temperature=" + String(roomTemp) + " dt=" + String(dt) + " boiler setpoint=" + String(output) + " P=" + String(P) + " I=" + String(I) + " D=" + String(D));

    return output;
}


void updateBoilerState(float setpoint, float roomTemp) {
    if (CHEnabled == 1 && roomTemp >= setpoint - threshold) {
        // Turn off the boiler if the temperature is within the desired range
        CHEnabled = 0;
    } else if (CHEnabled == 0 && roomTemp <= setpoint - restartDelta) {
        // Turn the boiler back on only if temperature drops significantly
        CHEnabled = 1;
    }
}

void handleNotFound() {
    digitalWrite(BUILTIN_LED, 1);
    String message = "File Not Found\n\n";
    message += "URI: ";
    message += server.uri();
    message += "\nMethod: ";
    message += (server.method() == HTTP_GET) ? "GET" : "POST";
    message += "\nArguments: ";
    message += server.args();
    message += "\n";
    for (uint8_t i = 0; i < server.args(); i++) {
        message += " " + server.argName(i) + ": " + server.arg(i) + "\n";
    }
    server.send(404, "text/plain", message);
    digitalWrite(BUILTIN_LED, 0);
}

String getInfo() {
    unsigned long timestamp = millis();
    char uptime[32];
    unsigned long secs = timestamp / 1000, mins = secs / 60;
    unsigned int hours = mins / 60, days = hours / 24;
    timestamp -= secs * 1000;
    secs -= mins * 60;
    mins -= hours * 60;
    hours -= days * 24;
    sprintf(uptime, "%dd %02d:%02d:%02d", (byte)days, (byte)hours, (byte)mins, (byte)secs);

    String page = FPSTR(HTTP_INFO);
    page.replace("{0}", bitRead(boiler_status, 0) ? "checked=\"checked\"" : "");
    page.replace("{s}", CHEnabled ? "checked=\"checked\"" : "");
    page.replace("{d}", DirectBluetoothEnabled ? "checked=\"checked\"" : "");
    page.replace("{c}", CoolingEnabled ? "checked=\"checked\"" : "");
    page.replace("{6}", bitRead(boiler_status, 6) ? "checked=\"checked\"" : "");

    page.replace("{t}", String(ch_temperature));
    page.replace("{m}", String(modulation_level));
    page.replace("{r}", String(room_temperature));
    page.replace("{rp}", String(room_temperature <= 25 ? room_temperature * 4 : 25));
    page.replace("{rsp}", String(room_setpoint));
    page.replace("{u}", uptime);
    return page;
}

void handleInfo() {
    Serial.println(">>> Info request");
    String tStr = server.arg("value");
    if (!tStr.isEmpty())
    {
        float t = tStr.toFloat();
        Serial.println(">>> Room Setpoint request: " + String(t));
        updateRoomSetpointRequest(t);
    }
    server.send(200, "text/html", getInfo() + getChart());
}

void handleRoot() {
    String page = FPSTR(HTTP_HEAD_BEGIN);
    page.replace("{v}", "Wi-Fi Thermostat");
    page += FPSTR(HTTP_STYLE);
    page += "<script>";
    page += FPSTR(HTTP_SCRIPT_VARS);
    page += FPSTR(HTTP_SCRIPT_START_DRAG);
    page += FPSTR(HTTP_SCRIPT_DO_DRAG);
    page += FPSTR(HTTP_SCRIPT_STOP_DRAG);
    page += FPSTR(HTTP_SCRIPT_SEND_STATUS);
    page += FPSTR(HTTP_SCRIPT_ATTACH_EVENTS);
    page += FPSTR(HTTP_SCRIPT_UPDATE);
    page += "</script>";
    page += FPSTR(HTTP_HEAD_END);
    page += F("<h1>Wi-Fi Thermostat</h1><div id=\"info\">");
    page += getInfo() + getChart();
    page += F("</div>");
    page += FPSTR(HTTP_END);
    server.send(200, "text/html", page);
}

void handleMessage() {
    byte type = server.arg("type").toInt() ? 1 : 0;
    byte id = server.arg("id").toInt();
    unsigned int data = server.arg("data").toInt();
    unsigned long msg = ot.buildRequest((OpenThermMessageType)type, (OpenThermMessageID)id, data);
    unsigned long response = ot.sendRequest(msg);
    Serial.println("Msg: type=" + String(type) + ", id=" + String(id) + ", data=" + String(data) + " Req: " + String(msg) + " Resp: " + String(response));
    server.send(200, "text/html", String(response));
}

void updateStatus(byte status) {
    CHEnabled = (status & MASTER_STATUS_CH_ENABLED) ? 1 : 0;
    DirectBluetoothEnabled = (status & DIRECT_BLUETOOTH_ENABLED) ? 1 : 0;
    CoolingEnabled = (status & MASTER_STATUS_COOLING_ENABLED) ? 1 : 0;
}

void handleStatus() {
    byte status = server.arg("value").toInt();
    Serial.println(">>> Status request: " + String(status, BIN));
    updateStatus(status);
    server.send(200, "text/html", String("ok"));
}

void handleHysteresis() {
    hysteresis = !hysteresis;
    server.send(200, "text/html", String("ok"));
}

void handleMetrics() {
    String response = "";
    response += "boiler_setpoint " + String(ch_setpoint) + "\n";
    response += "boiler_temperature " + String(ch_temperature) + "\n";
    response += "modulation_level " + String(modulation_level) + "\n";
    response += "room_setpoint " + String(room_setpoint) + "\n";
    response += "room_temperature " + String(room_temperature) + "\n";
    response += "boiler_status " + String(boiler_status) + "\n";
    response += "ch_enabled " + String(CHEnabled) + "\n";
    response += "pid_p " + String(P) + "\n";
    response += "pid_i " + String(I) + "\n";
    response += "pid_d " + String(D) + "\n";
    response += "pid_output " + String(output) + "\n";

    server.send(200, "text/plain", response);
}

void resetErrors() {
  P = 0.0;
  I = 0.0;
  integral = 0.0;
  D = 0.0;
  server.send(200, "text/html", String("ok"));
}

void updateCHTempRequest(float t) {
    if (t < 0) t = 0;
    if (t > 100) t = 100;
    ch_setpoint = t;
}

void handleCHTemp() {
    byte t = server.arg("value").toInt();
    Serial.println(">>> CH Temp request: " + String(t));
    updateCHTempRequest(t);
    server.send(200, "text/html", String("ok"));
}

void addChTemperature(String& out, char* buf)
{
    int i = 0;
    byte prev = 0;
    out += F("<rect x=\"0\" y=\"72\" width=\"100%\" height=\"100\" fill=\"#f4c63d\" fill-opacity=\"0.2\"/>");
    out += F("<path stroke=\"#f4c63d\" fill=\"#f4c63d\" fill-opacity=\"0.5\" d=\"M0,172 L0,172");
    for (i = 0; i < chart_items.getCount(); i++) {
        ChartItem* item = chart_items.peek(i);
        if (item->ch_temperature != prev) {
            sprintf(buf, ",%d,%d,%d,%d", i, 172 - prev, i, 172 - item->ch_temperature);
            prev = item->ch_temperature;
            out += buf;
        }
    }
    sprintf(buf, ",%d,%d,%d,%d,0,%d", i, 172 - prev, i, 172, 172);
    out += buf;
    out += "\" />";
}

void addModulation(String& out, char* buf)
{
    int i = 0;
    byte prev = 0;
    //out += "<rect x=\"0\" y=\"72\" width=\"100%\" height=\"100\" fill=\"#d17905\" fill-opacity=\"0.2\"/>";
    out += F("<path stroke=\"#f05b4f\" stroke-opacity=\"0.5\" fill=\"#d17905\" fill-opacity=\"0.2\" d=\"M0,172 L0,172");
    for (i = 0; i < chart_items.getCount(); i++) {
        ChartItem* item = chart_items.peek(i);
        if (item->modulation != prev) {
            sprintf(buf, ",%d,%d,%d,%d", i, 172 - prev, i, 172 - item->modulation);
            prev = item->modulation;
            out += buf;
        }
    }
    sprintf(buf, ",%d,%d,%d,%d,0,%d", i, 172 - prev, i, 172, 172);
    out += buf;
    out += "\" />";
}

void updateRoomSetpointRequest(float t) {
    if (t < 0) t = 0;
    if (t > 30) t = 30;
    room_setpoint = t;
}

void updateRoomTemperature(float t) {
    room_temperature = t;
}

void handleRoomSetpoint() {
    float t = server.arg("value").toFloat();
    Serial.println(">>> Room Setpoint request: " + String(t));
    updateRoomSetpointRequest(t);
    server.send(200, "text/html", String("ok"));
}

void handleRoomTemperature() {
  if (DirectBluetoothEnabled == 1) {
    Serial.println("WARNING: Please disable direct Bluetooth sensing before using this feature!");
  } else {
    float t = server.arg("value").toFloat();
    Serial.println(">>> Room temperature updated: " + String(t));
    updateRoomTemperature(t);
    server.send(200, "text/html", String("ok"));
  }
}

uint16_t getTChartValue(uint8_t t)
{
    float v = t / 10.0 - 5;
    v *= 4;
    return uint16_t(v);
}

void addRoomTemperature(String& out, char* buf)
{
    int i = 0;
    byte prev = 0;
    out += "<rect x=\"0\" y=\"186\" width=\"100%\" height=\"100\" fill=\"#59922b\" fill-opacity=\"0.2\"/>";
    out += F("<path stroke=\"#59922b\" fill=\"#59922b\"  fill-opacity=\"0.5\" d=\"M0,286 L0,286");
    for (i = 0; i < chart_items.getCount(); i++) {
        ChartItem* item = chart_items.peek(i);
        if (item->room_temperature != prev) {
            if (!prev) prev = item->room_temperature;
            sprintf(buf, ",%d,%d,%d,%d", i, 286 - getTChartValue(prev), i, 286 - getTChartValue(item->room_temperature));
            prev = item->room_temperature;
            out += buf;
        }
    }
    sprintf(buf, ",%d,%d,%d,%d,0,%d", i, 286 - getTChartValue(prev), i, 286, 286);
    out += buf;
    out += "\" />";
}

void addRoomSetpoint(String& out, char* buf)
{
    out += "<g id=\"roomSp\">";
    float sp = uint16_t((room_setpoint - 5) * 4);
    uint16_t spPos = 286 - sp;
    out += "<line id=\"roomL\" x1=\"0\" y1=\"" + String(spPos) + "\" x2=\"300\" y2=\"" + String(spPos) + "\" stroke=\"#453D3F\" stroke-dasharray=\"2,2\"/>";
    out += "<text id=\"roomTxt\" x=\"140\" y=\"" + String(spPos - 4) + "\" fill=\"#453D3F\" font-size=\"1.2em\" font-weight=\"bold\">" + String(room_setpoint) + " &#8451;</text>";
    out += "</g>";
}

String getChart() {
    String out = "";
    char buf[30];
    out += F("<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 300 300\" onmousemove=\"doDrag(evt)\" onmouseup=\"stopDrag(evt)\" onmouseleave=\"stopDrag(evt)\">");
    out += F("<rect width=\"100%\" height=\"100%\" fill=\"white\" stroke=\"#eee\" />");
    out += F("<g stroke=\"none\" fill=\"#aaa\" font-size=\"10\">");

    byte bits[] = { 3,1,2 };
    String colors[] = { "#d17905", "#f05b4f", "#0544d3" };
    String titles[] = { "Flame", "Central Heating", "Domestic Hot Water" };
    int i;
    byte prev;
    for (byte b = 0; b < 3; b++) {
        int y = 10 + b * 24;
        prev = 0;
        out += "<rect x=\"0\" y=\"" + String(y - 10) + "\" width=\"100%\" height=\"10\" fill=\"" + colors[b] + "\" fill-opacity=\"0.2\"/>";
        out += "<path stroke=\"" + colors[b] + "\" fill=\"" + colors[b] + "\" d=\"M0," + String(y) + " L0," + String(y);
        for (i = 0; i < chart_items.getCount(); i++) {
            ChartItem* item = chart_items.peek(i);
            byte val = bitRead(item->status, bits[b]);
            if (val != prev) {
                sprintf(buf, ",%d,%d,%d,%d", i, y - (prev ? 10 : 0), i, y - (val ? 10 : 0));
                prev = val;
                out += buf;
            }
        }
        sprintf(buf, ",%d,%d,%d,%d,0,%d", i, y - (prev ? 10 : 0), i, y, y);
        out += buf;

        out += "\" />";
        out += "<text x=\"0\" y=\"" + String(y + 10) + "\" fill=\"" + colors[b] + "\">" + titles[b] + "</text>";

    }


    out += "<g id=\"chT\" onmousedown=\"startDrag(evt)\">";
    addChTemperature(out, buf);
    addModulation(out, buf);
    //CH SP
    /*
    out += "<g id=\"chSp\">";
    byte sp = (requests[set_ch_temp_req_idx] & 0xFFFF) / 256.0;
    byte spPos = 172 - sp;
    out += "<line id=\"chL\" x1=\"0\" y1=\"" + String(spPos) + "\" x2=\"300\" y2=\"" + String(spPos) + "\" stroke=\"#f05b4f\" stroke-dasharray=\"2,2\"/>";
    out += "<text id=\"chTxt\" x=\"140\" y=\"" + String(spPos - 4) + "\" fill=\"#f05b4f\">" + String(sp) + " &#8451;</text>";
    out += "</g>";
    */
    out += "</g>";
    out += F("<text x=\"0\" y=\"182\">");
    out += F("<tspan fill=\"#f4c63d\">CH Temp</tspan>");
    out += F("<tspan fill=\"#d17905\">/Modulation</tspan>");
    out += "</text>";


    out += "<g id=\"roomT\" onmousedown=\"startDrag(evt)\">";
    addRoomTemperature(out, buf);
    addRoomSetpoint(out, buf);
    out += "</g>";
    out += F("<text x=\"0\" y=\"296\" fill=\"#59922b\">Room Thermperature</text>");


    out += "</g>";

    //Time grid
    out += "<g stroke = \"black\" stroke-dasharray=\"4,8\" stroke-opacity=\"0.2\">";
    for (i = 0; i < chart_items.getCount(); i++) {
        ChartItem* item = chart_items.peek(i);
        if (item->marked) {
            out += "<line x1=\"" + String(i) + "\" y1=\"0\" x2=\"" + String(i) + "\" y2=\"300\" />";
        }
    }
    out += "</g>";

    out += "</svg>";
    return out;
}

void handleChart() {
    Serial.println(">>> Chart request");
    server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    server.sendHeader("Pragma", "no-cache");
    server.sendHeader("Expires", "-1");
    server.send(200, "image/svg+xml", getChart());
}


void setup(void) {
    pinMode(BUILTIN_LED, OUTPUT);
    digitalWrite(BUILTIN_LED, 0);
    Serial.begin(115200);
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);
    Serial.println("");

    // Wait for connection
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    configTime(0, 0, ntpServer);

    Serial.println("");
    Serial.print("Connected to ");
    Serial.println(ssid);
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());

    if (MDNS.begin("thermostat")) {
        Serial.println("MDNS responder started");
    }

    server.on("/", handleRoot);
    server.on("/info", handleInfo);
    server.on("/chart.svg", handleChart);
    server.on("/msg", handleMessage);
    server.on("/status", handleStatus);
    server.on("/metrics", handleMetrics);
    server.on("/reset", resetErrors);
    server.on("/hysteresis", handleHysteresis);
    server.on("/ch_temp", handleCHTemp);
    server.on("/room_setpoint", handleRoomSetpoint);
    server.on("/room_temperature", handleRoomTemperature);
    server.on("/test", []() {
        server.send(200, "text/plain", "this works as well");
        });

    server.onNotFound(handleNotFound);

    server.begin();
    Serial.println("HTTP server started");

    ot.begin(handleInterrupt, processResponse);
    marked_min = millis() / 60000;

    miThermometer.begin();
    room_temperature = getTemperature();
    ts = millis();
}


void processResponse(unsigned long response, OpenThermResponseStatus status) {
    if (!ot.isValidResponse(response)) {
        Serial.println("Invalid response: " + String(response, HEX) + ", status=" + OpenTherm::statusToString(ot.getLastResponseStatus()));
        return;
    }
    if (curr_item == NULL) {
        Serial.println("Failed to process response: " + String(response, HEX));
        return;
    }
    float t;
    byte id = (response >> 16 & 0xFF);
    switch ((OpenThermMessageID)id)
    {
    case OpenThermMessageID::Status:
        boiler_status = response & 0xFF;
        curr_item->status = boiler_status;
        Serial.println("Boiler status: " + String(boiler_status, BIN));
        break;
    case OpenThermMessageID::TSet:
        t = (response & 0xFFFF) / 256.0;
        updateCHTempRequest(t);
        Serial.println("Set CH temp: " + String(t));
        break;
    case OpenThermMessageID::Tboiler:
        ch_temperature = (response & 0xFFFF) / 256.0;
        curr_item->ch_temperature = ch_temperature;
        Serial.println("CH temp: " + String(ch_temperature));
        break;
    case OpenThermMessageID::RelModLevel:
        modulation_level = (response & 0xFFFF) / 256.0;
        curr_item->modulation = modulation_level;
        Serial.println("Modulation level: " + String(modulation_level));
        break;
    default:
        Serial.println("Response: " + String(response, HEX) + ", id=" + String(id));
    }
}

void clearItem(ChartItem* item)
{
    curr_item->status = 0;
    curr_item->ch_temperature = 0;
    curr_item->room_temperature = 0;
    curr_item->modulation = 0;
}

unsigned int buildRequest(byte req_idx)
{
    uint16_t status;
    OpenThermMessageID id = requests[req_idx];
    switch (id)
    {
    case OpenThermMessageID::Status:
        status = 0;
        if (CHEnabled) status |= MASTER_STATUS_CH_ENABLED;
        status <<= 8;
        return ot.buildRequest(OpenThermMessageType::READ, OpenThermMessageID::Status, status);
    case OpenThermMessageID::TSet:
        return ot.buildRequest(OpenThermMessageType::WRITE, OpenThermMessageID::TSet, ((uint16_t)ch_setpoint) << 8);
    case OpenThermMessageID::Tboiler:
        return ot.buildRequest(OpenThermMessageType::READ, OpenThermMessageID::Tboiler, 0);
    case OpenThermMessageID::RelModLevel:
        return ot.buildRequest(OpenThermMessageType::READ, OpenThermMessageID::RelModLevel, 0);
    }
    return 0;
}

uint8_t normalizeTemperatureForChart(float t)
{
    if (t < 5) t = 5;
    if (t > 30) t = 30;
    return uint8_t(t * 10);
}


void handleOpenTherm() {
    if (ot.isReady()) {
        if (curr_item == NULL || (req_idx == 0)) {
            curr_item = chart_items.push();
            clearItem(curr_item);
            curr_item->room_temperature = normalizeTemperatureForChart(room_temperature);
            unsigned long min = millis() / 60000;
            if (min > marked_min) {
                marked_min = min;
                curr_item->marked = true;
            }
            else {
                curr_item->marked = false;
            }
        }
        if (curr_item == NULL) return;

        unsigned int request = buildRequest(req_idx);
        ot.sendRequestAync(request);
        //Serial.println("Request: " + String(request, HEX));
        req_idx++;
        if (req_idx >= requests_count) {
            req_idx = 0;
        }
    }
    ot.process();
}

void loop(void) {
    epochTime = getTime();
    handleOpenTherm();
    server.handleClient();

    new_ts = millis();
    new_ble_ts = new_ts;
    if (new_ble_ts - ble_ts > ble_poll_frequency * 1000) {
        room_temperature = getTemperature();
        ble_ts = new_ble_ts;
      }
    if (new_ts - ts > 1000) {
        float dt = (new_ts - ts) / 1000.0;
        ts = new_ts;
        ch_setpoint = computeBoilerWaterSetpoint(room_setpoint, room_temperature, dt);
        if (hysteresis) updateBoilerState(room_setpoint, room_temperature);
    }
}
