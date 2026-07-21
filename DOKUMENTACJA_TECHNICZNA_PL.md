# WROOM-BT-TX Dokumentacja Techniczna (PL)

Autor: A. Jaroszuk

## 1. Zakres
Ten dokument opisuje firmware:
- WROOM_A2DP.ino

Plytka docelowa:
- ESP32-WROOM-32D

Banner firmware:
- READY WROOM-BT-TX v1.6.5 (CB-reset enabled)

Glowna rola modulu:
- Odbior PCM z I2S (slave RX)
- Wykrywanie czestotliwosci zrodla (44.1 kHz / 48 kHz)
- Transmisja audio przez Bluetooth A2DP Source (TX)
- Interfejs komend i diagnostyki przez UART/USB

## 2. Mapowanie sprzetowe
UART (sterowanie):
- RX: GPIO16
- TX: GPIO17
- Baud: 115200

I2S (wejscie z hosta):
- BCLK: GPIO14
- WS/LRCLK: GPIO15
- DIN: GPIO32
- Format wejscia: stereo 32-bit, konwersja do 16-bit PCM wewnetrznie

## 3. Tor audio
1. Task I2S odczytuje probki 32-bit stereo.
2. Probki sa konwertowane do int16.
3. Opcjonalnie stosowany jest BOOST (100..400%).
4. Dane trafiaja do ring-buffera (8192 frames).
5. Callback A2DP pobiera dane:
- zrodlo 44.1 kHz: passthrough
- zrodlo 48 kHz: resampling 48k -> 44.1k (linear interpolation, opcjonalnie filtr HQ)

## 4. Ring-buffer i state machine (3 stany)
Rozmiar bufora:
- RB_FRAMES = 8192

Stany:
- PREFETCH: callback oddaje cisze, bufor sie napelnia
- PROCESS: normalne odtwarzanie z bufora
- DROP: redukcja przepelnienia przez odciecie starej historii i zostawienie najnowszych ramek

Progi:
- RB_PREFETCH_THRESHOLD = 2048
- RB_PROCESS_MIN_THRESHOLD = 1536
- RB_DROP_THRESHOLD = 7000
- RB_DROP_MIN_THRESHOLD = 5000

Logika:
- Start w PREFETCH
- przejscie do PROCESS po przekroczeniu progu napelnienia
- wejscie w DROP przy przepelnieniu
- w DROP bufor jest skracany do najnowszych danych
- powrot do PROCESS po zejciu ponizej progu DROP_MIN

## 5. Zachowanie sesji BT
Obsluga zdarzen polaczenia jest w on_conn_state().

Przy CONNECTED:
- zerowanie licznikow callbackow,
- czyszczenie RB i fazy resamplera,
- CHECK_SRC_RDY + START dla toru A2DP.

Przy DISCONNECTED:
- flaga polaczenia jest zerowana,
- zerowane sa liczniki diagnostyczne (CB/CBS/CBU),
- stan ring-buffera wraca do PREFETCH,
- aktywuje sie pojedynczy auto-retry (CONNECT_RETRY_MAX = 1).

Przy MODE OFF:
- callback polaczenia jest ignorowany, aby odciac opoznione eventy.

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
- wysokie CBS przy aktywnym TX: problem stanu/sterowania
- czesty DROP: dlugotrwale przepelnienie bufora

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

## 8. Skanowanie i laczenie
- Skan jest uruchamiany przez GAP inquiry (ok. 12.8 s).
- Po anulowaniu aktywnego skanu przed CONNECT stosowany jest krotki delay:
  CONNECT_POST_SCAN_DELAY_MS = 350.
- Przy timeout skanu discovery jest anulowane jawnie.
- Dostepny jest pojedynczy auto-retry CONNECT po szybkim DISCONNECTED.

## 9. Detekcja czestotliwosci zrodla
ISR liczy zbocza narastajace WS/LRCLK.
Task okresowy klasyfikuje zrodlo:
- okolice 44.1k -> SRC_44100
- okolice 48k -> SRC_48000

Przelaczanie wymaga stabilnych okien pomiarowych (anty-flapping).

## 10. Utrwalanie ustawien (NVS)
Namespace:
- btcfg

Zapisywane pola:
- mode
- vol
- boost
- mac

Komenda SAVE zapisuje aktualne ustawienia.

## 11. Zasieg i moc BT
Po starcie A2DP ustawiana jest maksymalna moc BR/EDR:
- esp_bredr_tx_power_set(ESP_PWR_LVL_P9, ESP_PWR_LVL_P9)

To moze poprawic stabilnosc na granicy zasiegu, ale nie omija ograniczen sprzetowych/zaklocen.

## 12. Integracja z S3
Bridge po stronie S3 (btbridge.cpp) powinien:
- cyklicznie pytac STATUS?,
- parsowac READY i STATE,
- uzywac synchronizacji UART i timeoutow komend.

Profil timeoutow po stronie S3:
- TX lock timeout: 200 ms
- RX lock timeout w loop: 10 ms

## 13. Build i flash
Projekt WROOM jest utrzymywany jako Arduino .ino.
Zalecany workflow:
1. Otworz WROOM_A2DP.ino w Arduino IDE.
2. Verify/Compile dla ESP32-WROOM-32D.
3. Upload do modulu WROOM.
4. Walidacja przez monitor szeregowy 115200.

## 14. Checklista po flashu
1. Banner startowy pokazuje v1.6.5 i dopisek CB-reset.
2. STATE zawiera RB z sufiksem stanu [PREFETCH|PROCESS|DROP].
3. Podczas odtwarzania: I2S_ERR=0 i CBU=0.
4. Po disconnect/reconnect: CB startuje od 0.
5. SCAN i CONNECT dzialaja, pojawiaja sie oczekiwane EVT.
6. Przy starcie pojawia sie OK BT TX POWER P9.

## 15. Wzorzec poprawnego logu
Sekwencja oczekiwana po reconnect:
- STATE ... RB=0[PREFETCH] ... CB=0
- EVT A2DP_CONN CONNECTED ...
- EVT A2DP_AUDIO STARTED ...
- STATE ... RB=xxxx[PROCESS] ... CB rosnie od 0

To potwierdza poprawna prace state machine i reset licznikow sesyjnych.
