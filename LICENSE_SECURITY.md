# License Authentication System - Security Architecture

## Overview

This document details the multi-layered security architecture implemented to prevent firmware cloning, code theft, and unauthorized copying on the Signal Mini ESP32-based radio device.

## Hardware Binding

### Efuse MAC Address

Each ESP32 chip has a **unique 48-bit MAC address** burned into eFuse hardware during manufacturing at Espressif's factory. This identifier:

- **Cannot be read or modified** by software
- **Is unique** to each chip (statistically guaranteed)
- **Survives** firmware updates, OTA flashing, and partition changes
- **Cannot be spoofed** or reset

This makes it the ideal hardware fingerprint for device binding.

## Multi-Layer Security Architecture

### Layer 1: Hardware Binding (Efuse MAC)

```
┌─────────────────────────────────────────────┐
│  ESP32 Chip (Unique to Each Device)         │
│  ┌─────────────────────────────────────┐    │
│  │  eFuse Hardware Storage             │    │
│  │  MAC: 54-5E-20-9E-13-9C             │    │
│  │  (Factory burned, immutable)        │    │
│  └─────────────────────────────────────┘    │
└─────────────────────────────────────────────┘
```

### Layer 2: HTTPS Encrypted Transport

```
┌──────────┐                    ┌──────────┐
│  Device  │                    │  Server  │
│          │    HTTPS (TLS)     │          │
│  ───────>│ ===================>│          │
│          │    Encrypted       │          │
│          │    Channel         │          │
└──────────┘                    └──────────┘
```

- MAC address encrypted in transit via TLS
- Server verifies its own certificate (pinned Root CA)
- Prevents man-in-the-middle attacks

### Layer 3: HMAC-SHA256 Request Signature

This is the **core security mechanism** that prevents MAC theft and replay attacks.

```
Device Side:                                  Server Side:
┌─────────────────────┐                       ┌─────────────────────┐
│ 1. Get Efuse MAC    │                       │ 1. Receive request  │
│ 2. Get timestamp    │                       │ 2. Extract params   │
│ 3. Create payload   │                       │    mac:ts           │
│    "MAC:timestamp"  │                       │                     │
│ 4. HMAC-SHA256(     │                       │ 3. Recompute HMAC   │
│    payload,          │                       │    with shared secret│
│    shared_secret)   │                       │                     │
│ 5. Send:            │                       │ 4. Compare signatures│
│    mac=xxx&ts=yyy   │                       │    (constant-time)  │
│    sig=zzz          │                       └─────────────────────┘
└─────────────────────┘
```

**HMAC Formula:**
```
sig = HMAC-SHA256("MAC_ADDRESS:TIMESTAMP", SHARED_SECRET)
```

**Why This Protects the MAC:**

1. **Cannot reuse captured MAC**: Each request has a unique timestamp. Old requests expire after 5 minutes.

2. **Cannot generate new signatures**: The shared secret is compiled into the device firmware and never transmitted. Without it, a thief cannot sign requests for a captured MAC.

3. **Cannot spoof another device**: Even knowing the algorithm, you need the actual device's Efuse MAC AND the shared secret.

4. **Cannot tamper with request**: HMAC is calculated over BOTH MAC AND timestamp. Changing either invalidates the signature.

### Layer 4: Persistent NVS Storage

```
┌─────────────────────────────────────┐
│  ESP32 NVS Partition                │
│  ┌───────────────────────────────┐  │
│  │  "settings" namespace         │  │ ← Cleared by factory reset
│  │  - User preferences           │  │
│  └───────────────────────────────┘  │
│  ┌───────────────────────────────┐  │
│  │  "license" namespace (DEDICATED)│ │
│  │  - authorized (boolean)       │  │ ← NOT cleared
│  │  - efusehash (uint64)         │  │    Persists forever
│  └───────────────────────────────┘  │
└─────────────────────────────────────┘
```

## Security Threats & Mitigations

### Threat 1: Firmware Extraction

**Attack**: Thief extracts firmware from Flash chip and tries to run on another ESP32.

**Mitigation**: 
- Device checks Efuse MAC on every boot
- Different chip = different MAC = license denied
- License stored in NVS "license" namespace (survives flashing)

### Threat 2: Network Interception

**Attack**: Thief captures HTTPS request containing MAC address.

**Mitigation**:
- HTTPS encrypts traffic (MAC not readable offline)
- HMAC signature requires shared secret to validate
- Timestamp expires after 5 minutes (replay attack prevention)

### Threat 3: Request Replay

**Attack**: Thief captures valid request and replays it to server.

**Mitigation**:
- Server checks timestamp against current time
- Requests outside ±5 minute window are rejected
- Different timestamp = different HMAC signature required

### Threat 4: MAC Spoofing

**Attack**: Thief tries to claim another device's MAC address.

**Mitigation**:
- HMAC signature uses "MAC:timestamp" payload
- Even if thief knows the MAC, they don't have the shared secret
- Cannot generate valid signature for stolen MAC

### Threat 5: Shared Secret Extraction

**Attack**: Thief reverse engineers firmware to find LICENSE_SHARED_SECRET.

