# WROOM-BT-TX Technical Documentation

Author: A. Jaroszuk

## 1. Scope
This document describes the firmware in:
- WROOM_A2DP.ino

Target board:
- ESP32-WROOM-32D

Firmware banner:
- READY WROOM-BT-TX v1.6.4 (CB-reset enabled)

Main role:
- Receive PCM from I2S slave input
- Detect source sample rate (44.1 kHz or 48 kHz)
- Stream audio over Bluetooth A2DP Source (TX mode)
- Expose UART/USB command interface and diagnostics

## 2. Hardware Mapping
UART (control):
- RX: GPIO16
- TX: GPIO17
- Baud: 115200

I2S (input from host):
- BCLK: GPIO14
- WS/LRCLK: GPIO15
- DIN: GPIO32
- Format: 32-bit stereo samples, converted to 16-bit internal PCM

## 3. Audio Pipeline
1. I2S RX task reads 32-bit stereo frames.
2. Samples are converted to int16 stereo.
3. Optional gain is applied (BOOST 100..400%).
4. Samples are pushed to an internal ring buffer (8192 frames).
5. A2DP callback pulls audio:
- 44.1 kHz source: passthrough
- 48 kHz source: linear interpolation resampling 48k -> 44.1k

## 4. Ring Buffer and State Machine
Ring buffer size:
- RB_FRAMES = 8192

States:
- PREFETCH: callback returns silence while buffer is filling
- PROCESS: normal streaming from ring buffer
- DROP: drops part of buffered frames to reduce overfill pressure

Thresholds:
- RB_PREFETCH_THRESHOLD = 2048
- RB_PROCESS_MIN_THRESHOLD = 1024
- RB_DROP_THRESHOLD = 6000
- RB_DROP_MIN_THRESHOLD = 4000

Current behavior:
- Start in PREFETCH
- Transition to PROCESS after fill threshold
- Enter DROP when too full, return to PROCESS when reduced

## 5. Bluetooth and Session Behavior
Connection events are handled in on_conn_state().

On DISCONNECTED:
- A2DP connection flag is cleared
- Counters are reset:
  - CB (callback count)
  - CBS (silent callback count)
  - CBU (underrun callback count)
- Ringbuffer state resets to PREFETCH

This allows per-session diagnostics after reconnect.

## 6. Runtime Counters and Diagnostics
STATE line fields:
- BT, MODE, VOL(127), BOOST
- SRC(class and measured Hz)
- RB with state suffix: RB=<frames>[PREFETCH|PROCESS|DROP]
- SCAN, CONN, MAC, NAME
- I2S_OK, I2S_ERR, I2S_ZR, I2S_B
- CB, CBS, CBU

Quick health interpretation:
- I2S_ERR=0 and CBU=0: stable pipeline
- High CBU: data starvation (underrun)
- High CBS during active TX: state/control issue
- Frequent DROP state: persistent overfill pressure (tune thresholds or source flow)

## 7. Command Interface
Transport:
- UART2 and USB serial
- Line-based commands, case-insensitive parsing

Supported commands:
- HELP
- PING
- GET / STATUS?
- BT ON
- BT OFF
- MODE OFF | TX | AUTO
- VOL 0..100
- BOOST 100..400
- SCAN
- CONNECT <idx>
- CONNECT AA:BB:CC:DD:EE:FF
- DISCONNECT
- PAIRED?
- DELPAIRED ALL
- SAVE
- DBG 0|1
- HARDRESET

Important responses:
- OK ...
- ERR ...
- EVT ... (A2DP_CONN, A2DP_AUDIO, GAP events)
- STATE ...

## 8. Source Rate Detection
WS/LRCLK rising edges are counted in ISR.
A periodic task classifies source rate:
- ~44.1k range -> SRC_44100
- ~48k range -> SRC_48000

The mode switch uses stability windows to avoid flapping.

## 9. Persistence
NVS namespace: btcfg
Stored values:
- mode
- vol
- boost
- mac

SAVE command commits current configuration.

## 10. Integration Notes (S3 Side)
The paired S3 bridge (btbridge.cpp) is expected to:
- Send STATUS? periodically
- Parse READY and STATE lines
- Use UART synchronization and command timeouts

Applied timeout profile on S3 side:
- TX lock timeout: 200 ms
- RX lock timeout in loop: 10 ms

## 11. Build and Flash
Firmware is maintained as Arduino .ino project.
Typical workflow:
1. Open WROOM_A2DP.ino in Arduino IDE.
2. Verify build for ESP32-WROOM-32D target.
3. Upload to WROOM module.
4. Validate with serial monitor at 115200.

## 12. Post-Flash Validation Checklist
1. Boot banner shows v1.6.1 and CB-reset note.
2. STATUS line includes RB state suffix [PREFETCH|PROCESS|DROP].
3. During active playback: I2S_ERR=0 and CBU=0.
4. After disconnect/reconnect: CB restarts from 0.
5. SCAN and CONNECT still work with expected EVT lines.

## 13. Known Good Log Pattern
Expected sequence after reconnect:
- STATE ... RB=8191[PREFETCH] ... CB=0
- EVT A2DP_AUDIO STARTED ...
- STATE ... RB=xxxx[PROCESS] ... CB grows from 0

This confirms both state machine transitions and per-session counter reset.
