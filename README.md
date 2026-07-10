# ESP32 OTA Firmware Update Pipeline (AWS IoT Core)

A complete over-the-air (OTA) firmware update system for ESP32 devices, built on AWS IoT Jobs, S3, DynamoDB, and Lambda. Devices poll for pending updates over MQTT, download new firmware over HTTPS, validate it with SHA-256, flash themselves, and report status back — no physical USB access required.

## Architecture

```
Build firmware (.bin)
        |
        v
   Amazon S3  ---------------------+
        |                          |
        v                          |
  DynamoDB (version registry)      |
        |                          |
        v                          |
   AWS Lambda (job creator) -------+  generates presigned URL
        |
        v
  AWS IoT Jobs (job lifecycle)
        |
        v (MQTT, mutual TLS)
     ESP32 Device
        |
        v (HTTPS GET, separate connection)
   Download + checksum validate + flash
        |
        v (MQTT)
  Report job status back to AWS IoT
```

## How it works

1. **Build** — compile the Arduino sketch and export the binary (`Sketch → Export Compiled Binary`).
2. **Store** — upload the `.bin` to S3.
3. **Register** — add a row to the `firmware_versions` DynamoDB table with the version string, S3 bucket/key, and SHA-256 checksum.
4. **Dispatch** — invoke the Lambda function with `{"thing_name": "...", "firmware_id": "..."}`. It looks up the firmware, generates a time-limited presigned S3 URL, and creates an AWS IoT Job.
5. **Poll** — the device polls `$aws/things/{thing}/jobs/$next/get` every 30 seconds (and reacts instantly to `notify-next` pushes).
6. **Deliver** — on receiving a job, the device downloads the firmware over a **separate** HTTPS connection (so it doesn't interfere with the live MQTT session), validates the SHA-256 checksum, and flashes itself using the ESP32 `Update` API.
7. **Confirm** — the device reports `SUCCEEDED`/`FAILED` back over MQTT, and AWS IoT confirms receipt on `.../update/accepted`.

## Repo structure

```
esp32-ota-aws-iot/
├── device/
│   └── esp32_ota_device.ino      # OTA-capable device firmware
├── lambda/
│   └── lambda_function.py        # Job creation Lambda
├── certs.h.example                # Template — fill in your own AWS IoT certs, DO NOT commit real certs
└── README.md
```

## Requirements

**Device side (Arduino IDE / PlatformIO):**
- ESP32 board support package
- Libraries: `PubSubClient`, `ArduinoJson`, `WiFiClientSecure` (bundled with ESP32 core)

**AWS side:**
- An IoT "Thing" registered in AWS IoT Core with a device certificate and policy allowing the relevant `iot:Connect`, `iot:Subscribe`, `iot:Publish`, `iot:Receive` actions on the job topics
- An S3 bucket for firmware binaries
- A DynamoDB table `firmware_versions` — partition key `version` (String), sort key `timestamp` (Number)
- A Lambda function with an execution role permitting `dynamodb:Query`, `s3:GetObject` (for presigning), and `iot:CreateJob`

## Setting up your own certs

Copy `certs.h.example` to `certs.h` and fill in:
- `root_ca` — Amazon Root CA 1
- `device_cert` — your Thing's certificate (from AWS IoT Core → Manage → Things → your thing → Certificates)
- `private_key` — the matching private key generated at certificate creation

`certs.h` is gitignored — never commit real certificates or keys.

## Known limitations / next steps

- Single-partition OTA — no A/B rollback if new firmware boots into a bad state
- No monotonic version enforcement — a job can "downgrade" a device to an older firmware with no warning
- Manual job creation per device — fine for single-device testing; a real fleet setup would add AWS IoT Thing Groups for targeting multiple devices and CI/CD automation for the build → S3 upload step, with staged/canary rollout for actual deployment
