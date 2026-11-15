# Daikin BRY88AB151K: migrate through stock OTA

In [Firmware & resources]({{SITE_URL}}en/#releases), select the latest compatible stable release for **Daikin air quality detector (G1/G2)**, the `full` variant and **Native OTA**. The stock sender accepts newer images meeting the format and size limits below; installing the old `0.9.21-dirty` transition image first is unnecessary. RC builds are for beta testing.

Choose your computer’s operating system below. Checking the file and uploading will both show matching commands. On Windows, use PowerShell; on macOS or Linux, use Terminal. Without JavaScript, all commands remain visible.

<!-- os-picker -->

## 1. Prepare the device and computer

Use a stable device power supply and a computer with 2.4GHz Wi-Fi and Python 3. The sender needs only the Python standard library. Download the files before switching the computer to the device hotspot. Ethernet can keep the computer online while Wi-Fi connects to the device, but internet access is not required during flashing.

Have a recovery method available. For a first attempt, keep your own complete 4MB serial Flash backup. Another device's complete backup can overwrite identity and private settings.

Stock OTA listens on TCP 8555 after entering update mode. It writes the inactive application slot, selects it for boot, and restarts. **Entering OTA mode already initializes and erases the inactive slot.** Keep power connected.

## 2. Download and check the files

- [Python sender](../assets/tools/daikin-stock-ota/daikin_stock_ota.py).
- In [Firmware & resources]({{SITE_URL}}en/#releases), select Daikin G1/G2, the latest compatible stable release and the `full` variant. Download `daikin_BRY88AX151K.ota.bin.raw` and `SHA256SUMS` from that same release. Use the actual published filename if it differs.

Keep all three files in one directory. **Stock installation requires a native application image, not `.all.bin`, a full-flash backup, `.ota.bin.signed`, the `hub` variant or G3 firmware.** Each stock application slot is **1,572,864 bytes (1.5 MiB)**; that limit remains after migration. The sender checks SHA-256, ESP32 image format and size.

Replace `SHA256_FROM_MANIFEST` below with the **64-character SHA-256** on the `daikin_BRY88AX151K.ota.bin.raw` line in your downloaded `SHA256SUMS`. Do not reuse the old tutorial image's digest.

Open a terminal in the download directory and check the file without contacting the device.

<!-- os:windows -->

Windows PowerShell:

```powershell
py -3 .\daikin_stock_ota.py .\daikin_BRY88AX151K.ota.bin.raw --sha256 SHA256_FROM_MANIFEST --check-only
```

<!-- /os -->

<!-- os:mac linux -->

macOS / Linux:

```sh
python3 ./daikin_stock_ota.py ./daikin_BRY88AX151K.ota.bin.raw --sha256 SHA256_FROM_MANIFEST --check-only
```

<!-- /os -->

Install Python 3 first if unavailable. Stop on a hash, format or size mismatch and check the model, variant and matching checksum manifest.

## 3. Trigger stock OTA with the button

With the stock application running, locate the **RESET pinhole on the back of the device**. Use a suitable thin tool to gently press the button inside, then follow this sequence:

1. Hold for approximately **3 seconds**, then release for approximately **0.5 seconds**.
2. Hold for approximately **3 seconds**, then release for approximately **0.5 seconds**.
3. Hold for approximately **3 seconds**, then release.

Each hold must fall between approximately 2 and 5 seconds. Keep each release-to-next-press gap below 2 seconds. Do not replace the sequence with one long hold or hold beyond 5 seconds. This sequence started the mode 0 OTA task on the test device.

**In some stock provisioning states, a long hold clears settings and restarts the device instead.** The code checks an internal `wififlag`; this gesture is not guaranteed to preserve settings in every mode. If the device restarts or the gesture fails, observe its state rather than repeatedly extending the hold time.

If a serial console is available, success includes:

```text
Start OTA Task Suceess, mode 0
app_ota_server_create success
```

Serial access is useful for diagnosis but is not required for the wireless transfer itself.

## 4. Connect to the stock hotspot

Select the device's `daikin!` hotspot using the computer's Wi-Fi menu. For this stock version, the default hotspot password is **`12345678`**.

The tested SSID was `daikin!f76748f9`; the suffix varies by device. Check your own device identity. The tested STA MAC was `9c:9c:1f:f7:67:48` and its AP BSSID was `9c:9c:1f:f7:67:49`.

The test computer received `192.168.4.2/24` through DHCP, with device/gateway **`192.168.4.1`**. Keep this connection even if the OS reports no internet. Do not reuse the device's old router-assigned address, and prevent automatic switching to another Wi-Fi network.

On Windows, use the Wi-Fi menu when possible. In the remote SSH test, a current-user `netsh` profile returned `0x57`; an all-users profile for this hotspot worked. Do not delete unrelated network profiles.

## 5. Probe the accepted version label

Stock firmware accepts only a version label higher than its current version. The old fixed `.16` label therefore fails on newer devices. Stay on the device network and probe an explicit range without an image:

<!-- os:windows -->

```powershell
py -3 .\daikin_stock_ota.py --probe-versions SV.02.00.01.16 SV.02.00.01.32
```

<!-- /os -->

<!-- os:mac linux -->

```sh
python3 ./daikin_stock_ota.py --probe-versions SV.02.00.01.16 SV.02.00.01.32
```

<!-- /os -->

The scan waits **2 seconds** between attempts and stops immediately on `ToDo`, an unexpected response, a timeout or another communication error. It sends only handshake/version headers: **0 firmware bytes**, no `End` or checksum, no Wi-Fi switching and no automatic upload.

For example, if `.16` through `.20` return `b'No' / EOF` and `.21` returns `b'ToDo' / complete`, use `.21` as the first accepted label in the upload command.

Use the first accepted label explicitly in the separate upload command below; change the example `.21` to your result. If communication failed, resolve it before trying another command.

## 6. Upload the application

Confirm that the computer is connected to your device's hotspot, then run the sender. Its default destination is `192.168.4.1:8555`.

<!-- os:windows -->

Windows PowerShell:

```powershell
py -3 .\daikin_stock_ota.py .\daikin_BRY88AX151K.ota.bin.raw --sha256 SHA256_FROM_MANIFEST --version SV.02.00.01.21
```

<!-- /os -->

<!-- os:mac linux -->

macOS / Linux:

```sh
python3 ./daikin_stock_ota.py ./daikin_BRY88AX151K.ota.bin.raw --sha256 SHA256_FROM_MANIFEST --version SV.02.00.01.21
```

<!-- /os -->

`--version` is the stock protocol admission label, not the community firmware version; it does not modify the image. The uploader requires this explicit choice and never reuses a probe result automatically. Confirm the image's model, layout and SHA-256 independently.

Expected output includes:

```text
Kit2 Version Up -> Okey
SV.02.00.01.21 -> ToDo
ACK .../<image bytes> (...%)
Final response: b'OK'
```

Wait for the final `OK` and reboot. The sender waits for each length ACK and separates `End` from the final checksum. This service is not an HTTP upload endpoint: do not replace the sender with `curl` or combine protocol messages into one write.

## 7. Verify the community application

After boot, join the open hotspot **`BRY88AX151K-AP-<last six MAC digits in uppercase>`** and open **http://192.168.0.1/**. Check the model, device ID and that the version matches your selected download, then configure router Wi-Fi, a management password and MQTT. Once connected to your router, use its assigned IP.

Check readings after sensor warm-up. CO₂ may take longer; devices without a formaldehyde sensor have no HCHO reading.

The stock bootloader and partition table remain, and the application starts from the second slot at `0x190000`. Configure Wi-Fi again on first boot. Later images must still fit the stock 1.5 MiB slots.

### If you already installed 0.9.21-dirty from the old guide

The old `daikin-bry88ab.application.bin` attachment is a fixed `0.9.21-2cfd89-dirty` snapshot. It is not the current release. Join its `DAIKIN-<last six MAC digits>` hotspot, open `http://192.168.4.1/` and configure router Wi-Fi first.

From [Downloads]({{SITE_URL}}en/#releases), obtain the latest compatible stable Daikin G1/G2 `full` image, `daikin_BRY88AX151K.ota.bin.raw`, and its matching checksums. Verify it and install it through the old WebUI's local firmware upload. That old version accepts a native application image; after installing a version supporting signatures, use `.ota.bin.signed` for normal updates as described in [Firmware updates](firmware-update). Keep power connected. After reboot, check the version, sensor readings and network settings. The new recovery hotspot and address follow the format at the start of this section.

## 8. Troubleshooting and recovery

| Symptom | Action |
| --- | --- |
| No hotspot | Check model/version, normal stock boot and gesture timing. The OTA task does not necessarily create an AP in every configuration state. |
| Hotspot connects but TCP 8555 times out/refuses | Check network switching and the actual gateway. Inspect the two OTA startup log markers if available. |
| Handshake or version rejected | Use the handshake-only scan in section 5. Select an accepted label explicitly for upload, and check image compatibility separately. |
| Transfer interrupted, invalid ACK or final NG | Save the output and observe the powered device. Wait for reboot and confirm the community version before another update. |
| OK but no new hotspot | Wait for reboot, rescan and inspect the device startup logs. |
| Device will not boot | Stop repeated wireless update attempts and seek serial recovery using your own complete Flash backup. |
