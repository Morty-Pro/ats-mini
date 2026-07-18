#include "Common.h"
#include "Storage.h"
#include "Themes.h"
#include "Utils.h"
#include "Menu.h"
#include "Draw.h"
#include "LicenseAuth.h"  // License authentication system for device authorization

#include <WiFi.h>
#include <WiFiMulti.h>
#include <WiFiUdp.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <NTPClient.h>
#include <ESPmDNS.h>

#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Update.h>
#include <vector>
#include "mbedtls/pk.h"
#include "mbedtls/sha256.h"

// #ifndef OTA_ROOT_CA
// #define OTA_ROOT_CA nullptr
// #endif

#define CONNECT_TIME  3000  // Time of inactivity to start connecting WiFi
#define WIFI_MULTI_TOTAL_TIMEOUT  30000

WiFiMulti wifiMulti;

//
// Access Point (AP) mode settings
//
static const char *apSSID    = RECEIVER_NAME;
static const char *apPWD     = 0;       // No password
static const int   apChannel = 10;      // WiFi channel number (1..13)
static const bool  apHideMe  = false;   // TRUE: disable SSID broadcast
static const int   apClients = 3;       // Maximum simultaneous connected clients

static uint16_t ajaxInterval = 2500;

static bool itIsTimeToWiFi = false; // TRUE: Need to connect to WiFi
static uint32_t connectTime = millis();

// Settings
String loginUsername = "";
String loginPassword = "";
static bool wifiScanHidden = false;

// AsyncWebServer object on port 80
AsyncWebServer server(80);

// NTP Client to get time
WiFiUDP ntpUDP;
NTPClient ntpClient(ntpUDP, "pool.ntp.org");

static bool wifiInitAP();
static bool wifiConnect();
static void webInit();

static void webSetConfig(AsyncWebServerRequest *request);

static const String webInputField(const String &name, const String &value, bool pass = false);
static const String webStyleSheet();
static const String webPage(const String &body);
static const String webUtcOffsetSelector();
static const String webThemeSelector();
static const String webRadioPage();
static const String webMemoryPage();
static const String webConfigPage();

// OTA update function prototypes
static void runOtaUpdate();
static bool downloadAndApplyFirmware(mbedtls_sha256_context &sha);
static bool startOTAUpdate(WiFiClient* client, int contentLength, mbedtls_sha256_context &sha);
static const String fetchLatestVersion();

// https(TLS) public_key
const char OTA_PUBLIC_KEY[] PROGMEM = R"EOF(-----BEGIN PUBLIC KEY-----
MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEVIlS2szOYLgYAuKwuxxZ5AuyjPl9
cO5KsGEXaQJRfeBpkC2+aPPIanbYCF1+MaxiS6SfNzw5OsHdwmTl7+xb1w==
-----END PUBLIC KEY-----)EOF";

const char OTA_ROOT_CA[] PROGMEM = R"EOF(-----BEGIN CERTIFICATE-----
MIICGzCCAaGgAwIBAgIQQdKd0XLq7qeAwSxs6S+HUjAKBggqhkjOPQQDAzBPMQsw
CQYDVQQGEwJVUzEpMCcGA1UEChMgSW50ZXJuZXQgU2VjdXJpdHkgUmVzZWFyY2gg
R3JvdXAxFTATBgNVBAMTDElTUkcgUm9vdCBYMjAeFw0yMDA5MDQwMDAwMDBaFw00
MDA5MTcxNjAwMDBaME8xCzAJBgNVBAYTAlVTMSkwJwYDVQQKEyBJbnRlcm5ldCBT
ZWN1cml0eSBSZXNlYXJjaCBHcm91cDEVMBMGA1UEAxMMSVNSRyBSb290IFgyMHYw
EAYHKoZIzj0CAQYFK4EEACIDYgAEzZvVn4CDCuwJSvMWSj5cz3es3mcFDR0HttwW
+1qLFNvicWDEukWVEYmO6gbf9yoWHKS5xcUy4APgHoIYOIvXRdgKam7mAHf7AlF9
ItgKbppbd9/w+kHsOdx1ymgHDB/qo0IwQDAOBgNVHQ8BAf8EBAMCAQYwDwYDVR0T
AQH/BAUwAwEB/zAdBgNVHQ4EFgQUfEKWrt5LSDv6kviejM9ti6lyN5UwCgYIKoZI
zj0EAwMDaAAwZQIwe3lORlCEwkSHRhtFcP9Ymd70/aTSVaYgLXTWNLxBo1BfASdW
tL4ndQavEi51mI38AjEAi/V3bNTIZargCyzuFJ0nN6T5U6VR5CmD1/iQMVtCnwr1
/q4AaOeMSQ+2b1tbFfLn
-----END CERTIFICATE-----)EOF";

// ============================================================
// LICENSE INTEGRATION
// These functions handle automatic license acquisition after
// WiFi connection is established.
// ============================================================

/**
 * @brief Request license acquisition after WiFi connects
 * 
 * Called when WiFi connection is established to automatically
 * acquire the device license from the server.
 */
