WROOM-BT-TX Dokumentacja Techniczna (PL)

Autor: A. Jaroszuk
1. Zakres

Ten dokument opisuje firmware:

    WROOM_A2DP.ino

Plytka docelowa:

    ESP32-WROOM-32D

Banner firmware:

    READY WROOM-BT-TX v1.6.5 (CB-reset enabled)

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
