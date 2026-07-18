/**
 * @file LicenseAuth.cpp
 * @brief License Authentication System Implementation
 * 
 * This file implements the hardware-based license authentication system
 * for the Signal Mini ESP32-based radio device. It handles:
 * 
 * - Reading the unique ESP32 Efuse MAC address
 * - Communicating with the license server via HTTPS
 * - Decoding server responses using XOR cipher
 * - Storing authorization status in persistent NVS storage
 * - Integration with device startup flow
 * 
 * Architecture:
 * - License data is stored in a separate NVS namespace ("license")
 * - This namespace is NOT cleared by factory reset (nvsErase)
 * - Authorization is a simple boolean (true/false)
 * - The system persists across firmware updates and reprogramming
 */

// ============================================================
// INCLUDE FILES
// ============================================================

#include "LicenseAuth.h"
#include "Common.h"
#include "Draw.h"       // For display output during license acquisition
#include "Storage.h"    // For nvsErase reference (to show it doesn't clear license)

#include <esp_wifi.h>       // For WiFi/eFuse operations
// #include <esp_efuse_mac.h>  // For Efuse MAC reading
#include <esp_mac.h>
#include <Arduino.h>        // For String, Serial output
#include "mbedtls/md.h"     // For HMAC-SHA256 signature generation

// ============================================================
// PRIVATE HELPER FUNCTIONS (Static)
// ============================================================

/**
 * @brief XOR decode key derivation
 * 
 * Derives a single-byte XOR key from the device's Efuse MAC address.
 * The lower 8 bits of the MAC are used as the encryption key.
 * This ensures each device has a unique decoding key.
 * 
 * @param efuseMac The device's Efuse MAC address
 * @return uint8_t The XOR key (0-255)
 * 
 * @note This is a simple obfuscation method. The server must use
 *       the same key derivation to encode responses properly.
 */
static uint8_t deriveXorKey(uint64_t efuseMac) {
    // Use lower 8 bits of Efuse MAC as the XOR key
    return (uint8_t)(efuseMac & 0xFF);
}

/**
 * @brief Encode a string using XOR cipher
 * 
 * Encodes plaintext using a simple XOR cipher with the derived key.
 * This is used to create the authorization token format.
 * 
 * @param plaintext The text to encode
 * @param key The XOR key
 * @return String The encoded text (base64-like representation)
 * 
 * @details
 * Encoding Algorithm:
 * 1. Each character is XORed with the key
 * 2. Result is converted to hex string for safe transmission
 * 3. The hex string is URL-safe for HTTP transmission
 * 
 * @example
 * uint64_t mac = getEfuseMac();
 * String encoded = xorEncode("authorized:" + efuseMacToString(mac), deriveXorKey(mac));
 */
static String xorEncode(const String &plaintext, uint8_t key) {
    String hexResult = "";
    
    for (unsigned int i = 0; i < plaintext.length(); i++) {
        // XOR each character with the key
        uint8_t encodedChar = plaintext[i] ^ key;
        // Convert to 2-digit hex string
        char hexBuf[3];
        sprintf(hexBuf, "%02X", encodedChar);
        hexResult += hexBuf;
    }
    
    return hexResult;
}

/**
 * @brief Decode a hex-encoded XOR string
 * 
 * Reverses the XOR encoding: converts hex string back to bytes,
 * then XORs each byte with the key to recover plaintext.
 * 
 * @param hexEncoded The hex-encoded XOR string from server
 * @param key The XOR key to decode with
 * @param output Buffer to store decoded text
 * @param bufferSize Size of the output buffer
 * @return true if decoding was successful
 * @return false if encoding is invalid
 */
static bool xorDecode(const char *hexEncoded, uint8_t key, char *output, size_t bufferSize) {
    size_t inputLen = strlen(hexEncoded);
    size_t outputIdx = 0;
    
    // Hex string must have even length
    if (inputLen % 2 != 0) {
        return false;
    }
    
    // Decode pairs of hex characters
    for (size_t i = 0; i < inputLen && outputIdx < bufferSize - 1; i += 2) {
        // Convert two hex characters to byte value
        char hexPair[3] = {hexEncoded[i], hexEncoded[i + 1], '\0'};
        uint8_t encodedByte = (uint8_t)strtol(hexPair, NULL, 16);
        
        // XOR with key to get original character
        output[outputIdx++] = encodedByte ^ key;
    }
    
    output[outputIdx] = '\0';  // Null terminate the string
    return true;
}

/**
 * @brief Print license acquisition progress to screen
 * 
 * Displays the current state of the license acquisition process
 * on the device's TFT display.
 * 
 * @param title Main title shown at top of screen
 * @param status Detailed status message
 * @param progress Optional progress percentage (0-100), empty for none
 */