void netRequestLicenseAcquisition() {
    // The checkAndAcquireLicense() function in netTickTime() will handle this
    // This is a convenience function for explicit triggering
    connectTime = millis();  // Trigger immediate check
    itIsTimeToWiFi = true;
}

//
// Delayed WiFi connection
//
void netRequestConnect()
{
  connectTime = millis();
  itIsTimeToWiFi = true;
}

void netTickTime()
{
  // Connect to WiFi if requested
  if(itIsTimeToWiFi && ((millis() - connectTime) > CONNECT_TIME))
  {
    netInit(wifiModeIdx);
    connectTime = millis();
    itIsTimeToWiFi = false;
  }
  
  // After WiFi is connected, check and acquire license if needed
  // This is non-blocking and only attempts once per check interval
  if(WiFi.status() == WL_CONNECTED)
  {
    checkAndAcquireLicense();
  }
}

//
// Get current connection status
// (-1 - not connected, 0 - disabled, 1 - connected, 2 - connected to network)
//
int8_t getWiFiStatus()
{
  wifi_mode_t mode = WiFi.getMode();

  switch(mode)
  {
    case WIFI_MODE_NULL:
      return(0);
    case WIFI_AP:
      return(WiFi.softAPgetStationNum()? 1 : -1);
    case WIFI_STA:
      return(WiFi.status()==WL_CONNECTED? 2 : -1);
    case WIFI_AP_STA:
      return((WiFi.status()==WL_CONNECTED)? 2 : WiFi.softAPgetStationNum()? 1 : -1);
    default:
      return(-1);
  }
}

char *getWiFiIPAddress()
{
  static char ip[16];
  return strcpy(ip, WiFi.status()==WL_CONNECTED ? WiFi.localIP().toString().c_str() : "");
}

//
// Stop WiFi hardware
//
void netStop()
{
  wifi_mode_t mode = WiFi.getMode();

  MDNS.end();

  // If network connection up, shut it down
  if((mode==WIFI_STA) || (mode==WIFI_AP_STA))
    WiFi.disconnect(true);

  // If access point up, shut it down
  if((mode==WIFI_AP) || (mode==WIFI_AP_STA))
    WiFi.softAPdisconnect(true);

  WiFi.mode(WIFI_MODE_NULL);
}

//
// Initialize WiFi network and services
//
void netInit(uint8_t netMode, bool showStatus)
{
  // Always disable WiFi first
  netStop();

  switch(netMode)
  {
    case NET_OFF:
      // Do not initialize WiFi if disabled
      return;
    case NET_AP_ONLY:
      // Start WiFi access point if requested
      WiFi.mode(WIFI_AP);
      // Let user see connection status if successful
      if(wifiInitAP() && showStatus) delay(2000);
      break;
    case NET_AP_CONNECT:
      // Start WiFi access point if requested
      WiFi.mode(WIFI_AP_STA);
      // Let user see connection status if successful
      if(wifiInitAP() && showStatus) delay(2000);
      break;
    case NET_RUN_OTA:
      // Start WiFi in station mode for OTA update
      WiFi.mode(WIFI_STA);
      // Let user see connection status if successful
      // if(wifiInitAP()) delay(2000);
      drawScreen("OTA Mode", "Connecting WiFi...");
      break;
    default:
      // No access point
      WiFi.mode(WIFI_STA);
      break;
  }

  // Initialize WiFi and try connecting to a network
  // Morty translate: will connect to wifi as STA if netMode need it.
  if(netMode>NET_AP_ONLY && wifiConnect())
  {
    // Let user see connection status if successful
    // Morty translate: added delay to show logs on screen from runnig the wifiConnect() function above,
    // otherwise countinue to next line.
    if(netMode!=NET_SYNC && netMode!=NET_RUN_OTA && showStatus) delay(2000); 
    

    // keyhan added: Handle OTA update mode
    if(netMode==NET_RUN_OTA)
    {
      drawScreen("OTA Update", "Checking for firmware...");delay(1500);
      runOtaUpdate();
      // After OTA, reset WiFi back to off
      netStop();
      return;
    }

    // NTP time updates will happen every 5 minutes
    ntpClient.setUpdateInterval(5*60*1000);

    // Get NTP time from the network
    clockReset();
    for(int j=0 ; j<10 ; j++)
      if(ntpSyncTime()) break; else delay(500);
  }

  // If only connected to sync or OTA then...
  if(netMode==NET_SYNC || netMode==NET_RUN_OTA)
  {
    // Sync or OTA is Done,
    // Drop network connection.
    WiFi.disconnect(true);
    WiFi.mode(WIFI_MODE_NULL);
  }
  else
  {
    // Initialize web server for remote configuration
    webInit();

    // Initialize mDNS
    MDNS.begin("signal");
    MDNS.addService("http", "tcp", 80);
  }
}

//
// Returns TRUE if NTP time is available
//
bool ntpIsAvailable()
{
  return(ntpClient.isTimeSet());
}

