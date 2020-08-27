# iConnectivity Mio10 remote controller for ESP8266

Uses ESP8266 as a web server to provide UI (Vue.js/Vuetify) for remote controlling MIDI connections and routings in iConnectivity Mio 10 MIDI interface. MIDI communication uses RTP MIDI (wireless) so Arduino AppleMidi library is required (https://github.com/lathoub/Arduino-AppleMIDI-Library)

## Setup 

To build/setup project one file is needed to configure the local WLAN secrets:

Add file **WifiConfig.h**:

```c
#define WIFI_SSID "my_wifi_ssid" // your network SSID (name)
#define WIFI_PASSWORD "password" // your network password
```

Currently the routing coniguration is done by editing ```index.html``` 