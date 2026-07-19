----------------------------------------
Jak czytać linię STATE (log UART)
----------------------------------------


Przykład:
[09:00:19] STATE BT=ON MODE=TX VOL=80(127=102) BOOST=125 SRC=48000(47988Hz) RB=7019 SCAN=0 CONN=1 MAC=39:F8:94:61:72:F0 NAME="FreeMotion B552" I2S_OK=40220 I2S_ERR=0 I2S_ZR=298 I2S_B=82370560 CB=59508 CBS=0 CBU=0

Opis pól:

[09:00:19] - czas wpisu logu.
STATE - pełny snapshot bieżącego stanu modułu BT.
BT=ON - stos Bluetooth aktywny.
MODE=TX - tryb nadajnika A2DP (wysyłanie audio do słuchawek).
VOL=80(127=102) - głośność UI 0..100 oraz wartość wewnętrzna 0..127.
BOOST=125 - dodatkowe wzmocnienie audio w procentach.
SRC=48000(47988Hz) - tryb/klasa źródła i chwilowy pomiar częstotliwości.
RB=7019 - poziom ring-buffera audio (jednostka wewnętrzna).
SCAN=0 - skanowanie BT: 0=nie, 1=tak.
CONN=1 - stan połączenia A2DP: 1=połączono, 0=rozłączono.
MAC=... - adres BT aktywnego urządzenia (lub None przy braku).
NAME="..." - nazwa aktywnego urządzenia.
I2S_OK - licznik udanych odczytów I2S.
I2S_ERR - licznik błędów i2s_read (docelowo 0).
I2S_ZR - licznik odczytów 0 bajtów (krótkie przerwy źródła).
I2S_B - łączna liczba bajtów odebranych z I2S od startu.
CB - liczba wywołań callbacku A2DP (get_data).
CBS - liczba callbacków, które zwróciły ciszę (np. OFF/brak gotowości).
CBU - liczba underrunów callbacku (za mało danych do wysłania).
Szybka interpretacja jakości toru audio:

I2S_ERR=0 i CBU=0 -> tor audio stabilny.
Rosnące CBU -> braki danych audio (underrun).
Rosnące CBS w MODE=TX podczas grania -> sygnał problemu stanu/buforowania.
CONN=1 przy MAC=None -> stan podejrzany (często auto-connect/scan-side effect), warto potwierdzić eventami A2DP.