//
// Update NTP time and synchronize clock with NTP time
//
bool ntpSyncTime()
{
  if(WiFi.status()==WL_CONNECTED)
  {
    ntpClient.update();

    if(ntpClient.isTimeSet())
      return(clockSet(
        ntpClient.getHours(),
        ntpClient.getMinutes(),
        ntpClient.getSeconds()
      ));
  }
  return(false);
}

//
// Initialize WiFi access point (AP)
//
static bool wifiInitAP()
{
  // These are our own access point (AP) addresses
  IPAddress ip(10, 1, 1, 1);
  IPAddress gateway(10, 1, 1, 1);
  IPAddress subnet(255, 255, 255, 0);

  // Start as access point (AP)
  WiFi.softAP(apSSID, apPWD, apChannel, apHideMe, apClients);
  WiFi.softAPConfig(ip, gateway, subnet);

  drawScreen(
    ("Use Access Point " + String(apSSID)).c_str(),
    ("IP : " + WiFi.softAPIP().toString() + " or atsmini.local").c_str()
  );

  ajaxInterval = 2500;
  return(true);
}

//
// Connect to a WiFi network (load WiFi from prefs(memory))
//
static bool wifiConnect()
{
  String status = "Connecting to WiFi network...";
  
  // Clean credentials
  wifiMulti.APlistClean();

  // Get the preferences
  prefs.begin("network", true, STORAGE_PARTITION);
  loginUsername = prefs.getString("loginusername", "");
  loginPassword = prefs.getString("loginpassword", "");
  wifiScanHidden = prefs.getBool("wifiscanhidden", false);

  // Try connecting to 3 known WiFi networks
  for(int j=0 ; (j<3) ; j++)
  {
    char nameSSID[16], namePASS[16];
    sprintf(nameSSID, "wifissid%d", j+1);
    sprintf(namePASS, "wifipass%d", j+1);

    String ssid = prefs.getString(nameSSID, "");
    String password = prefs.getString(namePASS, "");

    if(ssid != "")
      wifiMulti.addAP(ssid.c_str(), password.c_str());
  }

  // Done with preferences
  prefs.end();

  drawScreen(status.c_str());delay(1000);

  // try connect to hidden SSID AP
  consumeAbortPending();
  wl_status_t wifiStatus = WL_NO_SSID_AVAIL;
  uint32_t start = millis();
  while(((millis() - start)<WIFI_MULTI_TOTAL_TIMEOUT) && (wifiStatus!=WL_CONNECTED))
  {
    wifiStatus = (wl_status_t)wifiMulti.run(5000, wifiScanHidden);

    if(consumeAbortPending())
    {
      WiFi.disconnect();
      break;
    }

    if((wifiStatus!=WL_CONNECTED) && ((millis() - start)<WIFI_MULTI_TOTAL_TIMEOUT))
      delay(1000);
  }

  // If failed connecting to WiFi network...
  if (wifiStatus != WL_CONNECTED)
  {
    // WiFi connection failed
    drawScreen(status.c_str(), "No WiFi connection");
    // Done
    return(false);
  }
  else
  {
    // WiFi connection succeeded
    drawScreen(
      ("Connected to WiFi network (" + WiFi.SSID() + ")").c_str(),
      ("IP : " + WiFi.localIP().toString() + " or atsmini.local").c_str()
    );
    // Done
    ajaxInterval = 1000;
    return(true);
  }
}

//
// Initialize internal web server
//
static void webInit()
{
  server.on("/", HTTP_ANY, [] (AsyncWebServerRequest *request) {
    request->send(200, "text/html", webRadioPage());
  });

  server.on("/memory", HTTP_ANY, [] (AsyncWebServerRequest *request) {
    request->send(200, "text/html", webMemoryPage());
  });

  server.on("/config", HTTP_ANY, [] (AsyncWebServerRequest *request) {
    if(loginUsername != "" && loginPassword != "")
      if(!request->authenticate(loginUsername.c_str(), loginPassword.c_str()))
        return request->requestAuthentication();
    request->send(200, "text/html", webConfigPage());
  });

  server.onNotFound([] (AsyncWebServerRequest *request) {
    request->send(404, "text/plain", "Not found");
  });

  // This method saves configuration form contents
  server.on("/setconfig", HTTP_ANY, webSetConfig);

  // Start web server
  server.begin();
}

