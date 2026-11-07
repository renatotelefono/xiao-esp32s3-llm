# LLM su XIAO ESP32-S3 Sense

Un piccolo modello linguistico (260K parametri, addestrato su TinyStories) che gira
interamente su una **Seeed XIAO ESP32-S3 Sense**, senza internet, a circa **34 token/s**.

La scheda si collega al Wi-Fi e crea una pagina web: dal browser del telefono o del PC
si scrive (facoltativamente) l'inizio della storia, si sceglie fantasia e lunghezza,
e le parole compaiono una alla volta mentre vengono generate.

## Hardware
- Seeed XIAO ESP32-S3 Sense (8 MB flash, 8 MB PSRAM ottale)
- Antenna Wi-Fi inclusa nella confezione (rete a 2,4 GHz)

## Come usarlo
1. Installa **ESP-IDF v5.4** (testato con v5.4.4).
2. Copia `main/wifi_config.example.h` in `main/wifi_config.h` e inserisci nome e password della rete.
3. Compila e carica:
   ```
   idf.py set-target esp32s3
   idf.py -p COMx flash monitor
   ```
4. Nel monitor seriale compare l'indirizzo, ad esempio `http://192.168.1.57`: aprilo nel browser.

## Struttura
- `main/main.c` – Wi-Fi, server web e pagina HTML con streaming del testo
- `main/llm.c`, `main/llm.h` – motore di inferenza (llama2.c adattato all'ESP32, su due core)
- `data/stories260K.bin` – pesi del modello; `data/tok512.bin` – vocabolario.
  Vengono impacchettati in una partizione SPIFFS e caricati in PSRAM all'avvio.

## Modifiche rispetto al progetto originale
- Rimosso il display OLED: output via monitor seriale e via web
- Interfaccia web con streaming parola per parola (gancio `llm_token_cb` in `llm.c`)
- Seme casuale hardware (`esp_random`): storia diversa a ogni richiesta
- Corretto `typedef float v4sf` in `llm.h`: l'attributo `aligned(16)` con GCC 14
  faceva leggere dati non allineati e il modello produceva testo senza senso
- Prodotti scalari in C semplice al posto di `dsps_dotprod_f32_aes3` (esp-dsp)
- Corretti due errori di tipo che GCC 14 tratta come errori

## Crediti
- [llama2.c](https://github.com/karpathy/llama2.c) di Andrej Karpathy (licenza MIT), incluso il modello stories260K
- [esp32-llm](https://github.com/DaveBben/esp32-llm) di Dave Bennett, da cui deriva il porting per ESP32
