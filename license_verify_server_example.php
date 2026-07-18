<?php
/**
 * License Verification Endpoint - Signal Mini Device
 * 
 * This script handles license verification requests from Signal Mini devices.
 * It validates the device's Efuse MAC address using HMAC-SHA256 signature
 * verification and returns an authorization response.
 * 
 * SECURITY:
 * - Each request includes HMAC-SHA256 signature for authentication
 * - Timestamp prevents replay attacks (5-minute window)
 * - Even if MAC is intercepted, it cannot be reused
 * 
 * Usage:
 *   GET /signal-mini/ota/license_verify?mac=<MAC>&ts=<TIMESTAMP>&sig=<SIGNATURE>
 * 
 * Response:
 *   "authorized" - Device is licensed
 *   "denied"     - Device is not licensed
 * 
 * Request Parameters:
 *   - mac: Device's Efuse MAC address (hex string, e.g., "545E209E139C")
 *   - ts:  Unix timestamp in seconds (for replay protection)
 *   - sig: HMAC-SHA256 signature (hex string, 64 characters)
 */

// ============================================================
// CONFIGURATION - MUST MATCH DEVICE FIRMWARE
// ============================================================

/**
 * @brief Shared secret key for HMAC signature verification
 * 
 * This MUST match the LICENSE_SHARED_SECRET in LicenseAuth.h
 * Change both places if you want to use a different key.
 * 
 * PRODUCTION: Use a random 32-byte key generated with:
 *   openssl rand -hex 32
 */
const LICENSE_SHARED_SECRET = 'signalMiniSecret2024_KeyChanG3';

/**
 * @brief Timestamp tolerance in seconds (5 minutes)
 * 
 * Requests older than this are rejected to prevent replay attacks.
 * A thief who captures traffic cannot reuse it after this period.
 */
const TIMESTAMP_TOLERANCE = 300; // 5 minutes

/**
 * @brief Authorized MAC addresses database
 * 
 * Add authorized device MAC addresses here.
 * In production, store this in a database.
 * MAC addresses should be in uppercase hex format (12 characters).
 */
$AUTHORIZED_MACS = [
    // Example authorized devices - replace with your actual devices
    "545E209E139C",  // Morty (example)
    "58E7209E139C",  // Device 1 (example)
    "505E209E139C",  // Device 2 (example)
    
    // Add more MAC addresses as needed
    // Format: "AABBCCDDEEFF" (no colons, uppercase hex)
];

/**
 * @brief Rate limiting configuration
 * 
 * Prevent abuse by limiting requests per IP address.
 */
define('RATE_LIMIT_MAX_REQUESTS', 10);      // Max requests per period
define('RATE_LIMIT_PERIOD', 3600);          // Period in seconds (1 hour)

/**
 * @brief Logging configuration
 */
//define('LOG_FILE', '/var/log/signal-mini/license_verify.log'); // commented by morty because server can not make directory(have no permission)
define('LOG_FILE', 'license_verify.log');
define('ENABLE_LOGGING', true);

// ============================================================
// HELPER FUNCTIONS
// ============================================================

/**
 * @brief Log message to log file
 */
function logMessage($message) {
    if (!ENABLE_LOGGING) {
        return;
    }
    
    $timestamp = date('Y-m-d H:i:s');
    $logEntry = "[$timestamp] $message" . PHP_EOL;
    
    // Ensure log directory exists
    $logDir = dirname(LOG_FILE);
    if (!is_dir($logDir)) {
        mkdir($logDir, 0755, true);
    }
    
    file_put_contents(LOG_FILE, $logEntry, FILE_APPEND);
}

/**
 * @brief Check rate limiting for an IP address
 * 
 * @param string $ip Client IP address
 * @return bool true if rate limited, false if allowed
 */
function checkRateLimit($ip) {
    $sessionKey = "rate_limit_" . md5($ip);
    
    // Use file-based rate limiting (replace with Redis for high traffic)
    $rateFile = sys_get_temp_dir() . "/" . $sessionKey;
    
    if (file_exists($rateFile)) {
        $rateData = json_decode(file_get_contents($rateFile), true);
        
        // Check if period has expired
        if (time() - $rateData['time'] > RATE_LIMIT_PERIOD) {
            // Reset counter
            $rateData = ['count' => 0, 'time' => time()];
        }
        
        // Increment counter
        $rateData['count']++;
        
        // Save back
        file_put_contents($rateFile, json_encode($rateData));
        
        // Check limit
        if ($rateData['count'] > RATE_LIMIT_MAX_REQUESTS) {
            return true; // Rate limited
        }
    } else {
        // First request
        $rateData = ['count' => 1, 'time' => time()];
        file_put_contents($rateFile, json_encode($rateData));
    }
    
    return false; // Allowed
}

/**
 * @brief Get client IP address
 * 
 * @return string Client IP address
 */