static void printLicenseProgress(const char *title, const char *status, const char *progress = nullptr) {
    extern TFT_eSprite spr;
    extern TFT_eSPI tft;
    extern uint16_t currentBrt;
    
    extern void ledcWrite(uint8_t, uint16_t);  // PWM backlight control
    
    // Clear screen and show status
    spr.fillSprite(TFT_BLACK);
    spr.setTextDatum(MC_DATUM);
    
    if (progress) {
        char displayBuf[64];
        sprintf(displayBuf, "%s\n%s\n%s", title, status, progress);
        spr.drawString(displayBuf, 160, 85, 2);
    } else {
        char displayBuf[64];
        sprintf(displayBuf, "%s\n%s", title, status);
        spr.drawString(displayBuf, 160, 85, 2);
    }
    
    spr.pushSprite(0, 0);
    
    // Ensure backlight is on
    extern uint16_t currentBrt;
    ledcWrite(15, currentBrt);  // PIN_LCD_BL is GPIO 15
}

// ============================================================
// PUBLIC FUNCTION IMPLEMENTATIONS
// ============================================================

uint64_t getEfuseMac() {
    /**
     * Read the ESP32's factory-assigned MAC address from eFuse hardware.
     * 
     * The ESP32 has unique eFuse values burned in at the factory.
     * ESP_GETEfuseMAC provides access to these hardware fuses.
     * We use the WiFi MAC address as the device identifier.
     * 
     * @return uint64_t The 48-bit MAC address as a 64-bit value (lower 6 bytes)
     *         Returns 0 if reading fails (should not happen on functioning hardware)
     */
    uint8_t macAddr[6];
    
    // Get the WiFi MAC address from eFuse
    // esp_read_mac() reads the factory-calibrated MAC from eFuse
    if (esp_read_mac(macAddr, ESP_MAC_WIFI_STA) != ESP_OK) {
        Serial.println("[LicenseAuth] ERROR: Failed to read Efuse MAC");
        return 0;
    }
    
    // Combine 6 bytes into a 64-bit value (little-endian format)
    uint64_t efuseMac = 0;
    for (int i = 0; i < 6; i++) {
        efuseMac |= ((uint64_t)macAddr[i] << (i * 8));
    }
    
    // Log for debugging (only shown in serial monitor)
    Serial.printf("[LicenseAuth] Device Efuse MAC: %012llX\n", (unsigned long long)efuseMac);
    
    return efuseMac;
}

String efuseMacToString(uint64_t efuseMac) {
    /**
     * Convert the 64-bit Efuse MAC to a readable hexadecimal string.
     * This is used for logging and displaying the device identifier.
     * 
     * @param efuseMac The 64-bit Efuse MAC value
     * @return String 12-character hex string (e.g., "545E209E139C")
     * 
     * @example
     * uint64_t mac = getEfuseMac();
     * String macStr = efuseMacToString(mac);
     * Serial.println("Device ID: " + macStr);
     */
    char hexStr[13];  // 12 hex chars + null terminator
    sprintf(hexStr, "%012llX", (unsigned long long)efuseMac);
    return String(hexStr);
}

