#include "certs.h"
#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Update.h>
#include <mbedtls/md.h>

const char* ssid = "Ethernet15";
const char* password = "Fa@67890";
const char* mqtt_server = "a2v0ya4tb7ssid-ats.iot.eu-north-1.amazonaws.com";
const char* thing_name = "ESP32_OTA_TEST";

enum OTAState {
  IDLE,
  JOB_RECEIVED,
  DOWNLOADING,
  REPORTING_SUCCESS,
  REPORTING_FAILURE
};

struct OTA {
  OTAState state = IDLE;
  String jobId = "";
  String downloadUrl = "";
  String checksum = "";
  String targetVersion = "";
  String lastError = "";
  unsigned long lastJobCheck = 0;
};

OTA ota;

// Separate secure client for MQTT vs HTTPS download.
// Sharing one WiFiClientSecure between PubSubClient and HTTPClient means
// starting the firmware download tears down the MQTT socket underneath you.
WiFiClientSecure mqttSecureClient;
WiFiClientSecure otaSecureClient;
PubSubClient client(mqttSecureClient);

unsigned long lastMqttReconnectAttempt = 0;

// ---------- Topic helpers ----------
// NOTE: AWS IoT Jobs does NOT have a "$next/describe" / "describe/accepted"
// topic pair. The only real topics for fetching the next pending job are
// $next/get and $next/get/accepted. The old describe-based path is removed.

String getJobNotifyNextTopic() {
  return "$aws/things/" + String(thing_name) + "/jobs/notify-next";
}

String getJobNextGetTopic() {
  return "$aws/things/" + String(thing_name) + "/jobs/$next/get";
}

String getJobNextGetAcceptedTopic() {
  return "$aws/things/" + String(thing_name) + "/jobs/$next/get/accepted";
}

String getJobUpdateTopic(const String& jobId) {
  return "$aws/things/" + String(thing_name) + "/jobs/" + jobId + "/update";
}

// Wildcard subscriptions so we get confirmation regardless of which jobId is active.
String getJobUpdateAcceptedWildcardTopic() {
  return "$aws/things/" + String(thing_name) + "/jobs/+/update/accepted";
}

String getJobUpdateRejectedWildcardTopic() {
  return "$aws/things/" + String(thing_name) + "/jobs/+/update/rejected";
}



String CalculateSHA256(uint8_t* data, size_t len) {
  unsigned char hash[32];
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 0);
  mbedtls_md_starts(&ctx);
  mbedtls_md_update(&ctx, data, len);
  mbedtls_md_finish(&ctx, hash);
  mbedtls_md_free(&ctx);

  char hexString[65];
  for (int i = 0; i < 32; i++) {
    sprintf(hexString + (i * 2), "%02x", hash[i]);
  }
  hexString[64] = '\0';
  return String(hexString);
}

bool ValidateFirmwareCheckSum(uint8_t* data, size_t len, const String& expectedChecksum) {
  if (expectedChecksum.length() == 0) {
    Serial.println("[OTA] No checksum provided, skipping validation");
    return true;
  }

  String calculatedChecksum = CalculateSHA256(data, len);

  if (calculatedChecksum.equalsIgnoreCase(expectedChecksum)) {
    Serial.println("[OTA] Checksum validation PASSED");
    return true;
  } else {
    Serial.println("[OTA] Checksum validation FAILED");
    Serial.println("  Expected: " + expectedChecksum);
    Serial.println("  Got:      " + calculatedChecksum);
    ota.lastError = "Checksum mismatch";
    return false;
  }
}



