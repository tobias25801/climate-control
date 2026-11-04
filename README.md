# Smart Bedroom Climate Control

**Inteligentný systém monitorovania a riadenia prostredia v spálni**

Projekt stredoškolskej odbornej činnosti (SOČ). Mikrokontrolér ESP32 meria teplotu, vlhkosť, osvetlenie a pohyb, zobrazuje hodnoty na LCD, automaticky riadi ventilátor a svetlo a pripravuje upozornenia na Discord.

Projekt je vytvorený a testovaný v simulátore **Wokwi**. 

Wokwi projekt: https://wokwi.com/projects/479602791671279617 (pozor, odkaz zobrazuje aj `secrets.h`, pozri časť Bezpečnosť)

## Čo systém robí

- Meria teplotu a vlhkosť (DHT22), osvetlenie (LDR) a pohyb (PIR).
- Na LCD 16×2 sa striedajú tri obrazovky: hodnoty, režim a stav výstupov, stav Wi-Fi a Discordu.
- **Ventilátor:** zapne pri 28 °C, vypne pri 26 °C (hysteréza). Pri chybe senzora DHT22 sa po 60 s zapne (fail-safe).
- **Svetlo:** v automatickom režime sa zapne pri tme a pohybe a vypne po 10 s bez pohybu.
- **Stavové LED:** zelená = norma, žltá = zvýšené hodnoty (teplota ≥ 27 °C alebo vlhkosť mimo 30–70 %), červená = kritický stav (≥ 32 °C) alebo chyba senzora.
- **Discord upozornenia:** pri zmene stavu (vysoká teplota, vlhkosť mimo rozsahu, kritický stav, ventilátor, chyba senzora, obnovenie normy). Každý typ má vlastný cooldown, aby sa správy nespamovali.
- Kód je neblokujúci (`millis()`). Odosielanie na Discord beží v samostatnej FreeRTOS úlohe s frontou správ (max. 8), takže pomalý alebo nedostupný internet nezastaví lokálne riadenie.

## Komponenty

| Komponent | Úloha |
|---|---|
| ESP32 DevKit C V4 | riadiaca jednotka, Wi-Fi |
| DHT22 | teplota a vlhkosť |
| LDR modul (fotorezistor) | úroveň osvetlenia |
| PIR senzor | detekcia pohybu |
| LCD 1602 s I²C | zobrazenie údajov |
| 3× tlačidlo | ovládanie |
| LED modrá | simulácia ventilátora |
| LED biela | svetlo |
| LED zelená, žltá, červená | stavová indikácia |
| 5× rezistor 220 Ω | predradné rezistory LED |

Vo Wokwi neexistuje komponent ventilátora, preto ho simuluje modrá LED. V reálnom zapojení by sa ventilátor a svetlo spínali cez relé alebo tranzistor.

## Zapojenie pinov

| Súčiastka | GPIO |
|---|---|
| DHT22 (SDA) | 4 |
| LDR modul (AO) | 34 |
| PIR (OUT) | 27 |
| LCD SDA / SCL | 21 / 22 |
| Tlačidlo 1 / 2 / 3 | 13 / 14 / 33 |
| LED ventilátor / svetlo | 26 / 25 |
| LED zelená / žltá / červená | 16 / 17 / 18 |

Tlačidlá sú zapojené medzi GPIO a GND (interný pull-up). Každá LED má sériový rezistor 220 Ω. Presné zapojenie je v `diagram.json`.

## Ovládanie

| Tlačidlo | Režim AUTO | Režim MANUAL |
|---|---|---|
| BTN1 (GPIO 13) | prepne na MANUAL | prepne na AUTO |
| BTN2 (GPIO 14) | prepne obrazovku LCD | zapne/vypne svetlo |
| BTN3 (GPIO 33) | bez funkcie | zapne/vypne ventilátor |

V režime MANUAL automatika neprepisuje ručné príkazy.

## Ako otvoriť a spustiť vo Wokwi