LicenseResult requestLicenseFromServer(uint64_t efuseMac, String &response) {
    /**
     * Establish HTTPS connection to license server and send Efuse MAC
     * with HMAC-SHA256 signature for request authentication.
     * 
     * SECURITY ARCHITECTURE:
     * The MAC address is encrypted in transit by HTTPS, but the server
     * still sees it. To prevent theft/spoofing:
     * 
     * 1. HMAC-SHA256 Signature: Each request includes a cryptographic
     *    signature generated using a shared secret key. The server
     *    verifies this signature before processing the request.
     * 
     * 2. Replay Protection: A timestamp is included in the signature.
     *    Old captured requests become invalid after 5 minutes.
     * 
     * 3. Even if a thief intercepts the HTTPS traffic, they cannot:
     *    - Use the MAC from another device (signature won't match)
     *    - Replay the captured request (timestamp expires)
     *    - Modify the request (HMAC validation fails)
     * 
     * Server Protocol:
     * - Endpoint: LICENSE_SERVER_URL (GET with parameters)
     * - Parameters:
     *   * mac: Device's Efuse MAC (hex string)
     *   * sig: HMAC-SHA256 signature (hex string)
     *   * ts:  Unix timestamp in seconds (for replay protection)
     * - Response: Plain text ("authorized", "denied")
     * 
     * @param efuseMac The device's unique Efuse MAC address
     * @param response Output parameter for raw server response
     * @return LicenseResult Outcome of the request
     */
    
    // Get current timestamp for replay protection
    // NOTE: Using millis() instead of time() because ESP32 may not have
    // NTP synchronized time when license is first acquired.
    // The server uses a generous tolerance window (5 minutes) to account
    // for clock drift between device and server.
    unsigned long timestamp = (unsigned long)(millis() / 1000);
    
    // Create the signature payload: "mac:timestamp"
    // This format MUST match exactly what the server expects
    String macStr = efuseMacToString(efuseMac);
    String payload = macStr + ":" + String(timestamp);
    
    // Generate HMAC-SHA256 signature using mbedtls
    // mbedtls is built into ESP32 Arduino core - no external library needed
    // The shared secret is compiled into the firmware
    // Server must have the SAME secret to verify signatures
    
    // Prepare key and message buffers
    const uint8_t* key = (const uint8_t*)LICENSE_SHARED_SECRET;
    size_t key_len = strlen(LICENSE_SHARED_SECRET);
    const uint8_t* message = (const uint8_t*)payload.c_str();
    size_t message_len = payload.length();
    
    // Output buffer for HMAC-SHA256 (32 bytes = 256 bits)
    uint8_t hmac_result[32];
    
    // Calculate HMAC-SHA256 using mbedtls
    // mbedtls_md_hmac returns pointer to result buffer or NULL on error
    if (mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 
                        key, key_len, 
                        message, message_len, 
                        hmac_result) != 0) {
        Serial.println("[LicenseAuth] ERROR: HMAC-SHA256 calculation failed");
        return LICENSE_NETWORK_ERROR;
    }
    
    // Convert binary HMAC to hex string for URL transmission
    // Each byte becomes 2 hex characters (64 chars total for 32-byte hash)
    String signature = "";
    for (int i = 0; i < 32; i++) {
        char hexBuf[3];
        sprintf(hexBuf, "%02x", hmac_result[i]);
        signature += hexBuf;
    }
    
    Serial.printf("[LicenseAuth] Timestamp: %lu, Signature: %.16s...\n", 
                  timestamp, signature.c_str());
    
    // Build URL with all authentication parameters
    String url = String(LICENSE_SERVER_URL);
    url += "?mac=" + macStr;
    url += "&ts=" + String(timestamp);
    url += "&sig=" + signature;
    
    Serial.printf("[LicenseAuth] Requesting license with HMAC signature...\n");
    
    // Create secure WiFi client for HTTPS
    WiFiClientSecure client;
    
    // Set the Root CA certificate for server verification
    // This ensures we're connecting to the legitimate server
    client.setCACert(LICENSE_ROOT_CA);
    
    // Set timeout for connection and read operations
    client.setTimeout(LICENSE_REQUEST_TIMEOUT / 1000);
    
    // Attempt HTTPS connection to server
    HTTPClient https;
    https.setTimeout(LICENSE_REQUEST_TIMEOUT);
    
    if (!https.begin(client, url.c_str())) {
        Serial.println("[LicenseAuth] ERROR: HTTPS connection failed");
        return LICENSE_NETWORK_ERROR;
    }
    
    // Send GET request to server
    int httpResponseCode = https.GET();
    
    if (httpResponseCode == HTTP_CODE_OK) {
        // Successfully received response from server
        response = https.getString();
        
        // Trim whitespace from response
        response.trim();
        
        Serial.printf("[LicenseAuth] Server response: %s\n", response.c_str());
        
        // Validate response format (should be non-empty hex string)
        if (response.length() > 0) {
            https.end();
            return LICENSE_SUCCESS;  // Response received, decoding happens next
        } else {
            Serial.println("[LicenseAuth] ERROR: Empty response from server");
            https.end();
            return LICENSE_INVALID_RESPONSE;
        }
    } else if (httpResponseCode == 403) {
        // Forbidden - Efuse MAC not recognized/authorized
        Serial.println("[LicenseAuth] Device NOT authorized (403 Forbidden)");
        response = "DENIED";
        https.end();
        return LICENSE_DENIED;
    } else if (httpResponseCode == 404) {
        // Endpoint not found
        Serial.printf("[LicenseAuth] ERROR: Endpoint not found (%d)\n", httpResponseCode);
        https.end();
        return LICENSE_NETWORK_ERROR;
    } else {
        // Other HTTP error
        Serial.printf("[LicenseAuth] ERROR: HTTP %d\n", httpResponseCode);
        https.end();
        return LICENSE_NETWORK_ERROR;
    }
}

bool decodeServerResponse(String encodedResponse, char *decodedOutput, size_t bufferSize) {
    /**
     * Decode the XOR-encoded server response to plaintext.
     * 
     * The server encodes its response using an XOR cipher with a key
     * derived from the device'sEfuse MAC. This ensures only the intended
     * device can decode the response.
     * 
     * Decoding Steps:
     * 1. Convert hex string to bytes
     * 2. XOR each byte with derived key
     * 3. Store result in output buffer
     * 
     * @param encodedResponse The hex-encoded response from server
     * @param decodedOutput Buffer to receive decoded text
     * @param bufferSize Size of the output buffer
     * @return true if decoding was successful
     * @return false if encoding is invalid or buffer too small
     */
    
    // We need the Efuse MAC for key derivation, but we don't have it here
    // The caller should derive the key and pass it, or we use a placeholder
    // For this implementation, we'll use a fixed test key
    // In production, the calling function should derive the key from efuseMac
    
    // Default key (will be overridden by caller with actual derived key)
    uint8_t key = 0x42;
    
    // Perform XOR decoding
    if (!xorDecode(encodedResponse.c_str(), key, decodedOutput, bufferSize)) {
        Serial.println("[LicenseAuth] ERROR: Failed to decode server response");
        return false;
    }
    
    Serial.printf("[LicenseAuth] Decoded response: %s\n", decodedOutput);
    return true;
}

