# VTom Radio - Bluetooth Bridge Panel

Panel sterowania modułem **WROOM A2DP** dla radia **VTom Radio**.

Współpracuje z: https://github.com/andrew-jar/WROOM_A2DP

### Co to jest?

Webowy panel UART/Bridge do zarządzania Bluetooth Audio w trybie TX:
- `BT ON/OFF`, `MODE TX`, `SCAN`, `CONNECT`, `DISCONNECT`
- `VOL` (0-85) + `BOOST` (80-200) - bez przesteru CSR
- `SAVE`, `PAIRED?`, `DELPAIRED ALL`, `HARDRESET`
- Live log z filtrowaniem `STATE` / `STATUS?`
- Lista `Detected Devices` - dedup po MAC, sort po ID

------------------------------------------------------

<img width="650" height="405" alt="2026-07-21_174244" src="https://github.com/user-attachments/assets/bcaa48a4-4db7-4c65-9e27-35e031aa70bd" />


WROOM-BT-TX Dokumentacja Techniczna (PL)

Autor: A. Jaroszuk
1. Zakres

Ten dokument opisuje firmware:

    WROOM_A2DP.ino

Plytka docelowa:

    ESP32-WROOM-32D

Banner firmware:

    READY WROOM-BT-TX v1.7.0 (CB-reset enabled)

Glowna rola modulu:

    Odbior PCM z I2S (slave RX)
    Wykrywanie czestotliwosci zrodla (44.1 kHz / 48 kHz)
    Transmisja audio przez Bluetooth A2DP Source (TX)
    Interfejs komend i diagnostyki przez UART/USB

2. Mapowanie sprzetowe

UART (sterowanie):

    RX: GPIO16
    TX: GPIO17
    Baud: 115200

I2S (wejscie z hosta):

    BCLK: GPIO14
    WS/LRCLK: GPIO15
    DIN: GPIO32
    Format wejscia: stereo 32-bit, konwersja do 16-bit PCM wewnetrznie

Więcej....https://github.com/andrew-jar/WROOM_A2DP/blob/main/DOKUMENTACJA_TECHNICZNA_PL.md#wroom-bt-tx-dokumentacja-techniczna-pl

## Aktualizacja 2026-08-01

- Podniesiono wydanie firmware w pliku WROOM_A2DP.ino z v1.6.5 do v1.7.0.
- Zsynchronizowano projekt z nowsza lokalna wersja firmware.
- Zaktualizowano obsluge Bluetooth A2DP pod nowsze API (linia zmian 1.7.5 -> 1.8.11, Core 3.x).
- Ulepszono logike polaczen BT: obsluge stanow, reconnect/retry oraz diagnostyke zdarzen.
- Rozszerzono logowanie statusu pracy (m.in. stan zrodla audio, ring buffer, telemetria I2S/callbackow).
- Wprowadzono poprawki stabilnosci strumieniowania audio.
