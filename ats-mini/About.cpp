#include "Common.h"
#include "Storage.h"
#include "Themes.h"
#include "Utils.h"
#include "Menu.h"
#include "Draw.h"
#include <LittleFS.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <qrcode.h>

#include "images.h"

#include "esp_ota_ops.h"

static void displayQRCode(esp_qrcode_handle_t qrcode)
{
  int size = esp_qrcode_get_size(qrcode);

  for(int y = 0 ; y < size ; y++)
    for(int x = 0 ; x < size ; x++)
      if(esp_qrcode_get_module(qrcode, x, y))
        spr.fillRect(2 + x * 4, 170 - 2 - size * 4 + y * 4, 4, 4, TH.text);
}

String getChipIDStr()
{
    uint64_t id = ESP.getEfuseMac();
    char buf[13];          // 12 hex digits + null terminator
    sprintf(buf, "%012llX", id);
    return String(buf);
}

static void drawAboutCommon(uint8_t arrow)
{	// draw arrow
  if(arrow & 3) spr.fillRect(282, 11, 22, 3, TH.text_muted);
  if(arrow & 2) spr.fillTriangle(279, 12, 285, 8, 285, 16, TH.text_muted);
  if(arrow & 1) spr.fillTriangle(307, 12, 301, 8, 301, 16, TH.text_muted);
  // draw top title (two line)
  spr.setTextDatum(TL_DATUM);
  spr.setTextColor(TH.text_muted);
  spr.drawString(RECEIVER_DESC, 0, 0, 4);
  spr.setTextColor(TH.text);
//   spr.drawString(getVersion(), 2, 25, 2);
}

// instead of drawAboutKeyhan() function solve this.
//
void drawAboutKeyhan()
{
  // drawAboutCommon(arrow);
  // Display only the bitmap image
  spr.fillSprite(TFT_BLACK);
  spr.pushImage(0, 0, 320, 170, (uint16_t *)image_signal);
  spr.pushSprite(0, 0);
}

//
// Show HELP screen
//
void drawAboutHelp(uint8_t arrow)
{
  drawAboutCommon(arrow);
  esp_qrcode_config_t qrcode_config = ESP_QRCODE_CONFIG_DEFAULT();
  qrcode_config.display_func = displayQRCode;
  esp_qrcode_generate(&qrcode_config, MANUAL_URL);
  spr.drawString("Need Help?", 130, 70 + 16 * -1, 2);
  spr.drawString("Visit:", 130, 70 + 16 * 0, 2);
  spr.drawString("khrh.ir/signal-support", 130, 70 + 16 * 1, 2);

  // keyhan added: show wich OTA is runnig now and FLASH and PSRAM size status.
//   const esp_partition_t* running = esp_ota_get_running_partition();
//   static char sbuf[50];  // Static = persists after function returns
//   sprintf(sbuf, "Running partition: %s", running->label);
//   spr.drawString(sbuf, 130, 70 + 16 * 1, 2);
//   sprintf(sbuf, "Flash size: %u", ESP.getFlashChipSize());
//   spr.drawString(sbuf, 130, 70 + 16 * 2, 2);
//   sprintf(sbuf, "PSRAM size: %u", ESP.getPsramSize());
//   spr.drawString(sbuf, 130, 70 + 16 * 3, 2);


  // keyhan disabled 2 line below.
//   spr.drawString("Click the encoder button", 130, 70 + 16 * 1, 2);
//   spr.drawString("to continue.", 130, 70 + 16 * 2, 2);
//   if(arrow)
//   {
//     spr.drawString("Rotate the encoder to see", 130, 70 + 16 * 4, 2);
//     spr.drawString("the next page.", 130, 70 + 16 * 5, 2);
//   }
//   else
//   {
//     spr.drawString("To see this screen again,", 130, 70 + 16 * 4, 2);
//     spr.drawString("go to Menu->Settings->About.", 130, 70 + 16 * 5, 2);
//   }
  spr.pushSprite(0, 0);
}

