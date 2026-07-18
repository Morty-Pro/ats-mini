# License Authentication System - Documentation

## Overview

This document describes the hardware-based license authentication system implemented for the Signal Mini ESP32-based radio device. The system prevents firmware cloning and unauthorized copying by binding device authorization to the ESP32's unique hardware identifier (Efuse MAC address).

---

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                    DEVICE (ESP32)                            │
│  ┌───────────────────────────────────────────────────────┐  │
│  │              LicenseAuth Module                        │  │
│  │  ┌─────────────┐  ┌──────────────┐  ┌─────────────┐  │  │
│  │  │  Efuse MAC  │→│  HTTPS       │→│  NVS        │  │  │
│  │  │  Reader     │  │  Client      │  │  Storage    │  │  │
│  │  └─────────────┘  └──────────────┘  └─────────────┘  │  │
│  │       ↓                  ↓                  ↓         │  │
│  │  ┌─────────────────────────────────────────────────┐ │  │
│  │  │         License Validation Flow                 │ │  │
│  │  │  1. Read hardware ID    4. Decode response      │ │  │
│  │  │  2. Send to server      5. Verify authorization │ │  │
│  │  │  3. Get response        6. Save to NVS          │ │  │
│  │  └─────────────────────────────────────────────────┘ │  │
│  └───────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────┘
                              ↓
                    ┌─────────────────────┐
                    │  License Server     │
                    │  (HTTPS Endpoint)   │
                    │  khrh.ir/signal-    │
                    │  mini/ota/          │
                    │  license_verify     │
                    └─────────────────────┘
```

---

## How It Works

### 1. Device Startup Flow

```
┌─────────────┐
│ Power On    │
└──────┬──────┘
       │
       ▼
┌─────────────────┐
│ Check NVS for   │
│ License Status  │
└──────┬──────────┘
       │
    ┌──┴──┐
    │     │
    ▼     ▼
┌──────┐  ┌──────────────┐
│FOUND │  │ NOT FOUND    │
└──┬───┘  └──────┬───────┘
   │             │
   ▼             ▼
┌──────┐  ┌──────────────┐
│Continue│ │Force AP Mode │
│Normal │ │Show "LICENSE │
│Ops   │ │REQUIRED"      │
└──────┘  └──────┬───────┘
                 │
                 ▼
         ┌──────────────┐
         │User configures│
         │WiFi in AP     │
         └──────┬───────┘
                │
                ▼
         ┌──────────────┐
         │WiFi Connected │
         └──────┬───────┘
                │
                ▼
         ┌──────────────┐
         │Automatic     │
         │License       │
         │Acquisition   │
         └──────┬───────┘
                │
                ▼
         ┌──────────────┐
         │Server        │
         │Validates     │
         └──────┬───────┘
                │
            ┌───┴───┐
            │       │
            ▼       ▼
       ┌──────┐  ┌──────┐
       │Granted│ │Denied│
       └──┬───┘  └──┬───┘
          │         │
          ▼         ▼
      ┌──────┐  ┌──────┐
      │Continue│ │Halt  │
      │Normal  │ │Device│
      │Ops    │ │Useless│
      └──────┘  └──────┘
```

### 2. License Acquisition Flow

```
┌──────────────────────────────────────────────────────────┐
│              License Acquisition Process                   │
│                                                          │
│  Step 1: GetEfuseMac()                                    │
│           ↓ Reads unique 48-bit hardware MAC              │
│  Step 2: efuseMacToString()                               │
│           ↓ Converts to hex string (e.g., "545E209E139C") │
│  Step 3: requestLicenseFromServer()                       │
│           ↓ HTTPS POST to server with MAC                 │
│  Step 4: Server validates MAC                             │
│           ↓ Returns encoded response                      │
│  Step 5: decodeServerResponse()                           │
│           ↓ XOR decode with derived key                   │
│  Step 6: verifyAuthorization()                            │
│           ↓ Check if response matches MAC                 │
│  Step 7: saveLicenseToNVS()                               │
│           ↓ Store boolean in NVS namespace "license"      │
└──────────────────────────────────────────────────────────┘
```

---

## File Structure

```
ats-mini/ats-mini/
├── LicenseAuth.h       # Header file with API declarations
├── LicenseAuth.cpp     # Implementation of license system
├── Common.h            # Updated with license function declarations
├── Network.cpp         # Updated with license integration
├── ats-mini.ino        # Updated main file with license check
└── LICENSE_SYSTEM_README.md  # This file
```

---

## Key Files

### LicenseAuth.h

Contains:
- Type definitions and constants
- Function declarations
- Server URL and Root CA certificate
- NVS namespace configuration

**Key Constants:**
```cpp
#define LICENSE_NVS_NAMESPACE "license"    // NVS namespace
#define LICENSE_KEY_AUTHORIZED "authorized" // Storage key
#define LICENSE_SERVER_URL "https://..."   // Server endpoint
```

### LicenseAuth.cpp

Contains implementations of:
- `getEfuseMac()` - Read hardware MAC
- `requestLicenseFromServer()` - HTTPS communication
- `decodeServerResponse()` - XOR decoding
- `verifyAuthorization()` - Token verification
- `saveLicenseToNVS()` / `loadLicenseFromNVS()` - Persistent storage
- `acquireLicense()` - Complete acquisition workflow
- `initLicenseCheck()` - Startup initialization
- `checkAndAcquireLicense()` - Background acquisition

### Network.cpp

Integration points:
- `netTickTime()` - Called every loop iteration
- Automatically triggers license acquisition when WiFi connects
- Non-blocking operation with 10-second check interval

---

## NVS Storage Structure

```
ESP32 NVS Partition
┌─────────────────────────────────────┐
│ Namespace: "settings"               │
│   - User preferences (volume, etc.) │ ← Cleared by factory reset
│   - WiFi credentials                │
│   - Band settings                   │
├─────────────────────────────────────┤
│ Namespace: "license" (DEDICATED)    │
│   - Key: "authorized" (boolean)     │ ← NOT cleared by reset
│   - Key: "efusehash" (uint32)       │    Persists across updates
└─────────────────────────────────────┘
```

**Important:** The license namespace is separate from user settings, so factory reset (holding button on startup) does NOT clear the license.

---

## Server Protocol

### Request

```
POST https://khrh.ir/signal-mini-update/ota/license_verify?mac=<HEX_MAC_ADDRESS>
```

Where `<HEX_MAC_ADDRESS>` is the device's Efuse MAC in hex format (e.g., `545E209E139C`).

### Response

The server returns plain text that can be decoded:

**Authorization Success:**
- Returns: `"true"`, `"1"`, `"authorized"`, `"yes"`, or `"licensed"`

**Authorization Denied:**
- Returns: `"false"`, `"0"`, `"denied"`, `"no"`

**Network Error:**
- HTTP 404, 500, or connection timeout

### Security

- Uses HTTPS with certificate pinning
- Root CA certificate embedded in firmware
- Server identity verified before connection

---

## Usage Examples

### Check License at Startup

```cpp
#include "LicenseAuth.h"