function getClientIP() {
    $ip = $_SERVER['REMOTE_ADDR'];
    
    // Handle proxy
    if (!empty($_SERVER['HTTP_X_FORWARDED_FOR'])) {
        $ips = explode(',', $_SERVER['HTTP_X_FORWARDED_FOR']);
        $ip = trim($ips[0]);
    }
    
    return $ip;
}

/**
 * @brief Send plain text response
 * 
 * @param string $text Response text
 * @param int $statusCode HTTP status code
 */
function sendTextResponse($text, $statusCode = 200) {
    header('Content-Type: text/plain; charset=utf-8');
    http_response_code($statusCode);
    echo $text;
    exit;
}

/**
 * @brief Send JSON error response
 * 
 * @param string $message Error message
 * @param int $statusCode HTTP status code
 */
function sendErrorResponse($message, $statusCode = 400) {
    header('Content-Type: application/json; charset=utf-8');
    http_response_code($statusCode);
    echo json_encode(['error' => $message]);
    exit;
}

/**
 * @brief Normalize MAC address format
 * 
 * Ensures MAC is in correct format (12 uppercase hex chars)
 * 
 * @param string $mac Raw MAC address
 * @return string Normalized MAC address
 */
function normalizeMAC($mac) {
    // Remove any separators (colons, dashes, spaces)
    $mac = preg_replace('/[:\s\-]/', '', $mac);
    
    // Convert to uppercase
    $mac = strtoupper(trim($mac));
    
    // Validate format
    if (!preg_match('/^[0-9A-F]{12}$/', $mac)) {
        return ''; // Invalid format
    }
    
    return $mac;
}

/**
 * @brief Verify HMAC-SHA256 signature
 * 
 * This is the CORE security function. It verifies that:
 * 1. The request came from a device with the shared secret
 * 2. The request was not tampered with
 * 3. The timestamp is within acceptable window
 * 
 * @param string $mac The MAC address from request
 * @param int $timestamp The timestamp from request
 * @param string $signature The signature from request
 * @return bool true if signature is valid, false otherwise
 */
function verifyHMACSignature($mac, $timestamp, $signature) {
    global $LICENSE_SHARED_SECRET;
    
    // Check timestamp is numeric and reasonable
    if (!is_numeric($timestamp)) {
        logMessage("ERROR: Invalid timestamp format");
        return false;
    }
    
    $timestamp = intval($timestamp);
    
    // Check timestamp is not too far in the past or future
    $currentTime = time();
    $timeDiff = abs($currentTime - $timestamp);
    
    if ($timeDiff > TIMESTAMP_TOLERANCE) {
        logMessage("ERROR: Timestamp out of tolerance window. Diff: {$timeDiff}s");
        return false;
    }
    
    // Recreate the signature payload (MUST match device code exactly)
    // Format: "mac:timestamp"
    $payload = $mac . ":" . $timestamp;
    
    // Calculate expected HMAC-SHA256 signature
    $expectedSignature = hash_hmac('sha256', $payload, $LICENSE_SHARED_SECRET);
    
    // Constant-time comparison to prevent timing attacks
    if (hash_equals($expectedSignature, $signature)) {
        logMessage("HMAC signature verified successfully");
        return true;
    } else {
        logMessage("ERROR: HMAC signature verification failed");
        logMessage("  Expected: $expectedSignature");
        logMessage("  Got:      $signature");
        return false;
    }
}

/**
 * @brief Check MAC against authorized list
 * 
 * @param string $mac Normalized MAC address
 * @return bool true if authorized, false otherwise
 */
function isMACAuthorized($mac) {
    global $AUTHORIZED_MACS;
    
    // Check in-memory list
    if (in_array($mac, $AUTHORIZED_MACS)) {
        logMessage("MAC found in authorized list: " . $mac);
        return true;
    }
    
    // Check database (if configured)
    // This is a placeholder - implement based on your database setup
    /*
    try {
        $pdo = new PDO(
            "mysql:host=$DB_HOST;dbname=$DB_NAME;charset=utf8",
            $DB_USER,
            $DB_PASS
        );
        $pdo->setAttribute(PDO::ATTR_ERRMODE, PDO::ERRMODE_EXCEPTION);
        
        $stmt = $pdo->prepare(
            "SELECT COUNT(*) FROM licenses 
             WHERE mac_address = :mac 
             AND is_active = 1 
             AND expires_at > NOW()"
        );
        $stmt->execute([':mac' => $mac]);
        $count = $stmt->fetchColumn();
        
        return $count > 0;
    } catch (PDOException $e) {
        logMessage("Database error: " . $e->getMessage());
        return false;
    }
    */
    
    return false;
}

// ============================================================
// MAIN PROCESSING
// ============================================================

// Set CORS headers (if needed for web interface)
header('Access-Control-Allow-Origin: *');
header('Access-Control-Allow-Methods: GET, POST, OPTIONS');
header('Access-Control-Allow-Headers: Content-Type');