//
// Show SYSTEM screen
//
static void drawAboutSystem(uint8_t arrow)
{
  drawAboutCommon(arrow);

  char text[100];
  sprintf(
    text,
    "CPU: %s r%i, %lu MHz",
    ESP.getChipModel(),
    ESP.getChipRevision(),
    ESP.getCpuFreqMHz()
  );
  spr.drawString(text, 2, 70 + 16 * -1, 2);

  sprintf(
    text,
    "FLASH: %luM, %luk (%luk), FS %luk (%luk)",
    ESP.getFlashChipSize() / (1024U * 1024U),
    ESP.getFreeSketchSpace() / 1024U,
    (ESP.getFreeSketchSpace() - ESP.getSketchSize()) / 1024U,
    (unsigned long)LittleFS.totalBytes() / 1024U,
    (unsigned long)(LittleFS.totalBytes() - LittleFS.usedBytes()) / 1024U
  );
  spr.drawString(text, 2, 70 + 16 * 0, 2);

  nvs_stats_t nvs_stats;
  nvs_get_stats(STORAGE_PARTITION, &nvs_stats);
  sprintf(
    text,
    "NVS: TOTAL %u, USED %u, FREE %u",
    nvs_stats.total_entries,
    nvs_stats.used_entries,
    nvs_stats.free_entries
  );
  spr.drawString(text, 2, 70 + 16 * 1, 2);

  sprintf(
    text,
    "MEM: HEAP %luk (%luk), PSRAM %luk (%luk)",
    ESP.getHeapSize()/1024U, ESP.getFreeHeap()/1024U,
    ESP.getPsramSize()/1024U, ESP.getFreePsram()/1024U
  );
  spr.drawString(text, 2, 70 + 16 * 2, 2);

  sprintf(
    text,
    "Display ID: %08lX, STAT: %02X%08lX",
#if !defined(LILYGO_SI473X)
    tft.readcommand32(ST7789_RDDID, 1),
    tft.readcommand8(ST7789_RDDST, 1),
    tft.readcommand32(ST7789_RDDST, 2)
#else
    0, 0, 0
#endif
  );
  spr.drawString(text, 2, 70 + 16 * 3, 2);

  char *ip = getWiFiIPAddress();
  sprintf(text, "WiFi MAC: %s%s%s", getMACAddress(), *ip ? ", IP: " : "", *ip ? ip : "");
  spr.drawString(text, 2, 70 + 16 * 4, 2);

  for(int i=0 ; i<8 ; i++)
  {
    uint16_t rgb = (i&1? 0x001F:0) | (i&2? 0x07E0:0) | (i&4? 0xF800:0);
    spr.fillRect(i*40, 160, 40, 20, rgb);
  }
  spr.pushSprite(0, 0);
}

static void drawLicense(uint8_t arrow)
{
	drawAboutCommon(arrow);
	spr.drawString("Licensed to", 2, 70 + 16 * 0, 2);
	spr.drawString("Morteza", 2, 70 + 16 * 1, 2);
	spr.drawString("Device ID:", 2, 70 + 16 * 2, 2);
	spr.drawString(getChipIDStr(), 2, 70 + 16 * 3, 2);
	spr.pushSprite(0, 0);
}

//
// Show AUTHORS screen
//
static void drawAboutAuthors(uint8_t arrow)
{
  drawAboutCommon(arrow);
  spr.drawString(FIRMWARE_URL, 2, 25 + 16, 2);
  spr.drawString(AUTHORS_LINE1, 2, 70, 2);
  spr.drawString(AUTHORS_LINE2, 2, 70 + 16, 2);
  spr.drawString(AUTHORS_LINE3, 2, 70 + 16 * 2, 2);
  spr.drawString(AUTHORS_LINE4, 2, 70 + 16 * 3, 2);
  spr.pushSprite(0, 0);
}

static void drawInfo(uint8_t arrow)
{
  static char dateString[35] = "\0";
  drawAboutCommon(arrow);
  sprintf(dateString, "%s", __DATE__);
  spr.drawString(dateString, 2, 70 + 16 * 0, 2);
  sprintf(dateString, "Version: %s", getVersionNum());
  spr.drawString(dateString, 2, 70 + 16 * 1, 2);
  spr.drawString(AUTHORS_LINE3, 2, 70 + 16 * 2, 2);
  spr.drawString(AUTHORS_LINE4, 2, 70 + 16 * 3, 2);
  spr.drawString(AUTHORS_LINE5, 2, 70 + 16 * 4, 2);
  spr.pushSprite(0, 0);
}

//
// Draw ABOUT screens
//
void drawAbout()
{
  switch(doAbout(0))
  {
    case 0: drawAboutHelp(1); break;
    case 1: drawInfo(3); break;
    case 2: drawLicense(2); break;
    default: break;
  }
}