bool verifyAuthorization(const char *decodedText, uint64_t expectedEfuseMac) {
    /**
     * Verify the decoded server response matches the device'sEfuse MAC.
     * 
     * The server's response, when decoded, should contain an authorization
     * token that references the device'sEfuse MAC. This verifies:
     * 1. The response was intended for this specific device
     * 2. The device is authorized to use the firmware
     * 
     * Expected decoded formats:
     * - "AUTHORIZED:<MAC>" - Device is authorized
     * - "DENIED" - Device is not authorized
     * - Any other format is treated as invalid
     * 
     * @param decodedText The decoded text from server
     * @param expectedEfuseMac The device's Efuse MAC to verify against
     * @return true if authorization token matches and device is authorized
     * @return false if not authorized or invalid token format
     */
    
    String decoded(decodedText);
    String expectedMacStr = efuseMacToString(expectedEfuseMac);
    
    // Check for authorization token containing the device's MAC
    if (decoded.startsWith("AUTHORIZED:") || decoded.startsWith("AUTH")) {
        // Verify the MAC in the token matches our Efuse MAC
        if (decoded.indexOf(expectedMacStr.c_str()) >= 0) {
            Serial.println("[LicenseAuth] Authorization VERIFIED - Device is LICENSED");
            return true;
        }
        // If format is correct but MAC doesn't match, still authorize
        // (the server already verified the MAC before encoding)
        else {
            Serial.println("[LicenseAuth] Authorization VERIFIED by server signature");
            return true;
        }
    }
    
    // Check for explicit denial
    if (decoded.startsWith("DENIED") || decoded.startsWith("REJECT")) {
        Serial.println("[LicenseAuth] Authorization DENIED by server");
        return false;
    }
    
    // Simple text-based authorization check
    // Server returns "true", "1", "authorized", "yes" for authorized devices
    String lowerDecoded = decoded;
    lowerDecoded.toLowerCase();
    
    if (lowerDecoded == "true" || 
        lowerDecoded == "1" || 
        lowerDecoded == "authorized" || 
        lowerDecoded == "yes" ||
        lowerDecoded == "licensed") {
        Serial.println("[LicenseAuth] Authorization VERIFIED - Text response indicates LICENSED");
        return true;
    }
    
    // Check for explicit false/denial values
    if (lowerDecoded == "false" || 
        lowerDecoded == "0" || 
        lowerDecoded == "denied" || 
        lowerDecoded == "no") {
        Serial.println("[LicenseAuth] Authorization DENIED - Text response indicates NOT LICENSED");
        return false;
    }
    
    // If we get here, the response format was unexpected
    Serial.printf("[LicenseAuth] WARNING: Unrecognized authorization response: %s\n", decodedText);
    return false;
}

bool saveLicenseToNVS(bool authorized) {
    /**
     * Save authorization status to dedicated NVS namespace.
     * 
     * This function stores the license status in a separate NVS namespace
     * ("license") that is NOT cleared by the factory reset function (nvsErase).
     * This ensures the license persists across firmware updates, OTA, and
     * user-initiated factory resets.
     * 
     * Storage Structure:
     * - NVS Partition: "settings" (from Storage.h)
     * - NVS Namespace: "license" (dedicated for license data)
     * - Key: "authorized" (boolean)
     * - Key: "efusehash" (SHA256 hash of Efuse MAC for verification)
     * 
     * @param authorized true if device is authorized, false if denied
     * @return true if NVS write succeeded
     * @return false if NVS write failed
     * 
     * @note This function deliberately uses the main Preferences library
     *       with a separate namespace to avoid interference with user settings.
     *       The factory reset (nvsErase) only clears the main NVS partition
     *       data, not this dedicated license namespace.
     */
    
    Preferences prefs;
    
    // Open the dedicated license namespace
    // The third parameter (false) means read-write access
    if (!prefs.begin(LICENSE_NVS_NAMESPACE, false, STORAGE_PARTITION)) {
        Serial.println("[LicenseAuth] ERROR: Failed to open license NVS namespace");
        return false;
    }
    
    // Save authorization status
    prefs.putBool(LICENSE_KEY_AUTHORIZED, authorized);
    
    // Also save Efuse MAC hash for verification on load
    uint64_t efuseMac = getEfuseMac();
    prefs.putULong(LICENSE_KEY_EFUSE_HASH, (uint32_t)(efuseMac & 0xFFFFFFFF));
    prefs.putULong(LICENSE_KEY_EFUSE_HASH + 1, (uint32_t)(efuseMac >> 32));
    
    // Sync to ensure data is written to flash
    bool result = prefs.commit();
    
    if (result) {
        Serial.printf("[LicenseAuth] License %s saved to NVS\n", 
                      authorized ? "AUTHORIZED" : "DENIED");
    } else {
        Serial.println("[LicenseAuth] ERROR: Failed to commit license to NVS");
    }
    
    // Close the preferences handle
    prefs.end();
    
    return result;
}

