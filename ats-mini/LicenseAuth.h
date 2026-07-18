/**
 * @file LicenseAuth.h
 * @brief License Authentication System for Signal Mini Device
 * 
 * This module implements a hardware-based license authentication system
 * that prevents code cloning and unauthorized copying of the firmware.
 * 
 * How it works:
 * 1. Each ESP32 chip has a unique Efuse MAC address (hardware fingerprint)
 * 2. The device sends its EfuseMac to a secure web server via HTTPS
 * 3. The server validates the EfuseMac and returns an authorized response
 * 4. The device decodes the response and stores authorization status in NVS
 * 5. The stored authorization persists even after reprogramming or partition changes
 * 
 * Setup Flow:
 * - On first boot or after preferences reset, device starts in AP mode
 * - User connects to AP and accesses WiFi settings page
 * - User configures WiFi credentials (Station mode)
 * - Device connects to WiFi and automatically requests license from server
 * - License status is saved to NVS and device continues normal operation
 * 
 * Security Notes:
 * - Authorization is stored in a dedicated NVS namespace that is NOT
 *   cleared by nvsErase() which is called during factory reset
 * - This ensures license persists across firmware updates and reprogramming
 * - The button-hold factory reset only clears user preferences, not license
 */

#ifndef LICENSE_AUTH_H
#define LICENSE_AUTH_H

#include <stdint.h>
#include <string>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Preferences.h>
// #include <esp_efuse_mac.h>
#include <esp_mac.h>

// mbedtls MD header for HMAC-SHA256 (built into ESP32 Arduino core)
#include "mbedtls/md.h"

// ============================================================
// CONFIGURATION CONSTANTS
// ============================================================

/**
 * @brief NVS namespace for license storage
 * 
 * This dedicated namespace ensures license data is stored separately
 * from user preferences and survives factory reset operations.
 */
#define LICENSE_NVS_NAMESPACE "license"

/**
 * @brief NVS key for storing authorization status
 * 
 * Stores a boolean value (true/false) indicating whether the device
 * is authorized to operate.
 */
#define LICENSE_KEY_AUTHORIZED "authorized"

/**
 * @brief NVS key for storing the device'sEfuse MAC hash
 * 
 * Stores a hash of the device's Efuse MAC for verification purposes.
 */
#define LICENSE_KEY_EFUSE_HASH "efusehash"

/**
 * @brief HTTPS URL for license validation
 * 
 * The server endpoint that receives the device's EfuseMac and returns
 * an authorization response. Must return text that can be decoded.
 */
// #define LICENSE_SERVER_URL "https://khrh.ir/signal-mini-update/ota/license_verify" // keyhan (khrh)
#define LICENSE_SERVER_URL "https://lozelab.ir/license_verify" // lozelab
/**
 * @brief Shared secret key for HMAC signature generation
 * 
 * This secret is shared between the device and server. It's used to create
 * an HMAC-SHA256 signature that proves the request came from a legitimate
 * device (not a thief who intercepted traffic).
 * 
 * SECURITY: The MAC address alone is visible in HTTPS requests to the server.
 * However, the HMAC signature prevents thieves from:
 * 1. Replaying the MAC (server verifies signature matches)
 * 2. Spoofing another device's MAC (signature won't match)
 * 3. Tampering with the request (HMAC validation fails)
 * 
 * IMPORTANT: Change this key for production! Use a random 32-byte key.
 * The same key must be configured on the server.
 */
#define LICENSE_SHARED_SECRET "signalMiniSecret2024_KeyChanG3"

/**
 * @brief Request timestamp (used for HMAC to prevent replay attacks)
 * 
 * Current milliseconds since boot. Used to create unique signatures.
 */

/**
 * @brief Root CA certificate for server authentication
 * 
 * PEM-encoded Root CA certificate used to verify the server's SSL
 * certificate. This prevents man-in-the-middle attacks.
 */