void webSetConfig(AsyncWebServerRequest *request)
{
  uint32_t prefsSave = 0;

  // Start modifying preferences
  prefs.begin("network", false, STORAGE_PARTITION);

  // Save user name and password
  if(request->hasParam("username", true) && request->hasParam("password", true))
  {
    loginUsername = request->getParam("username", true)->value();
    loginPassword = request->getParam("password", true)->value();

    prefs.putString("loginusername", loginUsername);
    prefs.putString("loginpassword", loginPassword);
  }

  // Save SSIDs and their passwords
  bool haveSSID = false;
  for(int j=0 ; j<3 ; j++)
  {
    char nameSSID[16], namePASS[16];

    sprintf(nameSSID, "wifissid%d", j+1);
    sprintf(namePASS, "wifipass%d", j+1);

    if(request->hasParam(nameSSID, true) && request->hasParam(namePASS, true))
    {
      String ssid = request->getParam(nameSSID, true)->value();
      String pass = request->getParam(namePASS, true)->value();
      prefs.putString(nameSSID, ssid);
      prefs.putString(namePASS, pass);
      haveSSID |= ssid != "" && pass != "";
    }
  }

  // Save hidden SSID scanning preference
  wifiScanHidden = request->hasParam("wifiscanhidden", true);
  prefs.putBool("wifiscanhidden", wifiScanHidden);

  // Save time zone
  if(request->hasParam("utcoffset", true))
  {
    String utcOffset = request->getParam("utcoffset", true)->value();
    utcOffsetIdx = utcOffset.toInt();
    clockRefreshTime();
    prefsSave |= SAVE_SETTINGS;
  }

  // Save theme
  if(request->hasParam("theme", true))
  {
    String theme = request->getParam("theme", true)->value();
    themeIdx = theme.toInt();
    prefsSave |= SAVE_SETTINGS;
  }

  // Save scroll direction and menu zoom
  scrollDirection = request->hasParam("scroll", true)? -1 : 1;
  zoomMenu        = request->hasParam("zoom", true);
  prefsSave |= SAVE_SETTINGS;

  // Done with the preferences
  prefs.end();

  // Save preferences immediately
  prefsRequestSave(prefsSave, true);

  // Show config page again
  request->redirect("/config");

  // If we are currently in AP mode, and infrastructure mode requested,
  // and there is at least one SSID / PASS pair, request network connection
  if(haveSSID && (wifiModeIdx>NET_AP_ONLY) && (WiFi.status()!=WL_CONNECTED))
    netRequestConnect();
}

static const String webInputField(const String &name, const String &value, bool pass)
{
  String newValue(value);

  newValue.replace("\"", "&quot;");
  newValue.replace("'", "&apos;");

  return(
    "<INPUT TYPE='" + String(pass? "PASSWORD":"TEXT") + "' NAME='" +
    name + "' VALUE='" + newValue + "'>"
  );
}

static const String webStyleSheet()
{
  return
"html{"
"background:#070b14;"
"background-image:radial-gradient(circle at top,#20244d 0%,#0b0f18 40%,#06070b 100%);"
"min-height:100%;"
"}"

"body{"
"margin:0;"
"padding:30px 15px;"
"font-family:Arial,Helvetica,sans-serif;"
"color:#eef2ff;"
"}"

"h1{"
"text-align:center;"
"font-size:34px;"
"font-weight:700;"
"letter-spacing:1px;"
"color:#ffffff;"
"text-shadow:0 0 12px rgba(174,92,255,.7);"
"margin-bottom:30px;"
"}"

"h2,h3{"
"color:#7be8ff;"
"margin-top:0;"
"}"

"table{"
"width:100%;"
"max-width:900px;"
"margin:auto;"
"border-collapse:separate;"
"border-spacing:0 12px;"
"background:#0d1323;"
"border:1px solid #2a3154;"
"border-radius:18px;"
"overflow:hidden;"
"box-shadow:0 0 35px rgba(140,0,255,.18);"
"}"

"th.heading{"
"background:linear-gradient(90deg,#6b2cff,#00d4ff);"
"color:white;"
"padding:18px;"
"font-size:20px;"
"}"

"td{"
"padding:14px;"
"}"

"td.label{"
"width:220px;"
"color:#8deaff;"
"font-weight:bold;"
"text-align:right;"
"}"

"input[type=text],"
"input[type=password],"
"select{"
"width:100%;"
"box-sizing:border-box;"
"background:#141c2f;"
"border:1px solid #39456f;"
"border-radius:10px;"
"padding:12px;"
"color:white;"
"font-size:15px;"
"outline:none;"
"transition:.25s;"
"}"

"input[type=text]:focus,"
"input[type=password]:focus,"
"select:focus{"
"border-color:#00d9ff;"
"box-shadow:0 0 12px rgba(0,217,255,.45);"
"}"

"input[type=submit]{"
"background:linear-gradient(90deg,#8f37ff,#00d3ff);"
"border:none;"
"color:white;"
"padding:14px 34px;"
"font-size:17px;"
"font-weight:bold;"
"border-radius:12px;"
"cursor:pointer;"
"transition:.25s;"
"box-shadow:0 0 18px rgba(143,55,255,.35);"
"}"

"input[type=submit]:hover{"
"transform:translateY(-2px);"
"box-shadow:0 0 24px rgba(0,211,255,.45);"
"}"

"a{"
"color:#6fe9ff;"
"text-decoration:none;"
"}"

"a:hover{"
"color:#ffffff;"
"}"

".center{"
"text-align:center;"
"}"

"hr{"
"border:none;"
"height:1px;"
"background:linear-gradient(to right,transparent,#8f37ff,#00d3ff,transparent);"
"margin:20px 0;"
"}"

"fieldset{"
"border:1px solid #39456f;"
"border-radius:12px;"
"padding:18px;"
"background:#0e1528;"
"}"

"legend{"
"padding:0 10px;"
"color:#77e8ff;"
"}"

"button{"
"background:linear-gradient(90deg,#8f37ff,#00d3ff);"
"border:none;"
"border-radius:10px;"
"padding:12px 20px;"
"color:white;"
"}"

"::-webkit-scrollbar{"
"width:8px;"
"}"

"::-webkit-scrollbar-thumb{"
"background:#7d39ff;"
"border-radius:10px;"
"}"

"@media(max-width:700px){"
"body{padding:10px;}"
"table{font-size:14px;}"
"h1{font-size:26px;}"
"td.label{display:block;width:auto;text-align:left;padding-bottom:6px;}"
"}"
;
}

