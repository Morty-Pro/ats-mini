# Signal Mini License System - Deployment Checklist

## Overview

This checklist ensures proper deployment of the hardware-based license authentication system for Signal Mini ESP32 radio device.

---

## Files Modified/Created

### New Files
- [x] `ats-mini/ats-mini/LicenseAuth.h` - License authentication API header
- [x] `ats-mini/ats-mini/LicenseAuth.cpp` - License authentication implementation
- [x] `ats-mini/LICENSE_SYSTEM_README.md` - System documentation
- [x] `ats-mini/LICENSE_SECURITY.md` - Security architecture details
- [x] `ats-mini/license_verify_server_example.php` - Server-side verification script

### Modified Files
- [x] `ats-mini/ats-mini/ats-mini.ino` - Added license check in setup()
- [x] `ats-mini/ats-mini/Network.cpp` - Added license acquisition after WiFi connect
- [x] `ats-mini/ats-mini/Common.h` - Added external license function declarations

---

## Server-Side Setup

### 1. Configure PHP Script

```bash
# Edit license_verify_server_example.php
# Change LICENSE_SHARED_SECRET to a random 32-byte key
openssl rand -hex 32

# Update the constant in the PHP file
# Line ~45: const LICENSE_SHARED_SECRET = 'your_random_key_here';
```

### 2. Upload to Web Server

```bash
# Upload to web server with HTTPS
scp license_verify_server_example.php user@your-server:/var/www/html/signal-mini/ota/

# Set proper permissions
ssh user@your-server "chmod 644 /var/www/html/signal-mini/ota/license_verify_server_example.php"
```

### 3. Add Authorized MAC Addresses

Edit the PHP file and add device MAC addresses:

```php
$AUTHORIZED_MACS = [
    "545E209E139C",  // Device 1 - get from serial monitor
    "58E7209E139C",  // Device 2
    // ... add more as needed
];
```

### 4. Test Server Endpoint

```bash
# Generate test HMAC signature
echo -n "545E209E139C:1720089600" | \
  openssl dgst -sha256 -hmac "signalMiniSecret2024_KeyChanG3" -hex

# Test the endpoint
curl "https://your-domain/signal-mini/ota/license_verify?mac=545E209E139C&ts=1720089600&sig=<signature>"

# Expected response: "authorized" or "denied"
```

---

## Device-Side Setup

### 1. Update Configuration (if needed)

In `LicenseAuth.h`:
- [ ] Update `LICENSE_SHARED_SECRET` to match server
- [ ] Update `LICENSE_SERVER_URL` if using different domain
- [ ] Update `LICENSE_ROOT_CA` if using different CA certificate

### 2. Compile and Flash

```bash
# Using PlatformIO
cd ats-mini/ats-mini
platformio run -e your_board_definition

# Upload to ESP32
platformio run -e your_board_definition --target upload
```

### 3. First Boot Testing

- [ ] Connect USB serial (115200 baud)
- [ ] Observe serial output:
  ```
  [LicenseAuth] Initializing license check...
  [LicenseAuth] No license found in NVS - device needs licensing
  [LicenseAuth] Device Efuse MAC: <12-char-hex>
  ```

### 4. WiFi Configuration

- [ ] Device should create AP with name "RECEIVER_NAME"
- [ ] Connect to AP from smartphone/laptop
- [ ] Access `http://10.1.1.1` or `http://atsmini.local`
- [ ] Configure WiFi credentials for your network
- [ ] Device reconnects to WiFi as Station

### 5. License Acquisition

- [ ] After WiFi connects, device automatically requests license
- [ ] Check serial output:
  ```
  [LicenseAuth] Starting automatic license acquisition...
  [LicenseAuth] Requesting license with HMAC signature...
  [LicenseAuth] Server response: authorized
  [LicenseAuth] License AUTHORIZED saved to NVS
  [LicenseAuth] License ACQUIRED successfully!
  ```

### 6. Verify Persistent Storage

- [ ] Reboot device (reset button)
- [ ] Check serial output:
  ```
  [LicenseAuth] Valid license found in NVS - device is LICENSED
  ```
- [ ] Device should continue normal operation without license screen

### 7. Factory Reset Test

- [ ] Hold encoder button at startup
- [ ] Verify user preferences are cleared
- [ ] Reboot device
- [ ] Verify license is PRESERVED (no license screen shown)

---

## Security Testing

### 1. HMAC Signature Verification

Test that stolen MAC cannot be reused:

```bash
# Capture a valid request
mac=545E209E139C&ts=1720089600&sig=abc123...

# Try to replay 10 minutes later (should fail)
# Server should reject due to timestamp tolerance
```

### 2. MAC Spoofing Test

Test that another device with same MAC is rejected:

```bash
# Thief tries to use captured MAC
# Without shared secret, cannot generate valid HMAC
# Server rejects request
```

---

## Production Deployment

### Security Hardening

- [ ] Change `LICENSE_SHARED_SECRET` to strong random key
- [ ] Implement database for MAC management (instead of PHP array)
- [ ] Set up rate limiting per IP
- [ ] Configure logging and monitoring
- [ ] Enable HTTPS with valid SSL certificate
- [ ] Implement certificate pinning on device

### License Management

- [ ] Create admin interface for adding/removing MACs
- [ ] Implement license expiration/renewal
- [ ] Add license transfer mechanism
- [ ] Set up backup and disaster recovery

### Firmware Distribution

- [ ] Test OTA update preserves license
- [ ] Document license recovery process
- [ ] Create support documentation for customers

---

## Troubleshooting

### License Not Acquired

**Problem**: Device shows "LICENSE REQUIRED" after WiFi connects

**Solutions**:
1. Check WiFi connection is working
2. Verify server URL is accessible
3. Check serial logs for errors
4. Verify MAC is in `$AUTHORIZED_MACS` on server
5. Test server endpoint manually with curl

### License Lost After Update

**Problem**: Device requires license again after firmware update

**Solution**: License should persist in NVS "license" namespace. If not:
1. Check NVS partition is not being erased
2. Verify `LICENSE_NVS_NAMESPACE` is consistent
3. Manually erase NVS and re-acquire: `esptool.py erase_flash`

### HMAC Verification Fails

**Problem**: Server rejects valid requests

**Solution**:
1. Verify `LICENSE_SHARED_SECRET` matches on device and server
2. Check timestamp tolerance is sufficient
3. Verify HMAC algorithm is identical (SHA256, not SHA512)

---

## Support

For issues or questions:
- Review `LICENSE_SYSTEM_README.md`
- Review `LICENSE_SECURITY.md`
- Check serial logs (115200 baud)
- Contact KHRH Support

---

*License System v1.0 - Developed for Signal Mini by KHRH*