**Mitigation**:
- If secret is extracted, attacker CAN generate signatures
- **Countermeasure**: Change LICENSE_SHARED_SECRET for production
- Use random 32-byte key: `openssl rand -hex 32`
- Consider obfuscating the key in firmware

### Threat 6: Factory Reset

**Attack**: User holds button to reset device, hoping to clear license.

**Mitigation**:
- License stored in separate NVS namespace "license"
- Factory reset only clears "settings" namespace
- License survives user-initiated factory reset

## Implementation Details

### Request Format

```
GET /signal-mini/ota/license_verify?mac=545E209E139C&ts=1720000000&sig=<64_hex_chars>
```

| Parameter | Description | Example |
|-----------|-------------|---------|
| `mac` | Device Efuse MAC (hex) | `545E209E139C` |
| `ts` | Unix timestamp (seconds) | `1720000000` |
| `sig` | HMAC-SHA256 signature (hex) | `a1b2c3...` (64 chars) |

### HMAC Signature Generation (Device Side)

```cpp
// In LicenseAuth.cpp
String payload = macStr + ":" + String(timestamp);
sha256.beginHMAC();
sha256.setHMACKey((const uint8_t*)LICENSE_SHARED_SECRET, 
                  strlen(LICENSE_SHARED_SECRET));
sha256.print(payload);
uint8_t* hmacResult = sha256.resultHmac();
String signature = hexEncode(hmacResult);
```

### HMAC Signature Verification (Server Side)

```php
// In license_verify_server_example.php
$payload = $mac . ":" . $timestamp;
$expectedSignature = hash_hmac('sha256', $payload, $LICENSE_SHARED_SECRET);
if (hash_equals($expectedSignature, $signature)) {
    // Signature valid, proceed with authorization
}
```

### Timestamp Tolerance

```
Server receives request at: 2024-07-04 12:00:00 UTC (1720089600)
Request contains timestamp: 1720089580 (20 seconds ago)
Time difference: |1720089600 - 1720089580| = 20 seconds
Tolerance window: 300 seconds (5 minutes)
Result: 20 < 300 → ACCEPTED
```

```
Attacker tries to replay at: 2024-07-04 13:00:00 UTC (1 hour later)
Request contains timestamp: 1720089580
Time difference: |1720125200 - 1720089580| = 35620 seconds
Tolerance window: 300 seconds (5 minutes)
Result: 35620 > 300 → REJECTED (timestamp expired)
```

## Production Deployment Checklist

### Server Configuration

- [ ] Change `LICENSE_SHARED_SECRET` to random 32-byte key
- [ ] Configure authorized MAC list (database)
- [ ] Set up HTTPS with valid SSL certificate
- [ ] Configure rate limiting (IP-based)
- [ ] Set up logging and monitoring
- [ ] Test HMAC verification with sample requests

### Device Configuration

- [ ] Update `LICENSE_SHARED_SECRET` to match server
- [ ] Update `LICENSE_SERVER_URL` if needed
- [ ] Update `LICENSE_ROOT_CA` for your CA
- [ ] Add authorized MACs to server database

### Security Hardening

- [ ] Use unique shared secret per production batch
- [ ] Implement license expiration/renewal
- [ ] Add device fingerprinting beyond MAC
- [ ] Implement certificate pinning (not just CA verification)
- [ ] Add secure boot to prevent firmware modification

## Testing HMAC Security

### Test 1: Valid Request

```bash
# Generate valid signature (Linux/Mac)
echo -n "545E209E139C:1720089600" | \
  openssl dgst -sha256 -hmac "signalMiniSecret2024_KeyChanG3" -hex

# Expected: 64-character hex signature

# Test on server
curl "https://your-server/signal-mini/ota/license_verify?mac=545E209E139C&ts=1720089600&sig=<signature>"
# Expected response: "authorized" or "denied"
```

### Test 2: Tampered MAC

```bash
# Original request
mac=545E209E139C&ts=1720089600&sig=abc...

# Attacker changes MAC but keeps signature
mac=AABBCCDDEEFF&ts=1720089600&sig=abc...

# Server result: REJECTED (HMAC mismatch)
```

### Test 3: Replay Attack

```bash
# Capture valid request from 1 hour ago
mac=545E209E139C&ts=1720086000&sig=xyz...

# Replay after 1 hour
# Server result: REJECTED (timestamp expired, diff > 300s)
```

### Test 4: Spoofed Request

```bash
# Thief doesn't have shared secret
# Cannot generate valid HMAC for any MAC
# Server result: REJECTED (HMAC verification fails)
```

## Future Security Enhancements

- [ ] **Certificate Pinning**: Verify server certificate fingerprint, not just CA
- [ ] **Challenge-Response**: Server sends random nonce, device signs with secret
- [ ] **Device Fingerprinting**: Combine MAC with other hardware identifiers
- [ ] **Encrypted License Token**: Server returns signed JWT instead of plain text
- [ ] **Rate Limiting by MAC**: Limit attempts per device
- [ ] **License Revocation**: Server can revoke compromised licenses
- [ ] **Over-The-Air License Renewal**: Secure renewal without factory visit

---

*Security Architecture by KHRH for Signal Mini*