void setup() {
    Serial.begin(115200);
    
    // Check if device has valid license
    if (!initLicenseCheck(true)) {
        // No license - force WiFi setup mode
        wifiModeIdx = NET_AP_CONNECT;
        
        // Try to acquire license
        LicenseResult result = acquireLicense();
        
        if (result != LICENSE_SUCCESS) {
            // Device cannot operate
            while(1);
        }
    }
    
    // Continue normal initialization...
}
```

### Query License Status

```cpp
// Quick boolean check
if (isDeviceLicensed()) {
    // Device is authorized
} else {
    // Device needs license
}
```

### Background License Acquisition

```cpp
// After WiFi connects, this is called automatically
// from Network.cpp's netTickTime()
bool licensed = checkAndAcquireLicense();
```

---

## Error Codes

| Code | Value | Description |
|------|-------|-------------|
| `LICENSE_SUCCESS` | 0 | Device is authorized |
| `LICENSE_DENIED` | 1 | EfuseMac not recognized by server |
| `LICENSE_NETWORK_ERROR` | 2 | Could not connect to server |
| `LICENSE_INVALID_RESPONSE` | 3 | Server returned unexpected format |
| `LICENSE_STORAGE_ERROR` | 4 | NVS write failed |

---

## Factory Reset Behavior

When the user holds the encoder button at startup:

1. **User preferences are erased** (volume, WiFi settings, bands, memories)
2. **License data is PRESERVED** in dedicated NVS namespace
3. Device continues normal operation if license is valid

This ensures that:
- Users can reset settings without losing license
- Reprogramming the firmware doesn't invalidate the license
- Only replacing the ESP32 chip or server-side revocation affects licensing

---

## Deployment Checklist

### Server-Side Setup

- [ ] Create HTTPS endpoint: `/signal-mini/ota/license_verify`
- [ ] Implement MAC validation logic
- [ ] Return appropriate text responses
- [ ] Test with sample MAC addresses

### Device-Side Setup

- [ ] Update `LICENSE_SERVER_URL` if different server URL
- [ ] Update `LICENSE_ROOT_CA` if using different CA certificate
- [ ] Configure WiFi credentials for first-time setup
- [ ] Test license acquisition flow

### Testing

- [ ] Test on fresh device (no license in NVS)
- [ ] Test with valid MAC (should get license)
- [ ] Test with invalid MAC (should be denied)
- [ ] Test factory reset preserves license
- [ ] Test firmware update preserves license
- [ ] Test network failure handling

---

## Troubleshooting

### "License Required" Screen Stuck

**Cause:** WiFi not configured or server unreachable

**Solution:**
1. Connect to device AP
2. Access web interface at `http://10.1.1.1`
3. Configure WiFi credentials
4. Device will automatically retry license acquisition

### "Network Error"

**Cause:** Cannot reach license server

**Solution:**
1. Verify WiFi connection
2. Check server URL and accessibility
3. Verify SSL certificate is valid
4. Check serial logs for details

### "Efuse Hash Mismatch"

**Cause:** License data corrupted or transferred between devices

**Solution:**
1. Erase NVS completely: `esptool.py erase_flash`
2. Reprogram firmware
3. Re-acquire license from server

---

## Security Considerations

1. **Hardware Binding:** License bound to unique Efuse MAC
2. **Transport Security:** HTTPS with certificate pinning
3. **Persistent Storage:** License survives firmware updates
4. **Server Validation:** Server maintains authoritative list
5. **Response Encoding:** XOR obfuscation prevents simple tampering

---

## Future Enhancements

- [ ] Add digital signatures to license responses
- [ ] Implement license expiration/renewal
- [ ] Add batch MAC registration interface
- [ ] Support offline license activation
- [ ] Add license transfer mechanism

---

## Support

For issues or questions:
- Check serial logs (115200 baud)
- Review this documentation
- Contact support at KHRH

---

*Developed for Signal Mini by KHRH*