// Handle preflight request
if ($_SERVER['REQUEST_METHOD'] === 'OPTIONS') {
    http_response_code(200);
    exit;
}

// Get client IP and log request
$clientIP = getClientIP();
logMessage("=== Request from IP: " . $clientIP . " ===");

// Check rate limiting
if (checkRateLimit($clientIP)) {
    logMessage("Rate limit exceeded for IP: " . $clientIP);
    sendTextResponse("Rate limited. Please try again later.", 429);
}

// ============================================================
// STEP 1: EXTRACT AND VALIDATE REQUEST PARAMETERS
// ============================================================

// Get parameters from query string or POST body
$mac = '';
$timestamp = 0;
$signature = '';

if (isset($_GET['mac'])) {
    $mac = $_GET['mac'];
    $timestamp = intval($_GET['ts'] ?? 0);
    $signature = $_GET['sig'] ?? '';
} elseif (isset($_POST['mac'])) {
    $mac = $_POST['mac'];
    $timestamp = intval($_POST['ts'] ?? 0);
    $signature = $_POST['sig'] ?? '';
}

// Validate and normalize MAC
$mac = normalizeMAC($mac);

if (empty($mac)) {
    logMessage("ERROR: Invalid or missing MAC address");
    sendErrorResponse("Invalid MAC address format", 400);
}

if (empty($signature)) {
    logMessage("ERROR: Missing HMAC signature");
    sendErrorResponse("Missing authentication signature", 400);
}

if ($timestamp == 0) {
    logMessage("ERROR: Missing timestamp");
    sendErrorResponse("Missing request timestamp", 400);
}

logMessage("Received request - MAC: $mac, Timestamp: $timestamp");

// ============================================================
// STEP 2: VERIFY HMAC SIGNATURE
// ============================================================
// This is the CRITICAL security check. Even if a thief intercepts
// the HTTPS request, they cannot:
// - Modify the MAC (signature won't match)
// - Replay the request (timestamp expires)
// - Spoof another device (signature uses shared secret)

if (!verifyHMACSignature($mac, $timestamp, $signature)) {
    logMessage("SECURITY: Request rejected - signature verification failed");
    sendErrorResponse("Authentication failed - invalid signature", 403);
}

// ============================================================
// STEP 3: CHECK IF MAC IS AUTHORIZED
// ============================================================
// At this point, the signature is valid, meaning the request
// came from a legitimate device. Now check if this MAC
// is authorized to use the service.

if (!isMACAuthorized($mac)) {
    logMessage("MAC not in authorized list: " . $mac);
    sendTextResponse("denied");
}

// ============================================================
// STEP 4: RETURN AUTHORIZATION
// ============================================================
logMessage("MAC authorized: " . $mac);
sendTextResponse("authorized");

?>

<!--
 * ============================================================
 * SERVER DEPLOYMENT INSTRUCTIONS
 * ============================================================
 * 
 * 1. Upload this file to your web server:
 *    /var/www/html/signal-mini/ota/license_verify.php
 * 
 * 2. Ensure the web server can write to log directory:
 *    mkdir -p /var/log/signal-mini
 *    chmod 755 /var/log/signal-mini
 * 
 * 3. IMPORTANT: Set LICENSE_SHARED_SECRET to match the value
 *    in LicenseAuth.h on the device side
 * 
 * 4. Add authorized MAC addresses to $AUTHORIZED_MACS array
 * 
 * 5. For production, configure database connection
 * 
 * 6. Test with curl:
 *    curl "https://your-domain/signal-mini/ota/license_verify?mac=545E209E139C&ts=1234567890&sig=<computed_hmac>"
 * 
 * 7. To compute test signature:
 *    echo -n "545E209E139C:1234567890" | hmac-sha256 -h LICENSE_SHARED_SECRET
 * 
 * 8. For HTTPS, configure SSL certificate in web server
 * ============================================================
 * 
 * SECURITY NOTES:
 * 
 * 1. The MAC address IS visible in HTTPS requests, but:
 *    - It's encrypted in transit (HTTPS)
 *    - The server verifies HMAC signature before using it
 *    - A thief who captures it CANNOT reuse it (HMAC uses timestamp)
 *    - The server checks signature matches the shared secret
 * 
 * 2. Even if a thief knows:
 *    - The MAC address (intercepted from HTTPS)
 *    - The timestamp (sent in request)
 *    - The signature (sent in request)
 *    
 *    They CANNOT:
 *    - Generate new valid signatures (don't have shared secret)
 *    - Spoof another device (signature won't match their MAC)
 *    - Replay old requests (timestamp expires in 5 minutes)
 * 
 * 3. The shared secret is the key security component:
 *    - It's compiled into the device firmware
 *    - It's stored on the server
 *    - It's never transmitted over the network
 *    - Without it, thief cannot generate valid signatures
 * ============================================================
-->