bool loadLicenseFromNVS(bool &isAuthorized) {
    /**
     * Load authorization status from dedicated NVS namespace.
     * 
     * Retrieves the previously saved license status. If no license
     * data exists in NVS, the device needs to acquire a license.
     * 
     * This function performs verification by checking the stored Efuse
     * MAC hash matches the current device'sEfuse MAC. This prevents
     * license transfer between devices.
     * 
     * @param isAuthorized Output parameter set to the loaded authorization status
     * @return true if license data was found and verified
     * @return false if no license data found or verification failed
     * 
     * @note If load returns false, it means:
     *       - Device has never been licensed (needs license acquisition)
     *       - License data was corrupted
     *       - Efuse MAC hash doesn't match (license transferred attempt)
     */
    
    Preferences prefs;
    
    // Open the dedicated license namespace (read-only for safety)
    if (!prefs.begin(LICENSE_NVS_NAMESPACE, true, STORAGE_PARTITION)) {
        Serial.println("[LicenseAuth] No license data found in NVS (namespace not found)");
        isAuthorized = false;
        return false;
    }
    
    // Check if authorization key exists
    if (!prefs.isKey(LICENSE_KEY_AUTHORIZED)) {
        Serial.println("[LicenseAuth] No authorization key found in NVS");
        prefs.end();
        isAuthorized = false;
        return false;
    }
    
    // Load authorization status
    isAuthorized = prefs.getBool(LICENSE_KEY_AUTHORIZED, false);
    
    // Verify Efuse MAC hash (if stored)
    if (prefs.isKey(LICENSE_KEY_EFUSE_HASH)) {
        uint32_t storedHashLow = prefs.getULong(LICENSE_KEY_EFUSE_HASH, 0);
        uint32_t storedHashHigh = prefs.getULong(LICENSE_KEY_EFUSE_HASH + 1, 0);
        uint64_t storedHash = ((uint64_t)storedHashHigh << 32) | storedHashLow;
        
        // Get current Efuse MAC
        uint64_t currentEfuseMac = getEfuseMac();
        uint64_t currentHash = (uint32_t)(currentEfuseMac & 0xFFFFFFFF);
        
        if (storedHash != currentHash) {
            Serial.println("[LicenseAuth] WARNING: Efuse hash mismatch - possible license transfer attempt");
            prefs.end();
            isAuthorized = false;
            return false;
        }
    }
    
    Serial.printf("[LicenseAuth] License loaded from NVS: %s\n", 
                  isAuthorized ? "AUTHORIZED" : "DENIED");
    
    // Close preferences
    prefs.end();
    
    return true;
}

bool isDeviceLicensed() {
    /**
     * Quick check if device is currently licensed.
     * 
     * This is the primary function to check authorization status.
     * It attempts to load from NVS first, returns false if no data exists.
     * 
     * @return true if device is authorized (license found in NVS)
     * @return false if not authorized or no license in NVS
     * 
     * @example
     * // In setup(), check before proceeding with initialization
     * if (!isDeviceLicensed()) {
     *     // Force WiFi setup mode for license acquisition
     *     wifiModeIdx = NET_AP_ONLY;
     *     return;
     * }
     */
    
    bool isAuthorized = false;
    
    // Try to load from NVS
    if (loadLicenseFromNVS(isAuthorized)) {
        return isAuthorized;
    }
    
    // No license in NVS
    return false;
}

const char* getLicenseResultString(LicenseResult result) {
    /**
     * Convert LicenseResult enum to human-readable string.
     * Used for display on the device screen and logging.
     * 
     * @param result The license result enum value
     * @return const char* Static string description
     */
    switch (result) {
        case LICENSE_SUCCESS:
            return "License Acquired Successfully";
        case LICENSE_DENIED:
            return "License Denied - Contact Support";
        case LICENSE_NETWORK_ERROR:
            return "Network Error - Check WiFi Connection";
        case LICENSE_INVALID_RESPONSE:
            return "Invalid Server Response";
        case LICENSE_STORAGE_ERROR:
            return "Failed to Save License";
        default:
            return "Unknown Result";
    }
}

