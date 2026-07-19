# WROOM-BT-TX Dokumentacja Techniczna (PL)

Autor: A. Jaroszuk

## 1. Zakres
Ten dokument opisuje firmware:
- WROOM_A2DP.ino

Płytka docelowa:
- ESP32-WROOM-32D

Banner firmware:
- READY WROOM-BT-TX v1.6.4 (CB-reset enabled)

Główna rola modułu:
- Odbiór PCM z I2S (slave RX)
- Wykrywanie częstotliwości źródła (44.1 kHz / 48 kHz)
- Transmisja audio przez Bluetooth A2DP Source (TX)
- Interfejs komend i diagnostyki przez UART/USB

## 2. Mapowanie sprzętowe
UART (sterowanie):
- RX: GPIO16
- TX: GPIO17
- Baud: 115200

I2S (wejście z hosta):
- BCLK: GPIO14
- WS/LRCLK: GPIO15
- DIN: GPIO32
- Format wejścia: stereo 32-bit, konwersja do 16-bit PCM wewnętrznie

## 3. Tor audio
1. Task I2S odczytuje próbki 32-bit stereo.
2. Próbki są konwertowane do int16.
3. Opcjonalnie stosowany jest BOOST (100..400%).
4. Dane trafiają do ring-buffera (8192 frames).
5. Callback A2DP pobiera dane:
- źródło 44.1 kHz: passthrough
- źródło 48 kHz: resampling 48k -> 44.1k (interpolacja liniowa)

## 4. Ring-buffer i state machine (3 stany)
Rozmiar bufora:
- RB_FRAMES = 8192

Stany:
- PREFETCH: callback oddaje ciszę, bufor się napełnia
- PROCESS: normalne odtwarzanie z bufora
- DROP: redukcja przepełnienia przez pomijanie części ramek

Progi:
- RB_PREFETCH_THRESHOLD = 2048
- RB_PROCESS_MIN_THRESHOLD = 1024
- RB_DROP_THRESHOLD = 6000
- RB_DROP_MIN_THRESHOLD = 4000

Logika:
- Start w PREFETCH
- przejście do PROCESS po przekroczeniu progu napełnienia
- wejście w DROP przy przepełnieniu
- powrót do PROCESS po zejściu poniżej progu DROP_MIN

## 5. Zachowanie sesji BT
Obsługa zdarzeń połączenia w on_conn_state().

Przy DISCONNECTED:
- flaga połączenia jest zerowana
- zerowane są liczniki diagnostyczne:
  - CB
  - CBS
  - CBU
- stan ring-buffera wraca do PREFETCH

To daje liczniki per sesja po reconnect.

## 6. Diagnostyka i liczniki
Linia STATE zawiera:
- BT, MODE, VOL(127), BOOST
- SRC (klasa + pomiar Hz)
- RB ze stanem: RB=<frames>[PREFETCH|PROCESS|DROP]
- SCAN, CONN, MAC, NAME
- I2S_OK, I2S_ERR, I2S_ZR, I2S_B
- CB, CBS, CBU

Szybka interpretacja:
- I2S_ERR=0 i CBU=0: tor audio stabilny
- wysokie CBU: underrun (brak danych)
- wysokie CBS w aktywnym TX: problem stanu/sterowania
- częsty DROP: długotrwałe przepełnienie bufora

## 7. Interfejs komend
Transport:
- UART2 i USB serial
- komendy liniowe, parser case-insensitive

Wspierane komendy:
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

Typowe odpowiedzi:
- OK ...
- ERR ...
- EVT ... (A2DP_CONN, A2DP_AUDIO, GAP)
- STATE ...

## 8. Detekcja częstotliwości źródła
ISR liczy zbocza narastające WS/LRCLK.
Task okresowy klasyfikuje źródło:
- okolice 44.1k -> SRC_44100
- okolice 48k -> SRC_48000

Przełączanie wymaga stabilnych okien pomiarowych (anty-flapping).

## 9. Utrwalanie ustawień (NVS)
Namespace:
- btcfg

Zapisywane pola:
- mode
- vol
- boost
- mac

Komenda SAVE zapisuje aktualne ustawienia.

## 10. Integracja z S3
Bridge po stronie S3 (btbridge.cpp) powinien:
- cyklicznie pytać STATUS?
- parsować READY i STATE
- używać synchronizacji UART i timeoutów komend

Profil timeoutów po stronie S3:
- TX lock timeout: 200 ms
- RX lock timeout w loop: 10 ms

## 11. Build i flash
Projekt WROOM jest utrzymywany jako Arduino .ino.
Zalecany workflow:
1. Otwórz WROOM_A2DP.ino w Arduino IDE.
2. Verify/Compile dla ESP32-WROOM-32D.
3. Upload do modułu WROOM.
4. Walidacja przez monitor szeregowy 115200.

## 12. Checklista po flashu
1. Banner startowy pokazuje v1.6 i dopisek CB-reset.
2. STATE zawiera RB z sufiksem stanu [PREFETCH|PROCESS|DROP].
3. Podczas odtwarzania: I2S_ERR=0 i CBU=0.
4. Po disconnect/reconnect: CB startuje od 0.
5. SCAN i CONNECT działają, pojawiają się oczekiwane EVT.

## 13. Wzorzec poprawnego logu
Sekwencja oczekiwana po reconnect:
- STATE ... RB=8191[PREFETCH] ... CB=0
- EVT A2DP_AUDIO STARTED ...
- STATE ... RB=xxxx[PROCESS] ... CB rośnie od 0

To potwierdza poprawną pracę state machine i reset liczników sesyjnych.