// KHRH (keyhan) root_CA
// const char LICENSE_ROOT_CA[] PROGMEM = R"EOF(-----BEGIN CERTIFICATE-----
// MIICGzCCAaGgAwIBAgIQQdKd0XLq7qeAwSxs6S+HUjAKBggqhkjOPQQDAzBPMQsw
// CQYDVQQGEwJVUzEpMCcGA1UEChMgSW50ZXJuZXQgU2VjdXJpdHkgUmVzZWFyY2gg
// R3JvdXAxFTATBgNVBAMTDElTUkcgUm9vdCBYMjAeFw0yMDA5MDQwMDAwMDBaFw00
// MDA5MTcxNjAwMDBaME8xCzAJBgNVBAYTAlVTMSkwJwYDVQQKEyBJbnRlcm5ldCBT
// ZWN1cml0eSBSZXNlYXJjaCBHcm91cDEVMBMGA1UEAxMMSVNSRyBSb290IFgyMHYw
// EAYHKoZIzj0CAQYFK4EEACIDYgAEzZvVn4CDCuwJSvMWSj5cz3es3mcFDR0HttwW
// +1qLFNvicWDEukWVEYmO6gbf9yoWHKS5xcUy4APgHoIYOIvXRdgKam7mAHf7AlF9
// ItgKbppbd9/w+kHsOdx1ymgHDB/qo0IwQDAOBgNVHQ8BAf8EBAMCAQYwDwYDVR0T
// AQH/BAUwAwEB/zAdBgNVHQ4EFgQUfEKWrt5LSDv6kviejM9ti6lyN5UwCgYIKoZI
// zj0EAwMDaAAwZQIwe3lORlCEwkSHRhtFcP9Ymd70/aTSVaYgLXTWNLxBo1BfASdW
// tL4ndQavEi51mI38AjEAi/V3bNTIZargCyzuFJ0nN6T5U6VR5CmD1/iQMVtCnwr1
// /q4AaOeMSQ+2b1tbFfLn
// -----END CERTIFICATE-----)EOF";

// lozelab root_CA
const char LICENSE_ROOT_CA[] PROGMEM = R"EOF(-----BEGIN CERTIFICATE-----
MIIFazCCA1OgAwIBAgIRAIIQz7DSQONZRGPgu2OCiwAwDQYJKoZIhvcNAQELBQAw
TzELMAkGA1UEBhMCVVMxKTAnBgNVBAoTIEludGVybmV0IFNlY3VyaXR5IFJlc2Vh
cmNoIEdyb3VwMRUwEwYDVQQDEwxJU1JHIFJvb3QgWDEwHhcNMTUwNjA0MTEwNDM4
WhcNMzUwNjA0MTEwNDM4WjBPMQswCQYDVQQGEwJVUzEpMCcGA1UEChMgSW50ZXJu
ZXQgU2VjdXJpdHkgUmVzZWFyY2ggR3JvdXAxFTATBgNVBAMTDElTUkcgUm9vdCBY
MTCCAiIwDQYJKoZIhvcNAQEBBQADggIPADCCAgoCggIBAK3oJHP0FDfzm54rVygc
h77ct984kIxuPOZXoHj3dcKi/vVqbvYATyjb3miGbESTtrFj/RQSa78f0uoxmyF+
0TM8ukj13Xnfs7j/EvEhmkvBioZxaUpmZmyPfjxwv60pIgbz5MDmgK7iS4+3mX6U
A5/TR5d8mUgjU+g4rk8Kb4Mu0UlXjIB0ttov0DiNewNwIRt18jA8+o+u3dpjq+sW
T8KOEUt+zwvo/7V3LvSye0rgTBIlDHCNAymg4VMk7BPZ7hm/ELNKjD+Jo2FR3qyH
B5T0Y3HsLuJvW5iB4YlcNHlsdu87kGJ55tukmi8mxdAQ4Q7e2RCOFvu396j3x+UC
B5iPNgiV5+I3lg02dZ77DnKxHZu8A/lJBdiB3QW0KtZB6awBdpUKD9jf1b0SHzUv
KBds0pjBqAlkd25HN7rOrFleaJ1/ctaJxQZBKT5ZPt0m9STJEadao0xAH0ahmbWn
OlFuhjuefXKnEgV4We0+UXgVCwOPjdAvBbI+e0ocS3MFEvzG6uBQE3xDk3SzynTn
jh8BCNAw1FtxNrQHusEwMFxIt4I7mKZ9YIqioymCzLq9gwQbooMDQaHWBfEbwrbw
qHyGO0aoSCqI3Haadr8faqU9GY/rOPNk3sgrDQoo//fb4hVC1CLQJ13hef4Y53CI
rU7m2Ys6xt0nUW7/vGT1M0NPAgMBAAGjQjBAMA4GA1UdDwEB/wQEAwIBBjAPBgNV
HRMBAf8EBTADAQH/MB0GA1UdDgQWBBR5tFnme7bl5AFzgAiIyBpY9umbbjANBgkq
hkiG9w0BAQsFAAOCAgEAVR9YqbyyqFDQDLHYGmkgJykIrGF1XIpu+ILlaS/V9lZL
ubhzEFnTIZd+50xx+7LSYK05qAvqFyFWhfFQDlnrzuBZ6brJFe+GnY+EgPbk6ZGQ
3BebYhtF8GaV0nxvwuo77x/Py9auJ/GpsMiu/X1+mvoiBOv/2X/qkSsisRcOj/KK
NFtY2PwByVS5uCbMiogziUwthDyC3+6WVwW6LLv3xLfHTjuCvjHIInNzktHCgKQ5
ORAzI4JMPJ+GslWYHb4phowim57iaztXOoJwTdwJx4nLCgdNbOhdjsnvzqvHu7Ur
TkXWStAmzOVyyghqpZXjFaH3pO3JLF+l+/+sKAIuvtd7u+Nxe5AW0wdeRlN8NwdC
jNPElpzVmbUq4JUagEiuTDkHzsxHpFKVK7q4+63SM1N95R1NbdWhscdCb+ZAJzVc
oyi3B43njTOQ5yOf+1CceWxG1bQVs5ZufpsMljq4Ui0/1lvh+wjChP4kqKOJ2qxq
4RgqsahDYVvTH9w7jXbyLeiNdd8XM2w9U/t7y0Ff/9yi0GE44Za4rF2LN9d11TPA
mRGunUHBcnWEvgJBQl9nJEiU0Zsnvgc/ubhPgXRR4Xq37Z0j4r7g1SgEEzwxA57d
emyPxgcYxn/eR44/KJ4EBs+lVDR3veyJm+kXQ99b21/+jh5Xos1AnX5iItreGCc=
-----END CERTIFICATE-----)EOF";