bool downloadAndFlashFirmware(const String& url, const String& checksum) {
  Serial.println("\n[OTA] ========== FIRMWARE DOWNLOAD START ==========");
  Serial.println("[OTA] URL: " + url);
  Serial.println("[OTA] Expected checksum: " + checksum);

  HTTPClient http;
  http.setTimeout(30000);

  otaSecureClient.setInsecure(); // presigned S3 URL is HTTPS but not mTLS to your IoT CA

  if (!http.begin(otaSecureClient, url)) {
    Serial.println("[OTA] ERROR: Failed to begin HTTP connection");
    ota.lastError = "HTTP begin failed";
    return false;
  }

  int httpCode = http.GET();
  Serial.println("[OTA] HTTP Response Code: " + String(httpCode));

  if (httpCode != HTTP_CODE_OK) {
    Serial.println("[OTA] ERROR: HTTP request failed");
    ota.lastError = "HTTP " + String(httpCode);
    http.end();
    return false;
  }

  int contentLength = http.getSize();
  Serial.println("[OTA] Firmware size: " + String(contentLength) + " bytes");

  if (contentLength <= 0) {
    Serial.println("[OTA] ERROR: Invalid content length");
    ota.lastError = "Invalid content length";
    http.end();
    return false;
  }

  if (!Update.begin(contentLength)) {
    Serial.println("[OTA] ERROR: Not enough space for OTA");
    ota.lastError = "Not enough flash space";
    http.end();
    return false;
  }

  // For checksum validation we need the raw bytes as well as flashing them.
  uint8_t* fwBuffer = (uint8_t*)malloc(contentLength);
  bool haveBuffer = (fwBuffer != nullptr);
  if (!haveBuffer) {
    Serial.println("[OTA] WARNING: Not enough RAM to buffer for checksum check, will flash without validation");
  }

  Serial.println("[OTA] Starting firmware write...");

  WiFiClient* stream = http.getStreamPtr();
  uint8_t buff[512] = {0};
  int bytesWritten = 0;
  int progressPercent = 0;
  unsigned long lastMqttServiceTime = millis();

  while (http.connected() && (bytesWritten < contentLength)) {
    if (millis() - lastMqttServiceTime > 2000) {
      if (client.connected()) {
        client.loop();
      }
      lastMqttServiceTime = millis();
    }

    size_t available = stream->available();
    size_t toRead = available > sizeof(buff) ? sizeof(buff) : available;

    if (toRead > 0) {
      size_t c = stream->readBytes(buff, toRead);

      if (Update.write(buff, c) != c) {
        Serial.println("[OTA] ERROR: Write failed at offset: " + String(bytesWritten));
        ota.lastError = "Flash write failed";
        Update.abort();
        http.end();
        if (haveBuffer) free(fwBuffer);
        return false;
      }

      if (haveBuffer && (bytesWritten + (int)c) <= contentLength) {
        memcpy(fwBuffer + bytesWritten, buff, c);
      }

      bytesWritten += c;

      int newPercent = (bytesWritten * 100) / contentLength;
      if (newPercent != progressPercent && newPercent % 10 == 0) {
        Serial.println("[OTA] Progress: " + String(newPercent) + "%");
        progressPercent = newPercent;
      }
    } else {
      delay(10);
    }
  }

  Serial.println("[OTA] Download complete: " + String(bytesWritten) + " bytes");
  http.end();

  if (bytesWritten != contentLength) {
    Serial.println("[OTA] ERROR: Downloaded size mismatch");
    ota.lastError = "Incomplete download";
    Update.abort();
    if (haveBuffer) free(fwBuffer);
    return false;
  }

  if (haveBuffer) {
    bool checksumOk = ValidateFirmwareCheckSum(fwBuffer, bytesWritten, checksum);
    free(fwBuffer);
    if (!checksumOk) {
      Update.abort();
      return false;
    }
  }

  if (!Update.end()) {
    Serial.println("[OTA] ERROR: Update.end() failed");
    ota.lastError = "Update.end failed";
    return false;
  }

  if (!Update.isFinished()) {
    Serial.println("[OTA] ERROR: Update not finished");
    ota.lastError = "Update not finished";
    return false;
  }

  Serial.println("[OTA] ========== FIRMWARE FLASHED SUCCESSFULLY ==========\n");
  return true;
}

// ---------- Shared job-parsing logic ----------
// Both notify-next and $next/get/accepted deliver the pending job execution
// as an "execution" object (notify-next: single upcoming job; get/accepted:
// the job you just asked for). Parse them the same way.

void handleExecutionObject(JsonObject execution) {
  if (execution.isNull()) return;

  ota.jobId = execution["jobId"].as<String>();

  if (execution.containsKey("jobDocument")) {
    JsonObject document = execution["jobDocument"];
    ota.downloadUrl = document["url"].as<String>();
    ota.checksum = document["checksum"].as<String>();
    ota.targetVersion = document["version"].as<String>();

    Serial.println("[OTA] Job Details:");
    Serial.println("  - Job ID: " + ota.jobId);
    Serial.println("  - URL: " + ota.downloadUrl);
    Serial.println("  - Checksum: " + ota.checksum);
    Serial.println("  - Version: " + ota.targetVersion);

    ota.state = JOB_RECEIVED;
    Serial.println("[OTA] State -> JOB_RECEIVED");
  }
}

void mqtt_callback(char* topic, byte* payload, unsigned int length) {
  char msg[length + 1];
  memcpy(msg, payload, length);
  msg[length] = '\0';

  String topicStr = String(topic);

  Serial.println("\n[MQTT] Message received!");
  Serial.println("[MQTT] Topic: " + topicStr);

  DynamicJsonDocument doc(2048);
  DeserializationError error = deserializeJson(doc, msg);

  if (error) {
    Serial.println("[MQTT] JSON parse failed: " + String(error.c_str()));
    return;
  }

  if (topicStr == getJobNotifyNextTopic()) {
    Serial.println("[OTA] notify-next received");
    if (doc.containsKey("execution")) {
      handleExecutionObject(doc["execution"]);
    } else {
      Serial.println("[OTA] notify-next had no pending execution (queue empty)");
    }
    return;
  }

  if (topicStr == getJobNextGetAcceptedTopic()) {
    Serial.println("[OTA] $next/get/accepted received");
    if (doc.containsKey("execution")) {
      handleExecutionObject(doc["execution"]);
    } else {
      Serial.println("[OTA] No pending job execution");
    }
    return;
  }

  if (topicStr.endsWith("/update/accepted")) {
    Serial.println("[OTA] Job status update CONFIRMED by AWS IoT");
    return;
  }

  if (topicStr.endsWith("/update/rejected")) {
    Serial.println("[OTA] Job status update REJECTED by AWS IoT:");
    Serial.println("  " + String(msg));
    return;
  }
}

