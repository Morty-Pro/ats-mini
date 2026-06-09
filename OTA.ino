#include <Update.h>
#include <ArduinoJson.h>
#include "secrets.h"  // Contains your github_pat token

const char* ssid = ""; // put your wifi name
const char* password = ""; // put your wifi passowrd

// --- GitHub Repo Details ---
const char* github_owner = "YOUR_GITHUB_USERNAME"; // your github username
const char* github_repo = "ESP32-Private-OTA"; // your repo name
const char* firmware_asset_name = "firmware.bin"; // your file name

// --- Current Firmware Version ---
const char* currentFirmwareVersion = "1.0.0";

// --- Update Check Timer ---
unsigned long lastUpdateCheck = 0;
const long updateCheckInterval = 5 * 60 * 1000;  // 5 minutes in milliseconds

// =================================================================================
// SETUP: Runs once at boot.
// =================================================================================
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\nBooting up...");
  Serial.println("Current Firmware Version: " + String(currentFirmwareVersion));
  connectToWiFi();
  delay(6000);
  checkForFirmwareUpdate();
}

// =================================================================================
// LOOP: Runs continuously. This is the heart of the application.
// =================================================================================
void loop() {
}

// =================================================================================
// HELPER FUNCTIONS
// =================================================================================

void connectToWiFi() {
  Serial.print("Connecting to WiFi");
  WiFi.begin(ssid, password);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi connected. IP: " + WiFi.localIP().toString());
    Serial.println("Starting firmware checking process");
  } else {
    Serial.println("\nFailed to connect to WiFi. Will retry later.");
  }
}


void checkForFirmwareUpdate() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi not connected. Skipping update check.");
    return;
  }

  String apiUrl = "https://api.github.com/repos/" + String(github_owner) + "/" + String(github_repo) + "/releases/latest";

  Serial.println("---------------------------------");
  Serial.println("Checking for new firmware...");
  Serial.println("Fetching release info from: " + apiUrl);

  HTTPClient http;
  http.begin(apiUrl);
  
  // 1. Increase timeout to prevent "IncompleteInput" on slow connections
  http.setTimeout(10000); 
  
  http.addHeader("Authorization", "token " + String(github_pat));
  http.addHeader("Accept", "application/vnd.github.v3+json");
  http.setUserAgent("ESP32-OTA-Client");

  Serial.println("Sending API request...");
  int httpCode = http.GET();

  if (httpCode != HTTP_CODE_OK) {
    Serial.printf("API request failed. HTTP code: %d\n", httpCode);
    http.end();
    return;
  }
  
  Serial.printf("API request successful (HTTP %d). Parsing JSON.\n", httpCode);

  // 2. Create a Filter to ignore unnecessary data
  // We ONLY want tag_name and the assets list. This saves massive RAM.
  StaticJsonDocument<512> filter;
  filter["tag_name"] = true;
  filter["assets"][0]["name"] = true;
  filter["assets"][0]["id"] = true;

  // 3. Use DynamicJsonDocument with more memory (16KB)
  // The GitHub response is large, even with filtering, we need buffer space.
  DynamicJsonDocument doc(16384);
  
  // 4. Deserialize with the filter
  DeserializationError error = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  
  // Note: We do NOT call http.end() yet because we might need the stream, 
  // but usually deserializeJson consumes it. We will close it after logic.

  if (error) {
    Serial.print("Failed to parse JSON: ");
    Serial.println(error.c_str());
    http.end();
    return;
  }

  String latestVersion = doc["tag_name"].as<String>();
  if (latestVersion.isEmpty() || latestVersion == "null") {
    Serial.println("Could not find 'tag_name' in JSON response.");
    http.end();
    return;
  }
  
  Serial.println("Current Version: " + String(currentFirmwareVersion));
  Serial.println("Latest Version:  " + latestVersion);

  if (latestVersion != currentFirmwareVersion) {
    Serial.println(">>> New firmware available! <<<");
    Serial.println("Searching for asset named: " + String(firmware_asset_name));
    String firmwareUrl = "";
    
    // Iterate through assets
    JsonArray assets = doc["assets"].as<JsonArray>();

    for (JsonObject asset : assets) {
      String assetName = asset["name"].as<String>();
      Serial.println("Found asset: " + assetName);

      if (assetName == String(firmware_asset_name)) {
        String assetId = asset["id"].as<String>();
        // Construct the direct download URL for the asset
        firmwareUrl = "https://api.github.com/repos/" + String(github_owner) + "/" + String(github_repo) + "/releases/assets/" + assetId;
        Serial.println("Found matching asset! Preparing to download.");
        break;
      }
    }
    
    // Close the JSON connection before starting the download connection
    http.end(); 

    if (firmwareUrl.isEmpty()) {
      Serial.println("Error: Could not find the specified firmware asset in the release.");
      return;
    }
    
    // Start the download
    downloadAndApplyFirmware(firmwareUrl);

  } else {
    Serial.println("Device is up to date. No update needed.");
    http.end();
  }
  Serial.println("---------------------------------");
}

void downloadAndApplyFirmware(String url) {
  HTTPClient http;
  Serial.println("Starting firmware download from: " + url);

  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setUserAgent("ESP32-OTA-Client");
  http.begin(url);
  http.addHeader("Accept", "application/octet-stream");
  http.addHeader("Authorization", "token " + String(github_pat));

  int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    Serial.printf("Download failed, HTTP code: %d\n", httpCode);
    http.end();
    return;
  }

  int contentLength = http.getSize();
  if (contentLength <= 0) {
    Serial.println("Error: Invalid content length.");
    http.end();
    return;
  }

  // Begin the OTA update
  if (!Update.begin(contentLength)) {
    Serial.printf("Update begin failed: %s\n", Update.errorString());
    http.end();
    return;
  }
  Serial.println("Writing firmware... (this may take a moment)");
  WiFiClient* stream = http.getStreamPtr();
  uint8_t buff[1024];  
  size_t totalWritten = 0;
  int lastProgress = -1;

  while (totalWritten < contentLength) {
    int available = stream->available();
    if (available > 0) {
      int readLen = stream->read(buff, min((size_t)available, sizeof(buff)));
      if (readLen < 0) {
        Serial.println("Error reading from stream");
        Update.abort();
        http.end();
        return;
      }

      if (Update.write(buff, readLen) != readLen) {
        Serial.printf("Error: Update.write failed: %s\n", Update.errorString());
        Update.abort();
        http.end();
        return;
      }

      totalWritten += readLen;
      int progress = (int)((totalWritten * 100L) / contentLength);
      if (progress > lastProgress && (progress % 5 == 0 || progress == 100)) {
        Serial.printf("Progress: %d%%", progress);
        Serial.println();
        if (progress == 100) {
          Serial.println(); 
        } else {
          Serial.print("\r"); 
        }
        lastProgress = progress;
      }
    }
    delay(1);
  }
  Serial.println();

  if (totalWritten != contentLength) {
    Serial.printf("Error: Write incomplete. Wrote %d of %d bytes\n", totalWritten, contentLength);
    Update.abort();
  } else if (!Update.end()) {  // Finalize the update
    Serial.printf("Error: Update end failed: %s\n", Update.errorString());
  } else {
    Serial.println("Update complete! Restarting...");
    delay(1000);
    ESP.restart();
  }
  http.end();
}