/**
 * @brief Timeout for HTTPS license request (milliseconds)
 * 
 * Maximum time to wait for the server response before giving up.
 */
#define LICENSE_REQUEST_TIMEOUT 30000

/**
 * @brief Maximum retry attempts for license request
 * 
 * Number of times to retry the license request on failure.
 */
#define LICENSE_MAX_RETRIES 3

/**
 * @brief Delay after successful license acquisition (milliseconds)
 * 
 * Brief delay after saving license to allow NVS write to complete.
 */
#define LICENSE_SAVE_DELAY 500

// ============================================================
// TYPE DEFINITIONS
// ============================================================

/**
 * @brief Authorization result from server
 * 
 * Enumerates possible outcomes of the license validation process.
 */
typedef enum {
    LICENSE_SUCCESS,        ///< Device is authorized and licensed
    LICENSE_DENIED,         ///< Device is NOT authorized (EfuseMac not recognized)
    LICENSE_NETWORK_ERROR,  ///< Could not connect to server (no WiFi, DNS failed, etc.)
    LICENSE_INVALID_RESPONSE, ///< Server returned unexpected response format
    LICENSE_STORAGE_ERROR   ///< Failed to save/load license from NVS
} LicenseResult;

// ============================================================
// FUNCTION DECLARATIONS
// ============================================================

/**
 * @brief Get the device's unique Efuse MAC address
 * 
 * Reads the ESP32's factory-assigned MAC address from eFuse hardware
 * storage. This is a unique hardware identifier that serves as the
 * device's fingerprint for license validation.
 * 
 * @return uint64_t Lower 48 bits of the Efuse MAC address, or 0 if unavailable
 * 
 * @note This value is burned into the ESP32 during manufacturing
 *       and cannot be modified, making it ideal for device identification.
 */
uint64_t getEfuseMac();