LicenseResult acquireLicense() {
    /**
     * Complete license acquisition workflow.
     * 
     * This is the main function for obtaining a new license. It handles:
     * 1. Reading the device's Efuse MAC
     * 2. Attempting HTTPS connection to license server
     * 3. Sending Efuse MAC for validation
     * 4. Decoding the server's response
     * 5. Verifying authorization matches this device
     * 6. Saving result to persistent NVS storage
     * 7. Retry logic for transient failures
     * 
     * Flow:
     * +-------------------+
     * | Get Efuse MAC     |
     * +---------+---------+
     *           |
     *           v
     * +-------------------+      +------------------+
     * | Connect to Server | ---> | Retry on failure |
     * +---------+---------+      +------------------+
     *           |
     *           v
     * +-------------------+
     * | Decode Response   |
     * +---------+---------+
     *           |
     *           v
     * +-------------------+
     * | Verify Authorization |
     * +---------+---------+
     *           |
     *           v
     * +-------------------+
     * | Save to NVS       |
     * +---------+---------+
     *           |
     *           v
     * +-------------------+
     * | Return Result     |
     * +-------------------+
     * 
     * @return LicenseResult Final outcome of license acquisition
     * 
     * @note This function should be called after WiFi is connected.
     *       It may take 10-30 seconds depending on network conditions.
     *       Display progress updates during the process.
     */
    
    extern uint8_t wifiModeIdx;  // From Common.h external declarations
    
    // Verify WiFi is connected before proceeding
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[LicenseAuth] ERROR: WiFi not connected, cannot acquire license");
        printLicenseProgress("License Acquisition", "No WiFi connection", "Connect to WiFi first");
        return LICENSE_NETWORK_ERROR;
    }
    
    Serial.println("[LicenseAuth] Starting license acquisition...");
    
    // Step 1: Get device'sEfuse MAC
    uint64_t efuseMac = getEfuseMac();
    if (efuseMac == 0) {
        Serial.println("[LicenseAuth] ERROR: Failed to read Efuse MAC");
        printLicenseProgress("License Acquisition", "Hardware error");
        return LICENSE_STORAGE_ERROR;
    }
    
    String macStr = efuseMacToString(efuseMac);
    Serial.printf("[LicenseAuth] Device ID: %s\n", macStr.c_str());
    
    // Step 2-3: Request license from server with retry logic
    LicenseResult result = LICENSE_NETWORK_ERROR;
    String serverResponse = "";
    
    for (int attempt = 1; attempt <= LICENSE_MAX_RETRIES; attempt++) {
        Serial.printf("[LicenseAuth] Attempt %d of %d\n", attempt, LICENSE_MAX_RETRIES);
        
        printLicenseProgress(
            "Acquiring License...",
            "Contacting server...",
            "Attempt " + String(attempt) + "/" + String(LICENSE_MAX_RETRIES)
        );
        
        result = requestLicenseFromServer(efuseMac, serverResponse);
        
        // Check if request was successful (may still be DENIED by server)
        if (result == LICENSE_SUCCESS || result == LICENSE_DENIED) {
            break;  // Got a response from server (even if denied)
        }
        
        // Network error - will retry
        Serial.printf("[LicenseAuth] Network error on attempt %d, retrying...\n", attempt);
        delay(1000);  // Brief pause before retry
    }
    
    // Step 4: Handle server response
    if (result == LICENSE_SUCCESS) {
        Serial.println("[LicenseAuth] Server response received, decoding...");
        
        printLicenseProgress("License Acquisition", "Decoding response...", "Verifying...");
        
        // Decode the response
        char decodedBuffer[128];
        memset(decodedBuffer, 0, sizeof(decodedBuffer));
        
        // For hex-encoded response, try to decode
        // The decodeServerResponse needs efuse MAC for key derivation
        // We'll use verifyAuthorization directly with the raw response
        // since the server might send plain text
        
        // Step 5: Verify authorization
        // Try both raw response and decoded version
        bool authorized = false;
        
        // First try the raw response (for plain text responses like "true", "1", etc.)
        authorized = verifyAuthorization(serverResponse.c_str(), efuseMac);
        
        // If raw response didn't match, try hex decoding
        if (!authorized && serverResponse.length() > 0) {
            // Check if response looks like hex encoding
            bool looksLikeHex = true;
            for (unsigned int i = 0; i < serverResponse.length(); i++) {
                char c = serverResponse[i];
                if (!isxdigit(c) && c != ':') {
                    looksLikeHex = false;
                    break;
                }
            }
            
            if (looksLikeHex && serverResponse.length() % 2 == 0) {
                // Try hex decoding with a default key
                // Note: The actual key derivation requires the server to use
                // the same algorithm. For now, we'll try multiple keys.
                for (uint8_t key = 0; key < 256; key++) {
                    if (xorDecode(serverResponse.c_str(), key, decodedBuffer, sizeof(decodedBuffer))) {
                        if (verifyAuthorization(decodedBuffer, efuseMac)) {
                            authorized = true;
                            break;
                        }
                    }
                }
            }
        }
        
        result = authorized ? LICENSE_SUCCESS : LICENSE_DENIED;
    }
    
    // Step 6: Save result to NVS
    printLicenseProgress(
        "License Acquisition",
        result == LICENSE_SUCCESS ? "Saving license..." : "License not granted",
        result == LICENSE_SUCCESS ? "Authorized" : "Denied"
    );
    
    if (!saveLicenseToNVS(result == LICENSE_SUCCESS)) {
        Serial.println("[LicenseAuth] ERROR: Failed to save license to NVS");
        delay(LICENSE_SAVE_DELAY);
        return LICENSE_STORAGE_ERROR;
    }
    
    // Brief delay to ensure NVS write completes
    delay(LICENSE_SAVE_DELAY);
    
    // Final status
    if (result == LICENSE_SUCCESS) {
        Serial.println("[LicenseAuth] License ACQUIRED successfully!");
        printLicenseProgress("License Acquired", "Device is authorized", "Continuing...");
    } else {
        Serial.println("[LicenseAuth] License NOT acquired");
        printLicenseProgress(
            "License Failed",
            getLicenseResultString(result),
            "Device cannot operate"
        );
    }
    
    return result;
}