static const String webPage(const String &body)
{
  return
"<!DOCTYPE HTML>"
"<HTML>"
"<HEAD>"
  "<META CHARSET='UTF-8'>"
  "<META NAME='viewport' CONTENT='width=device-width, initial-scale=1.0'>"
  "<TITLE>تنظیمات سیگنال مینی</TITLE>"
  "<STYLE>" + webStyleSheet() + "</STYLE>"
"</HEAD>"
"<BODY STYLE='font-family: sans-serif;'>" + body + "</BODY>"
"</HTML>"
;
}

static const String webUtcOffsetSelector()
{
  String result = "";

  for(int i=0 ; i<getTotalUTCOffsets(); i++)
  {
    char text[64];

    sprintf(text,
      "<OPTION VALUE='%d'%s>%s</OPTION>",
      i, utcOffsetIdx==i? " SELECTED":"",
      utcOffsets[i].desc
    );

    result += text;
  }

  return(result);
}

static const String webThemeSelector()
{
  String result = "";

  for(int i=0 ; i<getTotalThemes(); i++)
  {
    char text[64];

    sprintf(text,
      "<OPTION VALUE='%d'%s>%s</OPTION>",
       i, themeIdx==i? " SELECTED":"", theme[i].name
    );

    result += text;
  }

  return(result);
}

static const String webRadioPage()
{
  String ip = "";
  String ssid = "";
  String freq = currentMode == FM?
    String(currentFrequency / 100.0) + "MHz "
  : String(currentFrequency + currentBFO / 1000.0) + "kHz ";

  if(WiFi.status()==WL_CONNECTED)
  {
    ip = WiFi.localIP().toString();
    ssid = WiFi.SSID();
  }
  else
  {
    ip = WiFi.softAPIP().toString();
    ssid = String(apSSID);
  }

  return webPage(
"<H1>سیگنال مینی - مرکز کنترل دستگاه</H1>"
"<P ALIGN='CENTER'>"
  "<A HREF='/memory'>کانال ها</A>&nbsp;|&nbsp;<A HREF='/config'>تنظیمات</A>"
"</P>"
"<TABLE COLUMNS=2>"
"<TR>"
  "<TD CLASS='LABEL'>IP Address</TD>"
  "<TD><A HREF='http://" + ip + "'>" + ip + "</A> (" + ssid + ")</TD>"
"</TR>"
"<TR>"
  "<TD CLASS='LABEL'>MAC Address</TD>"
  "<TD>" + String(getMACAddress()) + "</TD>"
"</TR>"
"<TR>"
  "<TD CLASS='LABEL'>Firmware</TD>"
  "<TD>" + String(getVersion(true)) + "</TD>"
"</TR>"
"<TR>"
  "<TD CLASS='LABEL'>باند</TD>"
  "<TD>" + String(getCurrentBand()->bandName) + "</TD>"
"</TR>"
"<TR>"
  "<TD CLASS='LABEL'>فرکانس</TD>"
  "<TD>" + freq + String(bandModeDesc[currentMode]) + "</TD>"
"</TR>"
"<TR>"
  "<TD CLASS='LABEL'>قدرت سیگنال</TD>"
  "<TD>" + String(rssi) + "dBuV</TD>"
"</TR>"
"<TR>"
  "<TD CLASS='LABEL'>نسبت سیگنال به نویز</TD>"
  "<TD>" + String(snr) + "dB</TD>"
"</TR>"
"<TR>"
  "<TD CLASS='LABEL'>ولتاژ باتری</TD>"
  "<TD>" + String(batteryMonitor()) + "V</TD>"
"</TR>"
"</TABLE>"
);
}