/**
 * @brief Convert Efuse MAC to a hex string representation
 * 
 * Formats the 64-bit Efuse MAC address as a readable hexadecimal string.
 * This is used for logging and debugging purposes.
 * 
 * @param efuseMac The 64-bit Efuse MAC address
 * @return String Hexadecimal representation of the MAC address
 * 
 * @example
 * uint64_t mac = getEfuseMac();
 * String macStr = efuseMacToString(mac);
 * // Output: "545E209E139C"
 */
String efuseMacToString(uint64_t efuseMac);

/**
 * @brief Send EfuseMac to license server for validation
 * 
 * Establishes a secure HTTPS connection to the license server and sends
 * the device's Efuse MAC address. The server responds with an authorization
 * token that must be decoded.
 * 
 * @param efuseMac The device's Efuse MAC address to validate
 * @param response Output parameter for the server's raw response text
 * @return LicenseResult Result of the license request operation
 * 
 * @details
 * Server Protocol:
 * - Request: POST with query parameter ?mac=<hex_mac_address>
 * - Response: Plain text containing an encoded authorization token
 * - Token Format: The server returns text that, when decoded, contains
 *   the device'sEfuse MAC if authorized, or an error code if not
 * 
 * @example
 * String response;
 * LicenseResult result = requestLicenseFromServer(getEfuseMac(), response);
 * if (result == LICENSE_SUCCESS) {
 *     // Process the response
 * }
 */
LicenseResult requestLicenseFromServer(uint64_t efuseMac, String &response);

/**
 * @brief Decode the server's authorization response
 * 
 * Decodes the text response from the license server and extracts
 * the authorization information. The encoding scheme uses a simple
 * XOR-based cipher for obfuscation.
 * 
 * @param encodedResponse The raw text response from the server
 * @param decodedOutput Output buffer for the decoded text
 * @param bufferSize Size of the output buffer
 * @return true if the response was successfully decoded and contains
 *         a matching authorization token
 * @return false if the response is invalid or doesn't match
 * 
 * @details
 * Decoding Algorithm:
 * 1. Each character in the response is XORed with a key derived from
 *    the lower 8 bits of the Efuse MAC address
 * 2. The decoded text should contain the device'sEfuse MAC if authorized
 * 3. Verification is done by checking if the decoded content matches
 * 
 * @note This is a simple obfuscation method. For production use,
 *       consider implementing proper digital signatures.
 */
bool decodeServerResponse(String encodedResponse, char *decodedOutput, size_t bufferSize);

/**
 * @brief Verify decoded response matches device Efuse MAC
 * 
 * Checks if the decoded server response contains a valid authorization
 * token that matches the device's hardware identifier.
 * 
 * @param decodedText The decoded text from the server response
 * @param expectedEfuseMac The device'sEfuse MAC to verify against
 * @return true if authorization token matches the Efuse MAC
 * @return false if no valid token found or mismatch
 */
bool verifyAuthorization(const char *decodedText, uint64_t expectedEfuseMac);

/**
 * @brief Save authorization status to NVS non-volatile storage
 * 
 * Persists the device's authorization status in a dedicated NVS namespace
 * that survives factory reset and firmware reprogramming.
 * 
 * @param authorized true if device is authorized, false if denied
 * @return true if save operation succeeded
 * @return false if NVS storage failed
 * 
 * @details
 * Storage Location:
 * - NVS partition namespace: "license" (dedicated, separate from user settings)
 * - Key: "authorized" (boolean value)
 * - This namespace is NOT cleared by nvsErase() function
 * 
 * Persistence Guarantee:
 * - Authorization survives: firmware updates, OTA, factory reset
 * - Authorization survives: partition table changes, preferences erase
 * - Only manual NVS license namespace erase will clear it
 * 
 * @example
 * saveLicenseToNVS(true);
 * delay(LICENSE_SAVE_DELAY); // Allow NVS write to complete
 */
bool saveLicenseToNVS(bool authorized);

/**
 * @brief Load authorization status from NVS non-volatile storage
 * 
 * Retrieves the previously saved authorization status from the dedicated
 * license namespace in NVS.
 * 
 * @param isAuthorized Output parameter for the authorization status
 * @return true if value was successfully read from NVS
 * @return false if no value found or NVS read failed
 * 
 * @details
 * This function checks if the device has been previously authorized.
 * If no authorization status exists, it returns false without setting
 * the output parameter (indicating the device needs licensing).
 * 
 * @example
 * bool authorized = false;
 * if (loadLicenseFromNVS(authorized)) {
 *     if (authorized) {
 *         // Device is licensed, continue normal operation
 *     } else {
 *         // Device was previously denied
 *     }
 * } else {
 *     // No license in NVS, need to acquire one
 * }
 */