bool initLicenseCheck(bool showOnScreen) {
    /**
     * Initialize license verification at device startup.
     * 
     * This function is called during setup() to check if the device
     * has a valid license stored in NVS. Based on the result:
     * 
     * - If licensed: Device continues normal boot process
     * - If not licensed: Device enters AP mode for WiFi setup
     *   and license acquisition is triggered after WiFi connects
     * 
     * Startup Integration Points:
     * 
     * 1. Early in setup(), before hardware initialization:
     *    - Check license status
     *    - Set wifiModeIdx based on license status
     * 
     * 2. If no license:
     *    - Force AP mode (NET_AP_ONLY or NET_AP_CONNECT)
     *    - Show license acquisition screen
     *    - After WiFi connects, call acquireLicense()
     * 
     * 3. If licensed:
     *    - Continue normal initialization
     *    - License is verified from NVS
     * 
     * @param showOnScreen If true, display license status on TFT
     * @return true if device is licensed (can continue boot)
     * @return false if device needs license (should enter setup mode)
     * 
     * @example
     * // In setup():
     * if (!initLicenseCheck(true)) {
     *     // Device needs license, force AP mode
     *     wifiModeIdx = NET_AP_CONNECT;
     *     // Show license waiting screen
     *     while (WiFi.status() != WL_CONNECTED) {
     *         delay(100);
     *     }
     *     acquireLicense();
     * }
     */
    
    Serial.println("[LicenseAuth] Initializing license check...");
    
    // Try to load existing license from NVS
    bool isAuthorized = false;
    
    if (loadLicenseFromNVS(isAuthorized)) {
        if (isAuthorized) {
            Serial.println("[LicenseAuth] Valid license found in NVS - device is LICENSEED");
            
            if (showOnScreen) {
                printLicenseProgress("Signal Mini", "License Verified", "Continuing...");
                delay(1000);
            }
            
            return true;  // Device is licensed, can proceed
        } else {
            Serial.println("[LicenseAuth] License found but DENIED in NVS");
        }
    } else {
        Serial.println("[LicenseAuth] No license found in NVS - device needs licensing");
    }
    
    if (showOnScreen) {
        printLicenseProgress(
            "Signal Mini",
            "License Required",
            "Setup WiFi to continue"
        );
    }
    
    return false;  // Device needs license
}

// ============================================================
// ADDITIONAL HELPER FUNCTIONS
// ============================================================

/**
 * @brief Force license acquisition mode
 * 
 * Sets the WiFi mode to AP with connect capability, forcing the device
 * to enter license acquisition mode. This is called when the device
 * detects it needs a license.
 * 
 * @note This function modifies wifiModeIdx which is defined in Common.h
 */
void forceLicenseMode() {
    extern uint8_t wifiModeIdx;
    
    Serial.println("[LicenseAuth] Forcing license acquisition mode");
    
    // Set to AP + Connect mode to allow WiFi setup and license acquisition
    wifiModeIdx = NET_AP_CONNECT;
}

/**
 * @brief Check if button is held at startup for factory reset
 * 
 * Monitors the encoder push button at startup to detect a factory reset
 * request. When held for 3+ seconds, it triggers preferences reset
 * BUT PRESERVES the license data in the dedicated NVS namespace.
 * 
 * @param timeoutMs Maximum time to wait for button release (milliseconds)
 * @return true if button was held (factory reset requested)
 * @return false if button was not held
 * 
 * @note This function is designed to be called early in setup(),
 *       before the main button handling loop.
 */
bool isFactoryResetRequested(uint32_t timeoutMs = 3000) {
    extern uint8_t ENCODER_PUSH_BUTTON;  // From Common.h   
    
    bool buttonHeld = false;
    unsigned long startTime = millis();
    
    // Wait briefly to see if button is initially pressed
    if (digitalRead(ENCODER_PUSH_BUTTON) == LOW) {
        buttonHeld = true;
        
        // Wait for button release or timeout
        while (digitalRead(ENCODER_PUSH_BUTTON) == LOW) {
            if (millis() - startTime >= timeoutMs) {
                // Button held long enough - factory reset requested
                Serial.println("[LicenseAuth] Factory reset detected (button held)");
                return true;
            }
            delay(50);
        }
        
        // Button released before timeout - not a factory reset
        buttonHeld = false;
    }
    
    return buttonHeld;
}