1. Na [wokwi.com](https://wokwi.com) vytvor nový projekt **ESP32**.
2. Obsah `diagram.json` z tohto repozitára vlož do záložky `diagram.json`.
3. Obsah `libraries.txt` vlož do záložky `libraries.txt`.
4. Obsah `sketch.ino` vlož do záložky `sketch.ino`.
5. Vytvor nový súbor `secrets.h` podľa `secrets.example.h`.
6. Klikni na **Start** a otvor Serial Monitor.

Hodnoty senzorov meníš kliknutím na súčiastku vo Wokwi (teplota a vlhkosť DHT22, úroveň svetla LDR, simulácia pohybu PIR).

## Discord upozornenia

Správy sa posielajú cez Discord webhook. V `sketch.ino` sú dva režimy:

- `#define DISCORD_DRY_RUN 1` (predvolené): správy sa len vypisujú do Serial Monitora s predponou `[DRY-RUN]`, nič sa neodosiela.
- `#define DISCORD_DRY_RUN 0`: správy sa odosielajú na webhook cez HTTPS.

### Nastavenie webhooku (pre skutočné odosielanie)

1. V Discorde: nastavenia kanála → **Integrácie → Webhooky → Nový webhook → Kopírovať URL**.
2. Skopíruj `secrets.example.h` ako `secrets.h` a doplň svoje údaje:
   ```cpp
   #define WIFI_SSID     "Wokwi-GUEST"
   #define WIFI_PASSWORD ""
   #define WIFI_CHANNEL  6
   #define DISCORD_WEBHOOK_URL "https://discord.com/api/webhooks/..."
   ```
   Pri reálnej Wi-Fi sieti nastav vlastné SSID a heslo a `WIFI_CHANNEL 0`.
3. V `sketch.ino` nastav `DISCORD_DRY_RUN` na `0`.
4. Pre upozornenia v mobile nechaj v aplikácii Discord zapnuté notifikácie pre daný kanál.

### Bezpečnosť

Webhook URL funguje ako heslo: kto ju má, môže písať do kanála. Súbor `secrets.h` je v `.gitignore` a nesmie sa dostať do repozitára. Ak sa webhook niekde zverejní, zmaž ho v Discorde a vytvor nový. Pri zdieľaní Wokwi projektu odkazom nechaj v `secrets.h` falošnú URL.

## Čo je overené a čo nie

| Funkcia | Stav |
|---|---|
| Čítanie senzorov, LCD, ventilátor, svetlo, stavové LED, tlačidlá | PASS (Wokwi simulátor) |
| Pripojenie na Wi-Fi (`Wokwi-GUEST`) | PASS (Wokwi simulátor) |
| Logika upozornení v režime DRY-RUN (zmena stavu, cooldown, texty, fronta) | PASS (Wokwi simulátor) |
| Upozornenie na dlhodobo vysokú teplotu a opakovanie po cooldowne | NOT TESTED |
| Všeobecné HTTPS spojenie zo simulátora (`https://www.example.com`) | PASS (HTTP kód 200) |
| Skutočné odoslanie na Discord cez HTTPS zo simulátora | **FAIL**: spojenie na `discord.com` skončilo chybou `-1 (connection refused)` |
| Správanie pri výpadku a obnovení Wi-Fi | NOT TESTED |
| Odoslanie správ na Discord na fyzickom ESP32 | NOT TESTED |
| Fyzické zapojenie na reálnom ESP32 | NOT TESTED |

### Diagnostika problému s Discordom

V samostatnom Wokwi projekte som pre dve adresy vyskúšal `HTTPClient` s `WiFiClientSecure::setInsecure()`:

```
https://www.example.com             -> 200
https://discord.com/api/v10/gateway -> -1 (connection refused)
```

Wi-Fi aj HTTPS spojenie vo Wokwi teda fungujú, ale spojenie na Discord je odmietnuté. Príčinu som nezisťoval; pravdepodobne ide o obmedzenie simulátora. Chyba nie je v zadanej webhook URL (tá sa pri odmietnutom spojení vôbec nepoužije). Kvôli tomu je v projekte režim DRY-RUN, v ktorom sa dá overiť všetka logika okrem samotného odoslania. Reálne odoslanie sa dá overiť až na fyzickom ESP32 pripojenom na domácu Wi-Fi.

## Známe obmedzenia

- Ventilátor je vo Wokwi nahradený LED.
- Odosielanie na Discord nefunguje zo simulátora (pozri vyššie); funkčnosť na skutočnom zariadení nebola overená.
- LDR udáva **relatívnu úroveň v %**, nie luxy (senzor nie je kalibrovaný). LDR modul vo Wokwi je obrátený, preto je v kóde `LDR_HIGH_MEANS_DARK = true`.
- HTTPS spojenie používa `setInsecure()`, certifikát servera sa neoveruje. Pre reálne nasadenie by sa mal doplniť koreňový certifikát.
- Upozornenia, ktoré sa dostanú do cooldownu, sa zahodia (zámer proti spamu). Pri výpadku Wi-Fi sa do fronty zmestí najviac 8 správ.
- V reálnom zapojení LCD a PIR potrebujú napájanie 5 V (pri LCD treba riešiť úrovne signálov I²C).

## Štruktúra repozitára

```
├── README.md
├── .gitignore
├── sketch.ino          zdrojový kód
├── diagram.json        zapojenie pre Wokwi
├── libraries.txt       potrebné knižnice
└── secrets.example.h   vzor pre tajné údaje
```

## Použité knižnice

- DHT sensor library
- Adafruit Unified Sensor
- LiquidCrystal I2C
- WiFi, WiFiClientSecure, HTTPClient 