void subscribeToJobTopics() {
  Serial.println("[MQTT] Subscribing to job topics...");
  client.subscribe(getJobNotifyNextTopic().c_str());
  delay(50);
  client.subscribe(getJobNextGetAcceptedTopic().c_str());
  delay(50);
  client.subscribe(getJobUpdateAcceptedWildcardTopic().c_str());
  delay(50);
  client.subscribe(getJobUpdateRejectedWildcardTopic().c_str());
  Serial.println("[MQTT] Subscribed");
}

void requestNextJob() {
  Serial.println("[OTA] Requesting next pending job...");

  DynamicJsonDocument doc(256);
  doc["clientToken"] = String(millis());

  String payload;
  serializeJson(doc, payload);

  String topic = getJobNextGetTopic();
  client.publish(topic.c_str(), payload.c_str(), false);
}

void reportJobStatus(const String& status, const String& progressNote = "") {
  Serial.println("[OTA] Reporting status: " + status);

  DynamicJsonDocument doc(512);
  doc["status"] = status;

  JsonObject statusDetails = doc.createNestedObject("statusDetails");
  if (progressNote.length() > 0) {
    statusDetails["progress"] = progressNote;
  }
  if (ota.lastError.length() > 0) {
    statusDetails["reason"] = ota.lastError;
  }

  String payload;
  serializeJson(doc, payload);

  String topic = getJobUpdateTopic(ota.jobId);
  if (!client.publish(topic.c_str(), payload.c_str(), false)) {
    Serial.println("[OTA] WARNING: Failed to publish status");
  }
}



bool mqttConnect() {
  Serial.println("[MQTT] Connecting...");
  if (client.connect(thing_name)) {
    Serial.println("MQTT CONNECTED");
    subscribeToJobTopics();
    return true;
  } else {
    Serial.println("MQTT FAILED, rc=" + String(client.state()));
    return false;
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("\n\n========== ESP32 OTA FIRMWARE UPDATE SYSTEM ==========\n");

  
  Serial.println("[WiFi] Connecting...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[WiFi] Connected!");
    Serial.println("[WiFi] IP: " + WiFi.localIP().toString());
    delay(1000);
  } else {
    Serial.println("\n[WiFi] FAILED");
    return;
  }

  // TLS Setup for MQTT
  Serial.println("\n[TLS] Setting certificates...");
  mqttSecureClient.setCACert(root_ca);
  mqttSecureClient.setCertificate(device_cert);
  mqttSecureClient.setPrivateKey(private_key);
  mqttSecureClient.setTimeout(30);
  Serial.println("[TLS] Certs loaded");

  // MQTT Setup
  Serial.println("\n[MQTT] Configuring...");
  client.setServer(mqtt_server, 8883);
  client.setKeepAlive(60);
  client.setSocketTimeout(30);
  client.setBufferSize(2048); // job documents can exceed PubSubClient's 256B default
  client.setCallback(mqtt_callback);

  mqttConnect();

  Serial.println("\n[OTA] State -> IDLE");
  Serial.println("[OTA] Waiting for jobs... (polling every 30 seconds)\n");
}

void loop() {
  unsigned long now = millis();

  if (!client.connected()) {
    if (now - lastMqttReconnectAttempt > 5000) {
      lastMqttReconnectAttempt = now;
      mqttConnect();
    }
  } else {
    client.loop();
  }

  switch (ota.state) {
    case IDLE:
      if (client.connected() && (now - ota.lastJobCheck > 30000)) {
        ota.lastJobCheck = now;
        requestNextJob();
      }
      break;

    case JOB_RECEIVED:
      Serial.println("[OTA] State: JOB_RECEIVED - starting download...");
      ota.state = DOWNLOADING;

      reportJobStatus("IN_PROGRESS", "Downloading firmware");

      if (downloadAndFlashFirmware(ota.downloadUrl, ota.checksum)) {
        ota.state = REPORTING_SUCCESS;
      } else {
        ota.state = REPORTING_FAILURE;
      }
      break;

    case REPORTING_SUCCESS:
      Serial.println("[OTA] State: REPORTING_SUCCESS");
      reportJobStatus("SUCCEEDED", "Firmware updated successfully");
      client.loop(); // give the publish a chance to actually go out
      delay(300);
      ota.state = IDLE;

      Serial.println("\n[OTA] ========== UPDATE COMPLETE ==========");
      Serial.println("[OTA] Rebooting in 3 seconds...");
      delay(3000);
      ESP.restart();
      break;

    case REPORTING_FAILURE:
      Serial.println("[OTA] State: REPORTING_FAILURE");
      reportJobStatus("FAILED");
      client.loop();
      delay(300);
      ota.state = IDLE;

      ota.jobId = "";
      ota.downloadUrl = "";
      ota.checksum = "";
      ota.lastError = "";
      break;

    default:
      break;
  }

  delay(100);
}