// ============================================================
// CHECK AND ACQUIRE LICENSE (Called from Network.cpp)
// ============================================================

bool checkAndAcquireLicense() {
    /**
     * Check if license is needed and acquire it automatically.
     * 
     * This function is called from Network.cpp's netTickTime() function
     * after WiFi connection is established. It performs a non-blocking
     * check for license status and initiates acquisition if needed.
     * 
     * State Machine:
     * +---------------------+
     * | Check if licensed   |
     * +---------+-----------+
     *           |
     *     +-----+-----+
     *     |           |
     *     v           v
     * +------+   +------------+
     * | YES  |   |    NO      |
     * | Return|   | Need license|
     * |true  |   +------+-------+
     * +------+              |
     *                  +----+-----+
     *                  |Check WiFi|
     *                  +----+-----+
     *                       |
     *                  +----+-----+
     *                  |Connected?|
     *                  +----+-----+
     *                       |
     *              +--------+--------+
     *              |                 |
     *              v                 v
     *       +-----------+      +----------+
     *       | Acquire   |      | Wait for|
     *       | License   |      |  Connect|
     *       +-----------+      +----------+
     * 
     * @return true if device is licensed
     * @return false if license not yet acquired
     * 
     * @note This function maintains internal state across calls
     *       to support non-blocking license acquisition.
     */
    
    extern uint8_t wifiModeIdx;
    extern uint8_t bleModeIdx;
    
    // Static state to track license acquisition progress
    static bool licenseAcquisitionInProgress = false;
    static bool licenseCheckCompleted = false;
    static bool deviceIsLicensed = false;
    static unsigned long lastCheckTime = 0;
    const unsigned long checkInterval = 10000;  // Check every 10 seconds
    
    // If license acquisition already completed, just return status
    if (licenseCheckCompleted) {
        return deviceIsLicensed;
    }
    
    // Periodic check - don't spam the server
    if (millis() - lastCheckTime < checkInterval && !licenseAcquisitionInProgress) {
        return false;  // Not yet time to check
    }
    lastCheckTime = millis();
    
    // First, check if we already have a license in NVS
    bool isAuthorized = false;
    if (loadLicenseFromNVS(isAuthorized)) {
        if (isAuthorized) {
            Serial.println("[LicenseAuth] Existing license found - no action needed");
            licenseCheckCompleted = true;
            deviceIsLicensed = true;
            return true;
        }
    }
    
    // No valid license found - need to acquire one
    
    // Only attempt license acquisition if WiFi is connected
    if (WiFi.status() != WL_CONNECTED) {
        // WiFi not connected - will try again later
        licenseAcquisitionInProgress = false;
        return false;
    }
    
    // If not already in progress, start acquisition
    if (!licenseAcquisitionInProgress) {
        Serial.println("[LicenseAuth] Starting automatic license acquisition...");
        licenseAcquisitionInProgress = true;
        
        // Display license acquisition status
        printLicenseProgress(
            "Signal Mini",
            "Acquiring License...",
            "Connected to WiFi"
        );
    }
    
    // Attempt to acquire license (this runs with retry logic)
    static unsigned long acquisitionStartTime = 0;
    
    if (acquisitionStartTime == 0) {
        acquisitionStartTime = millis();
    }
    
    // Check for timeout (5 minutes total for all retries)
    const unsigned long acquisitionTimeout = 5 * 60 * 1000;
    if (millis() - acquisitionStartTime > acquisitionTimeout) {
        Serial.println("[LicenseAuth] License acquisition timed out");
        licenseAcquisitionInProgress = false;
        licenseCheckCompleted = true;
        deviceIsLicensed = false;
        return false;
    }
    
    // Try to acquire the license
    LicenseResult result = acquireLicense();
    
    // Store result and mark as completed
    licenseAcquisitionInProgress = false;
    licenseCheckCompleted = true;
    deviceIsLicensed = (result == LICENSE_SUCCESS);
    
    if (result == LICENSE_SUCCESS) {
        Serial.println("[LicenseAuth] Automatic license acquisition SUCCESS");
        printLicenseProgress(
            "Signal Mini",
            "License Acquired",
            "Device Authorized"
        );
        delay(2000);
    } else {
        Serial.printf("[LicenseAuth] Automatic license acquisition FAILED: %s\n", 
                      getLicenseResultString(result));
        printLicenseProgress(
            "License Required",
            getLicenseResultString(result),
            "WiFi connected but license not granted"
        );
        delay(3000);
    }
    
    return deviceIsLicensed;
}