static const String webMemoryPage()
{
  String items = "";

  for(int j=0 ; j<MEMORY_COUNT ; j++)
  {
    char text[64];
    sprintf(text, "<TR><TD CLASS='LABEL' WIDTH='10%%'>%02d</TD><TD>", j+1);
    items += text;

    if(!memories[j].freq)
      items += "&nbsp;---&nbsp;</TD></TR>";
    else
    {
      String freq = memories[j].mode == FM?
        String(memories[j].freq / 1000000.0) + "MHz "
      : String(memories[j].freq / 1000.0) + "kHz ";
      items += freq + bandModeDesc[memories[j].mode] + "</TD></TR>";
    }
  }

  return webPage(
"<H1>حافظه ی سیگنال مینی</H1>"
"<P ALIGN='CENTER'>"
  "<A HREF='/'>وضعیت</A>&nbsp;|&nbsp;<A HREF='/config'>تنظیمات</A>"
"</P>"
"<TABLE COLUMNS=2>" + items + "</TABLE>"
);
}

const String webConfigPage()
{
  prefs.begin("network", true, STORAGE_PARTITION);
  String ssid1 = prefs.getString("wifissid1", "");
  String pass1 = prefs.getString("wifipass1", "");
  String ssid2 = prefs.getString("wifissid2", "");
  String pass2 = prefs.getString("wifipass2", "");
  String ssid3 = prefs.getString("wifissid3", "");
  String pass3 = prefs.getString("wifipass3", "");
  bool scanHidden = prefs.getBool("wifiscanhidden", false);
  prefs.end();

  return webPage(
"<H1>تنظیمات سیگنال مینی</H1>"
"<P ALIGN='CENTER'>"
  "<A HREF='/'>وضعیت</A>"
  "&nbsp;|&nbsp;<A HREF='/memory'>کانال ها</A>"
"</P>"
"<FORM ACTION='/setconfig' METHOD='POST'>"
  "<TABLE COLUMNS=2>"
  "<TR><TH COLSPAN=2 CLASS='HEADING'>شبکه WiFi 1</TH></TR>"
  "<TR>"
    "<TD CLASS='LABEL'>اسم شبکه</TD>"
    "<TD>" + webInputField("wifissid1", ssid1) + "</TD>"
  "</TR>"
  "<TR>"
    "<TD CLASS='LABEL'>رمز</TD>"
    "<TD>" + webInputField("wifipass1", pass1, true) + "</TD>"
  "</TR>"
  "<TR><TH COLSPAN=2 CLASS='HEADING'>شبکه WiFi 2</TH></TR>"
  "<TR>"
    "<TD CLASS='LABEL'>اسم شبکه</TD>"
    "<TD>" + webInputField("wifissid2", ssid2) + "</TD>"
  "</TR>"
  "<TR>"
    "<TD CLASS='LABEL'>رمز</TD>"
    "<TD>" + webInputField("wifipass2", pass2, true) + "</TD>"
  "</TR>"
  "<TR><TH COLSPAN=2 CLASS='HEADING'>شبکه WiFi 3</TH></TR>"
  "<TR>"
    "<TD CLASS='LABEL'>اسم شبکه</TD>"
    "<TD>" + webInputField("wifissid3", ssid3) + "</TD>"
  "</TR>"
  "<TR>"
    "<TD CLASS='LABEL'>رمز</TD>"
    "<TD>" + webInputField("wifipass3", pass3, true) + "</TD>"
  "</TR>"
  "<TR><TH COLSPAN=2 CLASS='HEADING'>تنظیم رمز و نام کاربری برای صفحه ی تنظیمات</TH></TR>"
  "<TR>"
    "<TD CLASS='LABEL'>نام کاربری</TD>"
    "<TD>" + webInputField("username", loginUsername) + "</TD>"
  "</TR>"
  "<TR>"
    "<TD CLASS='LABEL'>رمز</TD>"
    "<TD>" + webInputField("password", loginPassword, true) + "</TD>"
  "</TR>"
  "<TR><TH COLSPAN=2 CLASS='HEADING'>تنظیمات دستگاه</TH></TR>"
  "<TR>"
    "<TD CLASS='LABEL'>اسکن شبکه های مخفی WiFi</TD>"
    "<TD><INPUT TYPE='CHECKBOX' NAME='wifiscanhidden' VALUE='on'" +
    (scanHidden? " CHECKED ":"") + "></TD>"
  "</TR>"
  "<TR>"
    "<TD CLASS='LABEL'>منطقه زمانی</TD>"
    "<TD>"
      "<SELECT NAME='utcoffset'>" + webUtcOffsetSelector() + "</SELECT>"
    "</TD>"
  "</TR>"
  "<TR>"
    "<TD CLASS='LABEL'>تم رادیو</TD>"
    "<TD>"
      "<SELECT NAME='theme'>" + webThemeSelector() + "</SELECT>"
    "</TD>"
  "</TR>"
  "<TR>"
    "<TD CLASS='LABEL'>اسکرول برعکس کلید روتاری</TD>"
    "<TD><INPUT TYPE='CHECKBOX' NAME='scroll' VALUE='on'" +
    (scrollDirection<0? " CHECKED ":"") + "></TD>"
  "</TR>"
   "<TR>"
    "<TD CLASS='LABEL'>بزرگنمایی منو</TD>"
    "<TD><INPUT TYPE='CHECKBOX' NAME='zoom' VALUE='on'" +
    (zoomMenu? " CHECKED ":"") + "></TD>"
  "</TR>"
  "<TR><TH COLSPAN=2 CLASS='HEADING'>"
    "<INPUT TYPE='SUBMIT' VALUE='Save'>"
  "</TH></TR>"
  "</TABLE>"
"</FORM>"
);
}

