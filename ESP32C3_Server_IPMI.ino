#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include <esp_system.h>

// ============================================================
// ESP32-C3 SUPERMINI - PC REMOTE
// DIRECT OPEN-DRAIN PWR_SW
// ============================================================
//
// Verdrahtung:
//
// Mainboard PWR_SW Signal (~3.4V)
//          |
//         1k
//          |
//        GPIO4
//
// Mainboard PWR_SW GND -------- ESP GND
//
// GPIO4:
// HIGH = Open Drain hochohmig = Taster offen
// LOW  = gegen GND             = Taster gedrueckt
//
// ============================================================

const uint8_t POWER_PIN = 4;
const uint32_t POWER_PRESS_MS = 500;
const uint32_t HARD_OFF_MS = 5500;

const char* SETUP_PASSWORD = "12345678";

IPAddress setupIP(192, 168, 4, 1);
IPAddress setupGateway(192, 168, 4, 1);
IPAddress setupSubnet(255, 255, 255, 0);

const uint32_t WIFI_CONNECT_TIMEOUT_MS = 20000;
const uint32_t WIFI_RECONNECT_INTERVAL_MS = 10000;

WebServer server(80);
DNSServer dnsServer;
Preferences preferences;

String wifiSSID = "";
String wifiPassword = "";
String hostname = "pc-switch";

bool useStaticIP = false;
String staticIPString = "";
String gatewayString = "";
String subnetString = "255.255.255.0";
String dnsString = "";

bool forceSetup = false;

// Optional login.
// Factory defaults: disabled, admin / 1234.
bool loginEnabled = false;
String loginUser = "admin";
String loginPassword = "1234";

String sessionToken;
String actionToken;

bool setupMode = false;
bool wifiWasConnected = false;
bool mdnsRunning = false;
uint32_t lastReconnectAttempt = 0;

bool powerPulseActive = false;
uint32_t powerPulseEnd = 0;
String powerAction = "IDLE";

bool rebootRequested = false;
uint32_t rebootAt = 0;

String htmlEscape(String value) {
  value.replace("&", "&amp;");
  value.replace("<", "&lt;");
  value.replace(">", "&gt;");
  value.replace("\"", "&quot;");
  value.replace("'", "&#39;");
  return value;
}

String jsonEscape(String value) {
  value.replace("\\", "\\\\");
  value.replace("\"", "\\\"");
  value.replace("\n", "\\n");
  value.replace("\r", "\\r");
  return value;
}

bool validHostname(String value) {
  if (value.length() < 1 || value.length() > 32) return false;
  if (value[0] == '-' || value[value.length() - 1] == '-') return false;

  for (size_t i = 0; i < value.length(); i++) {
    char c = value[i];
    bool ok =
      (c >= 'a' && c <= 'z') ||
      (c >= 'A' && c <= 'Z') ||
      (c >= '0' && c <= '9') ||
      c == '-';

    if (!ok) return false;
  }

  return true;
}

String createRandomToken() {
  char buffer[20];

  snprintf(
    buffer,
    sizeof(buffer),
    "%08lX%08lX",
    (unsigned long)esp_random(),
    (unsigned long)esp_random()
  );

  return String(buffer);
}

// ============================================================
// OPEN-DRAIN POWER OUTPUT
// ============================================================

void releasePowerButton() {
  // HIGH with OUTPUT_OPEN_DRAIN means high impedance.
  digitalWrite(POWER_PIN, HIGH);
}

void pressPowerButtonElectrical() {
  // LOW pulls PWR_SW to ground.
  digitalWrite(POWER_PIN, LOW);
}

void initPowerOutput() {
  // Set HIGH before enabling the output mode to avoid a boot pulse.
  digitalWrite(POWER_PIN, HIGH);
  pinMode(POWER_PIN, OUTPUT_OPEN_DRAIN);
  releasePowerButton();

  Serial.println("[POWER] GPIO4 OPEN-DRAIN / RELEASED");
}

bool startPowerPulse(uint32_t duration, String action) {
  if (powerPulseActive) return false;

  powerPulseActive = true;
  powerAction = action;

  pressPowerButtonElectrical();
  powerPulseEnd = millis() + duration;

  Serial.print("[POWER] ");
  Serial.print(action);
  Serial.print(" / ");
  Serial.print(duration);
  Serial.println(" ms");

  return true;
}

void updatePowerPulse() {
  if (!powerPulseActive) return;

  if ((int32_t)(millis() - powerPulseEnd) >= 0) {
    releasePowerButton();
    powerPulseActive = false;
    powerAction = "IDLE";
    Serial.println("[POWER] RELEASED");
  }
}

// ============================================================
// CONFIGURATION
// ============================================================

void loadConfiguration() {
  preferences.begin("pcswitch", true);

  wifiSSID = preferences.getString("ssid", "");
  wifiPassword = preferences.getString("wifipass", "");

  // Compatibility with an older test version.
  if (wifiPassword.length() == 0) {
    wifiPassword = preferences.getString("wpass", "");
  }

  hostname = preferences.getString("hostname", "");

  if (hostname.length() == 0) {
    hostname = preferences.getString("host", "pc-switch");
  }

  useStaticIP = preferences.getBool("static", false);
  staticIPString = preferences.getString("ip", "");
  gatewayString = preferences.getString("gw", "");
  subnetString = preferences.getString("mask", "255.255.255.0");
  dnsString = preferences.getString("dns", "");
  forceSetup = preferences.getBool("force", false);

  loginEnabled = preferences.getBool("auth", false);
  loginUser = preferences.getString("auser", "admin");
  loginPassword = preferences.getString("apass", "1234");

  preferences.end();

  Serial.println();
  Serial.println("=== CONFIG ===");
  Serial.print("SSID: ");
  Serial.println(wifiSSID.length() ? wifiSSID : "<keine>");
  Serial.print("Hostname: ");
  Serial.println(hostname);
  Serial.print("IP Mode: ");
  Serial.println(useStaticIP ? "STATIC" : "DHCP");
  Serial.print("Login: ");
  Serial.println(loginEnabled ? "ENABLED" : "DISABLED");
  Serial.println("==============");
}