bool loadLicenseFromNVS(bool &isAuthorized);

/**
 * @brief Check if device is authorized (convenience function)
 * 
 * Simple boolean check of the device's authorization status.
 * First checks NVS storage, returns false if no status found.
 * 
 * @return true if device is authorized
 * @return false if device is not authorized or no license found
 * 
 * @note This is the primary function to call during startup
 *       to determine if the device should continue operation.
 */
bool isDeviceLicensed();

/**
 * @brief Acquire license from server and save to NVS
 * 
 * Complete license acquisition workflow:
 * 1. Get device's Efuse MAC
 * 2. Connect to license server via HTTPS
 * 3. Send Efuse MAC for validation
 * 4. Decode server response
 * 5. Verify authorization
 * 6. Save result to NVS
 * 
 * @return LicenseResult Result of the license acquisition
 * 
 * @details
 * This function handles the entire licensing process with retry logic.
 * It should be called after WiFi connection is established.
 * 
 * Usage:
 * - Call after successful WiFi connection in Station mode
 * - Display progress on screen during acquisition
 * - Respect user abort (button press) during the process
 * 
 * @example
 * // After WiFi connects successfully
 * drawScreen("Acquiring License...", "Contacting server...");
 * LicenseResult result = acquireLicense();
 * if (result == LICENSE_SUCCESS) {
 *     drawScreen("License Acquired", "Device is authorized");
 * } else {
 *     drawScreen("License Failed", getLicenseResultString(result));
 * }
 */
LicenseResult acquireLicense();

/**
 * @brief Convert LicenseResult to human-readable string
 * 
 * @param result The license result enum value
 * @return String descriptive text for the result
 */
const char* getLicenseResultString(LicenseResult result);

/**
 * @brief Initialize license check at startup
 * 
 * Sets up the initial license verification flow. Checks NVS for existing
 * authorization. If no license found, prepares the device to acquire one
 * after WiFi connection.
 * 
 * @param showOnScreen Whether to display license status on screen
 * @return true if device is licensed and can continue
 * @return false if device needs license (startup should redirect to WiFi)
 * 
 * @details
 * Startup Flow Integration:
 * 1. Before main initialization, call this function
 * 2. If returns true, device has valid license, continue normal boot
 * 3. If returns false, device needs licensing:
 *    - Force AP mode for WiFi configuration
 *    - Show license acquisition screen
 *    - After WiFi connects, call acquireLicense()
 * 
 * @note The license is stored separately from user preferences,
 *       so holding the button to reset preferences will NOT clear
 *       the license. Only reprogramming the ESP from scratch or
 *       replacing the eFuse MAC will affect licensing.
 */
bool initLicenseCheck(bool showOnScreen = true);

/**
 * @brief Force license acquisition mode
 * 
 * Sets the WiFi mode to AP with connect capability, forcing the device
 * to enter license acquisition mode.
 * 
 * @note Modifies wifiModeIdx defined in Common.h
 */
void forceLicenseMode();

/**
 * @brief Check if button is held at startup for factory reset
 * 
 * Monitors the encoder push button at startup to detect a factory reset
 * request. When held for 3+ seconds, factory reset is triggered
 * BUT the license data in the dedicated NVS namespace is preserved.
 * 
 * @param timeoutMs Maximum time to wait for button release (milliseconds)
 * @return true if button was held (factory reset requested)
 * @return false if button was not held
 */
bool isFactoryResetRequested(uint32_t timeoutMs = 3000);

/**
 * @brief Called after WiFi connects to acquire license if needed
 * 
 * This function is called from Network.cpp after WiFi connection is
 * established. If the device doesn't have a license, it automatically
 * acquires one from the server.
 * 
 * @return true if device is licensed (already was or newly acquired)
 * @return false if license acquisition failed or was denied
 * 
 * @note This function is non-blocking and should be called from netTickTime()
 */
bool checkAndAcquireLicense();

#endif // LICENSE_AUTH_H