//
// OTA (Over-The-Air) Update Functions
// These functions handle downloading and applying firmware updates
//
// OTA configuration - modify these URLs for your firmware source
// const char* firmwareUrl = "https://github.com/Morty-Pro/ATS-mini-keyhan/releases/download/ATS-mini-keyhan/ats-mini.ino.bin";
// const char* versionUrl = "https://raw.githubusercontent.com/Morty-Pro/ATS-mini-keyhan/refs/heads/main/version.txt";

// lozelab server
const char* firmwareUrl = "https://lozelab.ir/ats-mini.ino.bin";
const char* versionUrl = "https://lozelab.ir/version.txt";
// keyhan server
// const char* firmwareUrl = "https://khrh.ir/signal-mini-update/ota/ats-mini.ino.bin";
// const char* versionUrl = "https://khrh.ir/signal-mini-update/ota/version.txt";


// const char* sigUrl = "https://khrh.ir/signal-mini-update/ota/ats-mini.ino.sig";
// Current firmware version
const char* currentFirmwareVersion = getVersionNum();
const unsigned long updateCheckInterval = 1 * 60 * 1000;  // 1 minute in milliseconds
unsigned long lastUpdateCheck = 0;

static char cbuf[50];  // Static = persists after function returns


static void runOtaUpdate()
{ 
  // declare public_key
  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  mbedtls_sha256_starts(&sha, 0);
  // Step 1: Fetch the latest version from GitHub
  String latestVersion = fetchLatestVersion();
  if (latestVersion == "") {
    Serial.println("Failed to fetch latest version");
    drawScreen("Failed to fetch latest version");delay(2000);
    return;
  }
  drawScreen("Current Firmware Version: ", currentFirmwareVersion);delay(2000);
  drawScreen("Latest Firmware Version: ", latestVersion.c_str());delay(2000);

  // Step 2: Compare versions
  if (latestVersion != currentFirmwareVersion) {
    drawScreen("Updading Firmware to", latestVersion.c_str());delay(1000);
    downloadAndApplyFirmware(sha);
  } else {
    Serial.println("Device is up to date.");
  }
}

static const String fetchLatestVersion() {
  WiFiClientSecure client;
  client.setCACert(OTA_ROOT_CA);
  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.begin(client, versionUrl);

  int httpCode = http.GET();
  if (httpCode == HTTP_CODE_OK) {
    String latestVersion = http.getString();
    latestVersion.trim();  // Remove any extra whitespace
    http.end();
    return latestVersion;
  } else {
    Serial.printf("Failed to fetch version. HTTP code: %d\n", httpCode);
    http.end();
    return "";
  }
}

static bool downloadAndApplyFirmware(mbedtls_sha256_context &sha) {
  WiFiClientSecure client;
  client.setCACert(OTA_ROOT_CA);
  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.begin(client, firmwareUrl);

  int httpCode = http.GET();
  Serial.printf("HTTP GET code: %d\n", httpCode);
  sprintf(cbuf, "%d", httpCode);
  drawScreen("HTTP GET code:", cbuf);delay(1000);

  if (httpCode == HTTP_CODE_OK) {
    int contentLength = http.getSize();
    Serial.printf("Firmware size: %d Bytes\n", contentLength);
    sprintf(cbuf, "%d Bytes", contentLength);
    drawScreen("Firmware size:", cbuf);delay(1000);

    if (contentLength > 0) {
      WiFiClient* stream = http.getStreamPtr();
      if (startOTAUpdate(stream, contentLength, sha)) {
        Serial.println("OTA update successful, restarting...");
        drawScreen("OTA update successful,", "restarting...");delay(2000);

        // unsigned char hash[32];
        // mbedtls_sha256_finish(&sha, hash);
        // mbedtls_sha256_free(&sha); // sha contains the SHA256 of the downloaded firmware.
        // // download sign file from server
        // HTTPClient sigHttp;
        // sigHttp.begin(client, sigUrl);
        // int code2 = sigHttp.GET();
        // if(code2 != HTTP_CODE_OK)
        // {
        //     Update.abort();
        //     sigHttp.end();
        //     return false;
        // }

        // std::vector<uint8_t> signature(sigHttp.getSize());
        // sigHttp.getStream().readBytes(signature.data(), signature.size());
        // if(signature.size()==0){
        //     Update.abort();
        //     sigHttp.end();
        //     return false;
        // }

        // sigHttp.end();
        

        Serial.printf("start checking public_OTA");
        mbedtls_pk_context pk;
        mbedtls_pk_init(&pk);
        int ret2 = mbedtls_pk_parse_public_key(
            &pk,
            (const unsigned char*)OTA_PUBLIC_KEY,
            strlen(OTA_PUBLIC_KEY)+1
        );
        Serial.printf("start checking public_OTA: %d\n", ret2);
        if (ret2 != 0)
        {
          Serial.printf("start checking public_OTA: aborted");
            mbedtls_pk_free(&pk);
            Update.abort();
            return false;
        }

        // check signiture: if ret == 0  -> signiture OK , if ret != 0 signiture fail.
        // int ret = mbedtls_pk_verify(&pk, MBEDTLS_MD_SHA256, hash, 0, signature.data(), signature.size());
        // Serial.printf("SHA status (ret): %d\n", ret);

        // if(ret != 0)
        // {
        //     Update.abort();
        //     mbedtls_pk_free(&pk);
        //     return false;
        // }
        // mbedtls_pk_free(&pk);
        if(!Update.end(true))
        {
            Serial.printf("Error: Update end failed: %s\n", Update.errorString());
            sprintf(cbuf, "code: %s", Update.errorString());
            drawScreen("Error: Update end failed:", cbuf);delay(2000);
            return false;
        }
        // ESP.restart();

        wifiModeIdx = NET_OFF;
        netStop();
        WiFi.disconnect(true);
        WiFi.mode(WIFI_MODE_NULL);
        delay(1000);
        if(WiFi.status()==WL_CONNECTED){
          drawScreen("Almost done.", "turn off WiFi");delay(1000);
        }
        delay(1000);
        ESP.restart();
      } else {
        Serial.println("OTA update failed");
        drawScreen("OTA update failed.");delay(2000);
      }
    } else {
      Serial.println("Invalid firmware size");
      drawScreen("Invalid firmware size.");delay(2000);
    }
  } else {
    Serial.printf("Failed to fetch firmware. HTTP code: %d\n", httpCode);
    sprintf(cbuf, "HTTP code: %d", httpCode);
    drawScreen("Failed to fetch firmware.", cbuf);delay(2000);
  }
  http.end();
}