// ============================================================
// WIFI
// ============================================================

void applyC3WiFiFix() {
  // Some ESP32-C3 SuperMini boards behave more reliably with
  // reduced Wi-Fi transmit power.
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
}

bool configureStaticIP() {
  if (!useStaticIP) return true;

  IPAddress ip;
  IPAddress gateway;
  IPAddress subnet;
  IPAddress dns;

  if (!ip.fromString(staticIPString.c_str())) return false;
  if (!gateway.fromString(gatewayString.c_str())) return false;
  if (!subnet.fromString(subnetString.c_str())) return false;

  if (dnsString.length() > 0) {
    if (!dns.fromString(dnsString.c_str())) return false;
  } else {
    dns = gateway;
  }

  return WiFi.config(ip, gateway, subnet, dns);
}

bool connectToHomeWiFi() {
  if (wifiSSID.length() == 0) return false;

  Serial.println();
  Serial.println("==============================");
  Serial.println("VERBINDE MIT WLAN");
  Serial.print("SSID: ");
  Serial.println(wifiSSID);
  Serial.println("==============================");

  WiFi.mode(WIFI_MODE_NULL);
  delay(250);

  WiFi.setHostname(hostname.c_str());

  WiFi.mode(WIFI_STA);
  delay(150);

  applyC3WiFiFix();

  if (useStaticIP && !configureStaticIP()) {
    Serial.println("[WiFi] STATIC IP CONFIG INVALID");
    WiFi.mode(WIFI_MODE_NULL);
    return false;
  }

  WiFi.begin(wifiSSID.c_str(), wifiPassword.c_str());

  uint32_t start = millis();

  Serial.print("[WiFi] ");

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");

    if (millis() - start >= WIFI_CONNECT_TIMEOUT_MS) {
      Serial.println();
      Serial.println("[WiFi] CONNECTION FAILED");

      WiFi.disconnect();
      delay(100);
      WiFi.mode(WIFI_MODE_NULL);
      return false;
    }
  }

  wifiWasConnected = true;

  Serial.println();
  Serial.println();
  Serial.println("WLAN VERBUNDEN");
  Serial.print("IP: ");
  Serial.println(WiFi.localIP());
  Serial.print("RSSI: ");
  Serial.println(WiFi.RSSI());

  return true;
}

void startMDNS() {
  if (mdnsRunning || WiFi.status() != WL_CONNECTED) return;

  if (MDNS.begin(hostname.c_str())) {
    MDNS.addService("http", "tcp", 80);

    mdnsRunning = true;

    Serial.print("[mDNS] http://");
    Serial.print(hostname);
    Serial.println(".local");
  }
}

void updateWiFiConnection() {
  if (setupMode) return;

  bool connected = WiFi.status() == WL_CONNECTED;

  if (connected && !wifiWasConnected) {
    wifiWasConnected = true;
    mdnsRunning = false;
    startMDNS();
    return;
  }

  if (!connected && wifiWasConnected) {
    wifiWasConnected = false;
    mdnsRunning = false;
    Serial.println("[WiFi] CONNECTION LOST");
  }

  if (connected) return;

  if (millis() - lastReconnectAttempt >= WIFI_RECONNECT_INTERVAL_MS) {
    lastReconnectAttempt = millis();
    Serial.println("[WiFi] RECONNECT");
    applyC3WiFiFix();
    WiFi.reconnect();
  }
}

void startSetupAccessPoint() {
  setupMode = true;

  WiFi.mode(WIFI_MODE_NULL);
  delay(250);

  WiFi.mode(WIFI_AP);
  delay(150);

  WiFi.setTxPower(WIFI_POWER_8_5dBm);

  WiFi.softAPConfig(
    setupIP,
    setupGateway,
    setupSubnet
  );

  uint64_t chipID = ESP.getEfuseMac();

  char apName[32];

  snprintf(
    apName,
    sizeof(apName),
    "PC-Switch-%06X",
    (uint32_t)(chipID & 0xFFFFFF)
  );

  WiFi.softAP(
    apName,
    SETUP_PASSWORD
  );

  Serial.println();
  Serial.println("=== SETUP MODE ===");
  Serial.print("SSID: ");
  Serial.println(apName);
  Serial.print("Passwort: ");
  Serial.println(SETUP_PASSWORD);
  Serial.println("URL: http://192.168.4.1");

  dnsServer.start(
    53,
    "*",
    setupIP
  );
}

// ============================================================
// SESSION / AUTH
// ============================================================

bool sessionValid() {
  if (!loginEnabled || setupMode) return true;

  String cookie = server.header("Cookie");
  String wanted = "pcsid=" + sessionToken;

  return cookie.indexOf(wanted) >= 0;
}

void redirectToLogin() {
  server.sendHeader("Location", "/login");
  server.send(303, "text/plain", "");
}

bool requirePageAuth() {
  if (sessionValid()) return true;
  redirectToLogin();
  return false;
}

void sendJSON(int status, String json) {
  server.sendHeader("Cache-Control", "no-store");
  server.send(
    status,
    "application/json; charset=utf-8",
    json
  );
}

