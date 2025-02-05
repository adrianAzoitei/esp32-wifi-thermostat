# OpenTherm Thermostat for Wemos D1 Mini ESP32

_NOTE_: This project uses the OpenTherm shield and starts from the example PID-based implementation from [DIYLESS](https://diyless.com).

## Secrets management

Use a `secrets.h` header file in the Sketch directory to pass the WiFi SSID and password secrets. For example:

```cpp
#ifndef SECRETS_H
#define SECRETS_H

#define WIFI_SSID "MyWiFi"
#define WIFI_PASSWORD "MyPassword"
#define MI_MAC_ADDR "xx:xx:xx:xx:xx:xx"
#define URL "192.168.178.209"
#define PATH "/api/v1/write"
#define PORT 30090

#endif
```