static bool startOTAUpdate(WiFiClient* client, int contentLength, mbedtls_sha256_context &sha) {
  Serial.println("Initializing update...");
  drawScreen("Initializing update...");delay(2000);

  if (!Update.begin(contentLength)) {
    Serial.printf("Update begin failed: %s\n", Update.errorString());
    sprintf(cbuf, "%s", Update.errorString());
    drawScreen("Update begin failed:", cbuf);delay(2000);
    return false;
  }

  Serial.println("Writing firmware...");
  drawScreen("Writing firmware...");delay(1000);
  size_t totalWritten = 0;
  int progress = 0;
  int lastProgress = 0;

  // Timeout variables
  const unsigned long timeoutDuration = 60*2000;  // 2 minute timeout if client have not connection to server.
  unsigned long lastDataTime = millis();
  Serial.print("written/contentLength: ");
  // compare the written bytes with the content length to ensure we write all data
  while (totalWritten < contentLength) {
    if (client->available()) {
      // get a buffer of update from server
      uint8_t buffer[128];
      size_t len = client->read(buffer, sizeof(buffer));
      // Serial.print("buffer len: "); Serial.println(len);
      // if buffer had value then...
      if (len > 0) {
        // add the buffer to OTA partition in FLASH
        Update.write(buffer, len);
        // Serial.print("written/contentLength: "); Serial.print(totalWritten); Serial.print(" / "); Serial.println(contentLength);
        
        // mbedtls_sha256_update(&sha, buffer, len);
        totalWritten += len;
        
        // Calculate and print progress
        progress = (totalWritten * 100) / contentLength;
        if (progress != lastProgress) {
          Serial.printf("Download & Write: %d%%\n", progress);
          sprintf(cbuf, "%d%%", progress);
          drawScreen("Download & Write...", cbuf);
          lastProgress = progress;
        }
        lastDataTime = millis();
      }
    }
    // Check for timeout
    if (millis() - lastDataTime > timeoutDuration) {
      Serial.println("Timeout: No data received for too long. Aborting update...");
      drawScreen("Timeout: downlaod timeout.", " Aborting update...");delay(2000);
      Update.abort();
      return false;
    }

    yield();
  }
  Serial.println("\nWriting complete");
  drawScreen("Writing complete");delay(1000);

  if (totalWritten != contentLength) {
    Serial.printf("Error: Write incomplete. Expected %d but got %d bytes\n", contentLength, totalWritten);
    sprintf(cbuf, "Written: %d / %d Bytes", totalWritten, contentLength);
    drawScreen("Error: Write incomplete.", cbuf);delay(2000);
    Update.abort();
    return false;
  }

  // if (!Update.end()) {
  //   Serial.printf("Error: Update end failed: %s\n", Update.errorString());
  //   sprintf(cbuf, "code: %s", Update.errorString());
  //   drawScreen("Error: Update end failed:", cbuf);delay(2000);
  //   return false;
  // }
  
  Serial.println("Update successfully completed");
  drawScreen("Update successfully completed");delay(1000);
  return true;
}