bool requireAPIAuth() {
  if (sessionValid()) return true;

  sendJSON(
    401,
    "{\"ok\":false,\"message\":\"Login required\"}"
  );

  return false;
}

bool checkActionToken() {
  return
    server.hasArg("token") &&
    server.arg("token") == actionToken;
}

// ============================================================
// LOGIN PAGE
// ============================================================

String loginPage(bool failed = false) {
  String html = R"rawliteral(
<!doctype html>
<html lang="de">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>PC Control Login</title>
<style>
*{box-sizing:border-box}
html,body{margin:0;width:100%;height:100%}
body{display:grid;place-items:center;padding:20px;background:radial-gradient(circle at 50% 0%,#1e395f,#09111d 48%,#030609);color:#eaf2ff;font-family:ui-monospace,Menlo,Consolas,monospace}
.login{width:min(400px,100%);padding:28px;background:#101a28;border:1px solid #344a66;border-radius:18px;box-shadow:0 30px 80px #000a}
.kicker{color:#5f7899;font-size:10px;letter-spacing:.17em}
h1{margin:6px 0 26px;font:900 34px Arial,sans-serif}
label{display:block;margin:16px 0 6px;color:#8095b2;font-size:11px}
input{width:100%;padding:13px;outline:0;border:1px solid #3c506d;border-radius:9px;background:#060b12;color:white;font:inherit}
input:focus{border-color:#4e91ef;box-shadow:0 0 0 3px #2d72d52a}
button{width:100%;margin-top:22px;padding:14px;border:1px solid #65a6ff;border-radius:10px;background:linear-gradient(#347ee7,#1956b2);color:white;font:800 15px Arial,sans-serif;cursor:pointer}
.error{margin-top:16px;padding:10px;border:1px solid #672626;border-radius:8px;background:#251010;color:#fca5a5;font-size:11px}
</style>
</head>
<body>
<form class="login" method="POST" action="/login">
<div class="kicker">ATX // SECURE TERMINAL</div>
<h1>PC CONTROL</h1>
<label>USER</label>
<input name="user" autocomplete="username" autofocus>
<label>PASSWORD</label>
<input name="pass" type="password" autocomplete="current-password">
<button type="submit">UNLOCK</button>
)rawliteral";

  if (failed) {
    html += R"rawliteral(
<div class="error">LOGIN FAILED</div>
)rawliteral";
  }

  html += R"rawliteral(
</form>
</body>
</html>
)rawliteral";

  return html;
}

// ============================================================
// SETTINGS PAGE
// ============================================================

String settingsPage() {
  String authChecked = loginEnabled ? "checked" : "";
  String staticChecked = useStaticIP ? "checked" : "";

  String html = R"rawliteral(
<!doctype html>
<html lang="de">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>PC Control Settings</title>
<style>
*{box-sizing:border-box}
body{margin:0;padding:18px;min-height:100vh;background:radial-gradient(circle at top,#172842,#070c14 60%);color:#e5efff;font-family:ui-monospace,Menlo,Consolas,monospace}
.panel{width:min(620px,100%);margin:10px auto;padding:24px;background:#101927;border:1px solid #344761;border-radius:16px;box-shadow:0 25px 70px #0009}
.top{display:flex;justify-content:space-between;align-items:center;gap:15px}
.kicker{color:#6680a1;font-size:9px;letter-spacing:.17em}
h1{margin:5px 0 0;font:900 30px Arial,sans-serif}
.back{padding:9px 12px;border:1px solid #344a66;border-radius:8px;color:#9bb6d8;text-decoration:none;font-size:11px}
fieldset{margin:24px 0;padding:18px;border:1px solid #30425b;border-radius:12px}
legend{padding:0 8px;color:#6ca7f7;font-size:11px;letter-spacing:.1em}
label{display:block;margin-top:15px;margin-bottom:5px;color:#879ab6;font-size:11px}
input{width:100%;padding:12px;background:#070c14;color:white;border:1px solid #3c506d;border-radius:8px;font:inherit}
.check{display:flex;align-items:center;gap:10px;margin:10px 0}
.check input{width:auto}
.note{margin-top:6px;color:#62758f;font-size:10px;line-height:1.45}
.two{display:grid;grid-template-columns:1fr 1fr;gap:12px}
.save,.ap,.reset{width:100%;padding:14px;border-radius:9px;font-weight:bold;cursor:pointer}
.save{border:1px solid #62a4ff;background:linear-gradient(#337ee8,#1856b4);color:white}
.ap{margin-top:12px;border:1px solid #47617f;background:#152235;color:#b3c9e6}
.danger{margin-top:30px;padding:18px;border:1px solid #592424;border-radius:12px;background:#1a0c0c}
.dangerTitle{color:#f87171;font:800 15px Arial,sans-serif}
.reset{margin-top:15px;border:1px solid #f16a6a;background:linear-gradient(#ce3636,#8c1414);color:white}
.hidden{display:none}
@media(max-width:560px){.two{grid-template-columns:1fr}}
</style>
</head>
<body>
<div class="panel">
<div class="top">
<div>
<div class="kicker">ESP32-C3 // CONFIG</div>
<h1>SETTINGS</h1>
</div>
)rawliteral";

  if (!setupMode) {
    html += R"rawliteral(
<a class="back" href="/">BACK</a>
)rawliteral";
  }

  html += R"rawliteral(
</div>

<form method="POST" action="/save">
<input type="hidden" name="token" value=")rawliteral";

  html += actionToken;

  html += R"rawliteral(">

<fieldset>
<legend>NETWORK</legend>

<label>SSID</label>
<input name="ssid" value=")rawliteral";

  html += htmlEscape(wifiSSID);

  html += R"rawliteral(" required>

<label>WLAN PASSWORD</label>
<input type="password" name="wifipass" autocomplete="new-password">
<div class="note">Leer lassen = bestehendes WLAN-Passwort behalten.</div>

<label class="check">
<input id="staticCheck" type="checkbox" name="static" value="1" )rawliteral";

  html += staticChecked;

  html += R"rawliteral(>
STATIC IP
</label>

<div id="staticFields" class="hidden">
<div class="two">
<div>
<label>IP</label>
<input name="ip" value=")rawliteral";

  html += htmlEscape(staticIPString);

  html += R"rawliteral(" placeholder="192.168.1.50">
</div>
<div>
<label>GATEWAY</label>
<input name="gw" value=")rawliteral";

  html += htmlEscape(gatewayString);

  html += R"rawliteral(" placeholder="192.168.1.1">
</div>
</div>

<div class="two">
<div>
<label>SUBNET</label>
<input name="mask" value=")rawliteral";

  html += htmlEscape(subnetString);

  html += R"rawliteral(">
</div>
<div>
<label>DNS</label>
<input name="dns" value=")rawliteral";

  html += htmlEscape(dnsString);

  html += R"rawliteral(" placeholder="leer = Gateway">
</div>
</div>
</div>
</fieldset>

<fieldset>
<legend>DEVICE</legend>
<label>HOSTNAME</label>
<input name="hostname" value=")rawliteral";

  html += htmlEscape(hostname);

  html += R"rawliteral(" required>
</fieldset>

<fieldset>
<legend>LOGIN</legend>

<label class="check">
<input id="authCheck" type="checkbox" name="auth" value="1" )rawliteral";

  html += authChecked;

  html += R"rawliteral(>
LOGIN-SCHUTZ AKTIVIEREN
</label>

<div id="authFields">
<label>USER</label>
<input name="loginuser" value=")rawliteral";

  html += htmlEscape(loginUser);

  html += R"rawliteral(">

<label>PASSWORD</label>
<input type="password" name="loginpass" placeholder="leer = bestehendes Passwort" autocomplete="new-password">
<div class="note">Standard nach Factory Reset: User <b>admin</b>, Passwort <b>1234</b>.</div>
</div>
</fieldset>

<button class="save" type="submit">SAVE CONFIG + REBOOT</button>
</form>

<button id="setupAP" class="ap" type="button">START OWN SETUP WIFI</button>

<div class="danger">
<div class="dangerTitle">FACTORY RESET</div>
<div class="note">Loescht WLAN, Static-IP, Login und alle sonstigen Einstellungen. Danach startet wieder PC-Switch-XXXXXX.</div>
<button id="factoryReset" class="reset" type="button">ERASE EVERYTHING</button>
</div>
</div>

<script>
const TOKEN = ")rawliteral";

  html += actionToken;

  html += R"rawliteral(";

const staticCheck=document.getElementById('staticCheck');
const staticFields=document.getElementById('staticFields');
const authCheck=document.getElementById('authCheck');
const authFields=document.getElementById('authFields');

function updateForms(){
  staticFields.classList.toggle('hidden',!staticCheck.checked);
  authFields.style.opacity=authCheck.checked?'1':'.35';
}

staticCheck.addEventListener('change',updateForms);
authCheck.addEventListener('change',updateForms);
updateForms();

async function postAction(url){
  const body=new URLSearchParams();
  body.set('token',TOKEN);

  const response=await fetch(url,{
    method:'POST',
    headers:{'Content-Type':'application/x-www-form-urlencoded'},
    body
  });

  const data=await response.json();

  if(!response.ok||!data.ok){
    throw new Error(data.message||'request failed');
  }

  return data;
}

document.getElementById('setupAP').addEventListener('click',async()=>{
  if(!confirm('ESP in eigenes Setup-WLAN neu starten?')) return;

  try{
    await postAction('/api/setup');
    alert('ESP startet neu. Danach mit PC-Switch-XXXXXX verbinden. Passwort: 12345678');
  }catch(error){
    alert(error.message);
  }
});

document.getElementById('factoryReset').addEventListener('click',async()=>{
  if(!confirm('Wirklich ALLE Einstellungen loeschen?')) return;
  if(!confirm('Letzte Warnung: WLAN + Login + IP-Konfiguration werden geloescht.')) return;

  try{
    await postAction('/api/factory-reset');
    alert('Factory Reset ausgefuehrt. Danach mit PC-Switch-XXXXXX verbinden. Passwort: 12345678');
  }catch(error){
    alert(error.message);
  }
});
</script>
</body>
</html>
)rawliteral";

  return html;
}

// ============================================================
// MAIN CONTROL PAGE
// ============================================================

String controlPage() {
  String html = R"rawliteral(
<!doctype html>
<html lang="de">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover,user-scalable=no">
<title>PC Control</title>

<style>
:root{color-scheme:dark}
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent}
html,body{margin:0;width:100%;height:100%;overflow:hidden}
body{background:radial-gradient(circle at 50% -20%,#213e6a 0,#0b1524 42%,#03070d 100%);color:#eaf2ff;font-family:ui-monospace,Menlo,Consolas,monospace;user-select:none}
.app{width:min(760px,100%);height:100dvh;margin:auto;padding:max(10px,env(safe-area-inset-top)) 14px max(10px,env(safe-area-inset-bottom));display:grid;grid-template-rows:auto minmax(0,1fr) auto;gap:clamp(8px,1.4vh,14px)}
.header{min-height:48px;display:flex;align-items:center;justify-content:space-between;gap:12px}
.kicker{color:#637e9f;font-size:8px;letter-spacing:.18em}
.brand{margin-top:2px;font:900 clamp(25px,5vw,38px) Arial,sans-serif;letter-spacing:-.04em}
.headerRight{display:flex;align-items:center;gap:7px}
.net{display:flex;align-items:center;gap:7px;padding:8px 10px;border:1px solid #283b54;border-radius:99px;background:#07101a;color:#839ab8;font-size:9px}
.dot{width:8px;height:8px;border-radius:50%;background:#64748b}
.dot.online{background:#22c55e;box-shadow:0 0 12px #22c55e}
.dot.offline{background:#ef4444}
.lock{padding:8px 9px;border:1px solid #283b54;border-radius:8px;background:#0c1622;color:#8298b5;text-decoration:none;font-size:9px}
.main{min-height:0;display:grid;grid-template-rows:minmax(0,1.15fr) minmax(0,.85fr) auto;gap:clamp(8px,1.4vh,14px)}
.hw{position:relative;min-height:0;width:100%;padding:clamp(17px,4vw,30px);display:flex;align-items:center;justify-content:space-between;border:1px solid #62a6ff;border-radius:18px;background:linear-gradient(180deg,#357fe9,#1857b9);color:white;box-shadow:0 8px 0 #082e69,0 17px 34px #0008,inset 0 1px #ffffff55;cursor:pointer;touch-action:none;transition:transform .08s,box-shadow .08s,filter .15s}
.hw.active,.hw:active{transform:translateY(6px);box-shadow:0 2px 0 #082e69,0 7px 17px #0008,inset 0 1px #ffffff44}
.hw:disabled{filter:grayscale(.7);opacity:.5;cursor:default}
.hw.danger{border-color:#ef6868;background:linear-gradient(180deg,#df3b3b,#a41414);box-shadow:0 8px 0 #5b0909,0 17px 34px #0008,inset 0 1px #ffffff44}
.hw.danger.active,.hw.danger:active{box-shadow:0 2px 0 #5b0909,0 7px 17px #0008}
.icon{font:300 clamp(42px,10vw,72px) Arial,sans-serif;line-height:1}
.buttonText{text-align:right}
.buttonTitle{font:900 clamp(25px,6vw,42px) Arial,sans-serif;letter-spacing:-.03em}
.buttonSub{margin-top:5px;color:#d7e7ffbb;font-size:clamp(8px,1.8vw,11px);letter-spacing:.12em}
.holdTrack{position:absolute;left:18px;right:18px;bottom:12px;height:4px;overflow:hidden;border-radius:99px;background:#5d1010}
.holdFill{width:0;height:100%;background:white;box-shadow:0 0 8px white}
.feedback{min-height:42px;padding:11px 13px;display:flex;justify-content:space-between;align-items:center;gap:10px;border:1px solid #263950;border-radius:10px;background:#070e17;color:#7895b9;font-size:10px}
.feedback.ok{color:#86efac;border-color:#185332}
.feedback.error{color:#fca5a5;border-color:#712828}
.msg{overflow:hidden;white-space:nowrap;text-overflow:ellipsis}
.spinner{display:none;width:12px;height:12px;flex:0 0 12px;border:2px solid #344b67;border-top-color:#80b7ff;border-radius:50%;animation:spin .7s linear infinite}
.spinner.show{display:block}
@keyframes spin{to{transform:rotate(360deg)}}
.footer{display:grid;grid-template-columns:minmax(0,1fr) 52px;gap:8px}
.telemetry{min-width:0;height:52px;display:grid;grid-template-columns:1.45fr .75fr .9fr;border:1px solid #263950;border-radius:11px;overflow:hidden;background:#070e17}
.metric{min-width:0;padding:8px 10px;border-right:1px solid #1d2a3b}
.metric:last-child{border-right:0}
.key{color:#4d6380;font-size:7px;letter-spacing:.13em}
.value{margin-top:5px;overflow:hidden;white-space:nowrap;text-overflow:ellipsis;color:#b0cdf4;font-size:clamp(9px,2vw,12px)}
.settings{height:52px;display:grid;place-items:center;border:1px solid #304660;border-radius:11px;background:#0d1826;color:#8aa2c0;text-decoration:none;font-size:20px}
.settings:active{background:#18283c}

@media(max-height:600px){
  .app{gap:7px;padding-top:7px;padding-bottom:7px}
  .header{min-height:38px}
  .brand{font-size:24px}
  .main{gap:7px}
  .hw{padding:13px 20px;border-radius:13px}
  .icon{font-size:39px}
  .buttonTitle{font-size:24px}
  .feedback{min-height:36px}
  .telemetry,.settings{height:44px}
}
</style>
</head>

<body>

<div class="app">

<header class="header">

<div>
<div class="kicker">ATX // REMOTE TERMINAL</div>
<div class="brand">PC CONTROL</div>
</div>

<div class="headerRight">

<div class="net">
<div id="wifiDot" class="dot"></div>
<span id="wifiText">LINK</span>
</div>
)rawliteral";

  if (loginEnabled) {
    html += R"rawliteral(
<a class="lock" href="/logout">LOCK</a>
)rawliteral";
  }

  html += R"rawliteral(
</div>
</header>

<main class="main">

<button id="powerButton" class="hw">
<div class="icon">⏻</div>
<div class="buttonText">
<div class="buttonTitle">POWER</div>
<div class="buttonSub">500ms // PWR_SW</div>
</div>
</button>

<button id="hardButton" class="hw danger">
<div class="icon">!</div>
<div class="buttonText">
<div class="buttonTitle">HARD OFF</div>
<div class="buttonSub">HOLD 1.7s // THEN 5.5s</div>
</div>

<div class="holdTrack">
<div id="holdFill" class="holdFill"></div>
</div>
</button>

<div id="feedback" class="feedback">
<div id="feedbackText" class="msg">SYSTEM READY</div>
<div id="spinner" class="spinner"></div>
</div>

</main>

<footer class="footer">

<div class="telemetry">

<div class="metric">
<div class="key">IP</div>
<div id="ip" class="value">-</div>
</div>

<div class="metric">
<div class="key">RSSI</div>
<div id="rssi" class="value">-</div>
</div>

<div class="metric">
<div class="key">UPTIME</div>
<div id="uptime" class="value">-</div>
</div>

</div>

<a class="settings" href="/settings" title="Settings">⚙</a>

</footer>
</div>

<script>
const TOKEN = ")rawliteral";

  html += actionToken;

  html += R"rawliteral(";

const powerButton=document.getElementById('powerButton');
const hardButton=document.getElementById('hardButton');
const holdFill=document.getElementById('holdFill');
const feedback=document.getElementById('feedback');
const feedbackText=document.getElementById('feedbackText');
const spinner=document.getElementById('spinner');

let busy=false;
let hardTimer=null;

function vibrate(pattern){
  if(navigator.vibrate) navigator.vibrate(pattern);
}

function message(text,type='',loading=false){
  feedbackText.textContent=text;
  feedback.className='feedback '+type;
  spinner.classList.toggle('show',loading);
}

async function postAPI(url){
  const body=new URLSearchParams();
  body.set('token',TOKEN);

  const response=await fetch(url,{
    method:'POST',
    headers:{'Content-Type':'application/x-www-form-urlencoded'},
    body
  });

  const data=await response.json();

  if(response.status===401){
    location.href='/login';
    throw new Error('Login required');
  }

  if(!response.ok||!data.ok){
    throw new Error(data.message||'request failed');
  }

  return data;
}

function formatUptime(sec){
  sec=Number(sec)||0;

  const days=Math.floor(sec/86400);
  sec%=86400;

  const h=Math.floor(sec/3600);
  sec%=3600;

  const m=Math.floor(sec/60);
  const s=sec%60;

  if(days>0){
    return days+'d '+String(h).padStart(2,'0')+':'+String(m).padStart(2,'0');
  }

  return String(h).padStart(2,'0')+':'+String(m).padStart(2,'0')+':'+String(s).padStart(2,'0');
}

async function refresh(){
  try{
    const response=await fetch('/api/status',{cache:'no-store'});

    if(response.status===401){
      location.href='/login';
      return;
    }

    if(!response.ok) throw new Error();

    const data=await response.json();

    document.getElementById('ip').textContent=data.ip;
    document.getElementById('rssi').textContent=data.wifi?data.rssi+' dBm':'DOWN';
    document.getElementById('uptime').textContent=formatUptime(data.uptime);

    const dot=document.getElementById('wifiDot');
    const text=document.getElementById('wifiText');

    if(data.wifi){
      dot.className='dot online';
      text.textContent='ONLINE';
    }else{
      dot.className='dot offline';
      text.textContent='OFFLINE';
    }

    busy=data.busy;
    powerButton.disabled=busy;
    hardButton.disabled=busy;

    if(data.busy){
      message('PWR_SW ACTIVE // '+data.action,'',true);
    }
  }catch{
    document.getElementById('wifiDot').className='dot offline';
    document.getElementById('wifiText').textContent='NO LINK';
  }
}

powerButton.addEventListener('click',async()=>{
  if(busy) return;

  vibrate(35);
  powerButton.classList.add('active');

  setTimeout(()=>{
    powerButton.classList.remove('active');
  },150);

  try{
    message('SENDING 500ms PWR_SW...','',true);

    const data=await postAPI('/api/power');

    message(data.message,'ok');
    vibrate([30,45,30]);
    refresh();
  }catch(error){
    message(error.message,'error');
  }
});

function cancelHard(){
  if(hardTimer){
    clearTimeout(hardTimer);
    hardTimer=null;
  }

  hardButton.classList.remove('active');
  holdFill.style.transition='none';
  holdFill.style.width='0';

  requestAnimationFrame(()=>{
    requestAnimationFrame(()=>{
      holdFill.style.transition='';
    });
  });
}

function startHard(){
  if(busy) return;

  cancelHard();

  hardButton.classList.add('active');
  message('HOLD // ARMING HARD OFF...','',true);
  vibrate(25);

  holdFill.style.transition='width 1.7s linear';

  requestAnimationFrame(()=>{
    holdFill.style.width='100%';
  });

  hardTimer=setTimeout(async()=>{
    hardTimer=null;

    vibrate([90,50,90]);

    try{
      message('HARD OFF // 5.5s PWR_SW','',true);

      const data=await postAPI('/api/hardoff');

      message(data.message,'ok');
    }catch(error){
      message(error.message,'error');
    }

    cancelHard();
    refresh();
  },1700);
}

hardButton.addEventListener('pointerdown',startHard);
hardButton.addEventListener('pointerup',cancelHard);
hardButton.addEventListener('pointerleave',cancelHard);
hardButton.addEventListener('pointercancel',cancelHard);
hardButton.addEventListener('contextmenu',event=>event.preventDefault());

refresh();
setInterval(refresh,1000);
</script>

</body>
</html>
)rawliteral";

  return html;
}

// ============================================================
// LOGIN HANDLERS
// ============================================================

void handleLoginGet() {
  if (!loginEnabled || setupMode) {
    server.sendHeader("Location", "/");
    server.send(303, "text/plain", "");
    return;
  }

  server.send(
    200,
    "text/html; charset=utf-8",
    loginPage(false)
  );
}

void handleLoginPost() {
  if (!loginEnabled || setupMode) {
    server.sendHeader("Location", "/");
    server.send(303, "text/plain", "");
    return;
  }

  String user = server.arg("user");
  String pass = server.arg("pass");

  if (
    user == loginUser &&
    pass == loginPassword
  ) {

    server.sendHeader(
      "Set-Cookie",
      String("pcsid=") +
      sessionToken +
      "; Path=/; HttpOnly; SameSite=Strict"
    );

    server.sendHeader("Location", "/");
    server.send(303, "text/plain", "");
    return;
  }

  server.send(
    401,
    "text/html; charset=utf-8",
    loginPage(true)
  );
}

void handleLogout() {
  server.sendHeader(
    "Set-Cookie",
    "pcsid=deleted; Path=/; Max-Age=0; SameSite=Strict"
  );

  server.sendHeader("Location", "/login");
  server.send(303, "text/plain", "");
}

// ============================================================
// PAGES
// ============================================================

void handleRoot() {
  server.sendHeader(
    "Cache-Control",
    "no-store"
  );

  if (setupMode) {
    server.send(
      200,
      "text/html; charset=utf-8",
      settingsPage()
    );

    return;
  }

  if (!requirePageAuth()) return;

  server.send(
    200,
    "text/html; charset=utf-8",
    controlPage()
  );
}

void handleSettings() {
  if (!requirePageAuth()) return;

  server.sendHeader(
    "Cache-Control",
    "no-store"
  );

  server.send(
    200,
    "text/html; charset=utf-8",
    settingsPage()
  );
}

// ============================================================
// SAVE SETTINGS
// ============================================================

void handleSave() {
  if (!requirePageAuth()) return;

  if (!checkActionToken()) {
    server.send(
      403,
      "text/plain",
      "Invalid token"
    );

    return;
  }

  String newSSID = server.arg("ssid");
  String newWifiPass = server.arg("wifipass");
  String newHostname = server.arg("hostname");

  bool newStatic = server.hasArg("static");

  String newIP = server.arg("ip");
  String newGateway = server.arg("gw");
  String newSubnet = server.arg("mask");
  String newDNS = server.arg("dns");

  bool newAuth = server.hasArg("auth");

  String newLoginUser = server.arg("loginuser");
  String newLoginPass = server.arg("loginpass");

  newHostname.trim();
  newHostname.toLowerCase();
  newLoginUser.trim();

  if (newWifiPass.length() == 0) {
    newWifiPass = wifiPassword;
  }

  if (newLoginUser.length() == 0) {
    newLoginUser =
      loginUser.length()
        ? loginUser
        : "admin";
  }

  if (newLoginPass.length() == 0) {
    newLoginPass =
      loginPassword.length()
        ? loginPassword
        : "1234";
  }

  if (
    newSSID.length() == 0 ||
    !validHostname(newHostname)
  ) {

    server.send(
      400,
      "text/plain",
      "Ungueltige Eingabe"
    );

    return;
  }

  if (
    newAuth &&
    (
      newLoginUser.length() == 0 ||
      newLoginPass.length() < 4
    )
  ) {

    server.send(
      400,
      "text/plain",
      "Login-Passwort mindestens 4 Zeichen"
    );

    return;
  }

  if (newStatic) {
    IPAddress test;

    if (
      !test.fromString(newIP.c_str()) ||
      !test.fromString(newGateway.c_str()) ||
      !test.fromString(newSubnet.c_str())
    ) {

      server.send(
        400,
        "text/plain",
        "Ungueltige Static-IP Einstellung"
      );

      return;
    }

    if (
      newDNS.length() > 0 &&
      !test.fromString(newDNS.c_str())
    ) {

      server.send(
        400,
        "text/plain",
        "Ungueltiger DNS Server"
      );

      return;
    }
  }

  preferences.begin(
    "pcswitch",
    false
  );

  preferences.putString("ssid", newSSID);
  preferences.putString("wifipass", newWifiPass);
  preferences.putString("hostname", newHostname);

  preferences.putBool("static", newStatic);
  preferences.putString("ip", newIP);
  preferences.putString("gw", newGateway);
  preferences.putString("mask", newSubnet);
  preferences.putString("dns", newDNS);

  preferences.putBool("auth", newAuth);
  preferences.putString("auser", newLoginUser);
  preferences.putString("apass", newLoginPass);

  preferences.putBool("force", false);

  preferences.end();

  server.send(
    200,
    "text/html; charset=utf-8",
    "<!doctype html>"
    "<html>"
    "<body style='background:#07101b;color:#ddd;"
    "font-family:monospace;padding:30px'>"
    "<h2>CONFIG SAVED</h2>"
    "<p>Rebooting...</p>"
    "</body>"
    "</html>"
  );

  rebootRequested = true;
  rebootAt = millis() + 1000;
}

// ============================================================
// API
// ============================================================

void handleAPIStatus() {
  if (!requireAPIAuth()) return;

  bool wifiConnected =
    WiFi.status() == WL_CONNECTED;

  String json = "{";

  json += "\"ok\":true,";

  json +=
    "\"busy\":" +
    String(
      powerPulseActive
        ? "true"
        : "false"
    ) +
    ",";

  json +=
    "\"action\":\"" +
    jsonEscape(powerAction) +
    "\",";

  json +=
    "\"wifi\":" +
    String(
      wifiConnected
        ? "true"
        : "false"
    ) +
    ",";

  json +=
    "\"ip\":\"" +
    (
      wifiConnected
        ? WiFi.localIP().toString()
        : "0.0.0.0"
    ) +
    "\",";

  json +=
    "\"rssi\":" +
    String(
      wifiConnected
        ? WiFi.RSSI()
        : 0
    ) +
    ",";

  json +=
    "\"uptime\":" +
    String(
      millis() / 1000
    );

  json += "}";

  sendJSON(
    200,
    json
  );
}

void handleAPIPower() {
  if (!requireAPIAuth()) return;

  if (!checkActionToken()) {
    sendJSON(
      403,
      "{\"ok\":false,\"message\":\"Invalid token\"}"
    );

    return;
  }

  if (
    !startPowerPulse(
      POWER_PRESS_MS,
      "POWER"
    )
  ) {

    sendJSON(
      409,
      "{\"ok\":false,\"message\":\"PWR_SW busy\"}"
    );

    return;
  }

  sendJSON(
    202,
    "{\"ok\":true,\"message\":\"POWER PULSE SENT // 500ms\"}"
  );
}

void handleAPIHardOff() {
  if (!requireAPIAuth()) return;

  if (!checkActionToken()) {
    sendJSON(
      403,
      "{\"ok\":false,\"message\":\"Invalid token\"}"
    );

    return;
  }

  if (
    !startPowerPulse(
      HARD_OFF_MS,
      "HARD OFF"
    )
  ) {

    sendJSON(
      409,
      "{\"ok\":false,\"message\":\"PWR_SW busy\"}"
    );

    return;
  }

  sendJSON(
    202,
    "{\"ok\":true,\"message\":\"HARD OFF ACTIVE // 5.5s\"}"
  );
}

void handleAPISetup() {
  if (!requireAPIAuth()) return;

  if (!checkActionToken()) {
    sendJSON(
      403,
      "{\"ok\":false,\"message\":\"Invalid token\"}"
    );

    return;
  }

  preferences.begin(
    "pcswitch",
    false
  );

  preferences.putBool(
    "force",
    true
  );

  preferences.end();

  sendJSON(
    200,
    "{\"ok\":true,\"message\":\"Rebooting into Setup AP\"}"
  );

  rebootRequested = true;
  rebootAt = millis() + 800;
}

void handleFactoryReset() {
  if (!requireAPIAuth()) return;

  if (!checkActionToken()) {
    sendJSON(
      403,
      "{\"ok\":false,\"message\":\"Invalid token\"}"
    );

    return;
  }

  releasePowerButton();

  preferences.begin(
    "pcswitch",
    false
  );

  preferences.clear();
  preferences.end();

  Serial.println();
  Serial.println(
    "FACTORY RESET COMPLETE"
  );

  sendJSON(
    200,
    "{\"ok\":true,\"message\":\"Factory reset complete\"}"
  );

  rebootRequested = true;
  rebootAt = millis() + 1000;
}

// ============================================================
// WEB SERVER
// ============================================================

void startWebServer() {
  const char* headerKeys[] = {
    "Cookie"
  };

  server.collectHeaders(
    headerKeys,
    1
  );

  server.on("/", HTTP_GET, handleRoot);

  server.on("/login", HTTP_GET, handleLoginGet);
  server.on("/login", HTTP_POST, handleLoginPost);
  server.on("/logout", HTTP_GET, handleLogout);

  server.on("/settings", HTTP_GET, handleSettings);
  server.on("/save", HTTP_POST, handleSave);

  server.on("/api/status", HTTP_GET, handleAPIStatus);
  server.on("/api/power", HTTP_POST, handleAPIPower);
  server.on("/api/hardoff", HTTP_POST, handleAPIHardOff);
  server.on("/api/setup", HTTP_POST, handleAPISetup);
  server.on("/api/factory-reset", HTTP_POST, handleFactoryReset);

  server.onNotFound(
    []() {
      if (setupMode) {
        server.sendHeader(
          "Location",
          "http://192.168.4.1/"
        );

        server.send(
          302,
          "text/plain",
          ""
        );

        return;
      }

      server.send(
        404,
        "text/plain",
        "404"
      );
    }
  );

  server.begin();

  Serial.println(
    "[HTTP] SERVER STARTED"
  );
}

// ============================================================
// SETUP / LOOP
// ============================================================

void setup() {
  Serial.begin(115200);

  delay(1500);

  Serial.println();
  Serial.println(
    "================================"
  );

  Serial.println(
    "ESP32-C3 PC CONTROL"
  );

  Serial.println(
    "================================"
  );

  initPowerOutput();

  sessionToken = createRandomToken();
  actionToken = createRandomToken();

  loadConfiguration();

  bool connected = false;

  if (forceSetup) {
    preferences.begin(
      "pcswitch",
      false
    );

    preferences.putBool(
      "force",
      false
    );

    preferences.end();

    startSetupAccessPoint();

  } else {
    connected = connectToHomeWiFi();

    if (!connected) {
      startSetupAccessPoint();
    }
  }

  startWebServer();

  if (connected) {
    startMDNS();
  }

  Serial.println(
    "BOOT COMPLETE"
  );
}

void loop() {
  if (setupMode) {
    dnsServer.processNextRequest();
  }

  server.handleClient();

  updatePowerPulse();
  updateWiFiConnection();

  if (
    rebootRequested &&
    (int32_t)(
      millis() - rebootAt
    ) >= 0
  ) {
    // Always release PWR_SW before rebooting.
    releasePowerButton();

    delay(20);

    ESP.restart();
  }

  delay(2);
}
