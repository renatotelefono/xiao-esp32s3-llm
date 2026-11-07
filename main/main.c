#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "esp_spiffs.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_http_server.h"
#include "esp_camera.h"
#include "driver/i2s_pdm.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "llm.h"

// ---- Dati della rete Wi-Fi ----
#include "wifi_config.h" // credenziali Wi-Fi (file non pubblicato, vedi wifi_config.example.h)

static const char *TAG = "MAIN";

// ---- Fotocamera (pin della XIAO ESP32-S3 Sense) ----
static bool cam_ok = false;
#define PART_BOUNDARY "123456789000000000000987654321"

// ---- Microfono PDM (CLK = GPIO42, DATA = GPIO41) ----
#define MIC_RATE 16000 // campioni al secondo
#define MIC_GAIN 4     // amplificazione software (prova 2..8)
static i2s_chan_handle_t mic_rx = NULL;
static bool mic_ok = false;

// gancio definito in llm.c: riceve ogni pezzo di testo generato
extern void (*llm_token_cb)(const char *piece);

// modello, tokenizer e sampler restano in memoria per tutta la vita del programma
static Transformer transformer;
static Tokenizer tokenizer;
static Sampler sampler;

static httpd_req_t *current_req = NULL; // richiesta del browser in corso
static EventGroupHandle_t wifi_events;
#define WIFI_CONNECTED_BIT BIT0

// ------------------------------------------------------------
// Pagina web (usa solo apici singoli per non complicare la stringa C)
// ------------------------------------------------------------
static const char *PAGINA =
    "<!doctype html><html lang='it'><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Cantastorie ESP32</title><style>"
    "body{font-family:system-ui,sans-serif;max-width:640px;margin:0 auto;padding:16px;background:#faf7f2;color:#222}"
    "h1{font-size:1.4em}"
    "textarea,select,input,button{font-size:1em;width:100%;box-sizing:border-box;margin:6px 0}"
    "textarea{height:4em;padding:8px}"
    "button{padding:12px;background:#c0392b;color:#fff;border:0;border-radius:8px}"
    "button:disabled{background:#999}"
    "#out{white-space:pre-wrap;background:#fff;border:1px solid #ddd;border-radius:8px;padding:12px;min-height:8em;line-height:1.5}"
    "small{color:#666}"
    "</style></head><body>"
    "<h1>Cantastorie ESP32</h1>"
    "<label>Inizio della storia (in inglese, facoltativo)</label>"
    "<textarea id='inizio' maxlength='150' placeholder='Once upon a time, there was a dragon'></textarea>"
    "<label>Fantasia: <span id='tv'>1.0</span></label>"
    "<input type='range' id='temp' min='0' max='1.5' step='0.1' value='1.0'>"
    "<label>Lunghezza</label>"
    "<select id='passi'><option value='128'>Corta</option><option value='256' selected>Media</option><option value='512'>Lunga</option></select>"
    "<button id='go'>Nuova storia</button>"
    "<div id='out'></div>"
    "<h2>Cosa vede e sente la scheda</h2>"
    "<button id='bcam'>Avvia video</button>"
    "<img id='cam' style='width:100%;border-radius:8px;background:#000;min-height:40px' alt=''>"
    "<button id='bmic'>Ascolta il microfono</button>"
    "<audio id='au' controls style='width:100%'></audio>"
    "<small>Il video arriva dalla porta 81, l'audio dalla porta 82 (con qualche secondo di ritardo).</small><br>"
    "<small>Generato dalla XIAO ESP32-S3, senza internet.</small>"
    "<script>"
    "const $=id=>document.getElementById(id);"
    "$('temp').oninput=()=>$('tv').textContent=(+$('temp').value).toFixed(1);"
    "$('go').onclick=async()=>{"
    " $('go').disabled=true;$('out').textContent='';"
    " const q='inizio='+encodeURIComponent($('inizio').value)+'&temp='+$('temp').value+'&passi='+$('passi').value;"
    " try{"
    "  const r=await fetch('/storia?'+q);"
    "  const rd=r.body.getReader();const dec=new TextDecoder();"
    "  while(true){const {done,value}=await rd.read();if(done)break;$('out').textContent+=dec.decode(value,{stream:true});}"
    " }catch(e){$('out').textContent+='\\n[Errore: '+e+']';}"
    " $('go').disabled=false;"
    "};"
    "const h=location.hostname;"
    "$('bcam').onclick=()=>{const c=$('cam');if(c.src){c.removeAttribute('src');$('bcam').textContent='Avvia video';}"
    " else{c.src='http://'+h+':81/stream';$('bcam').textContent='Ferma video';}};"
    "$('bmic').onclick=()=>{const a=$('au');if(a.src&&!a.paused){a.pause();a.removeAttribute('src');a.load();$('bmic').textContent='Ascolta il microfono';}"
    " else{a.src='http://'+h+':82/audio?t='+Date.now();a.play();$('bmic').textContent='Ferma audio';}};"
    "</script></body></html>";

// ------------------------------------------------------------
// Callback del modello
// ------------------------------------------------------------
static void on_token(const char *piece)
{
    if (current_req != NULL && piece != NULL && piece[0] != '\0')
        httpd_resp_send_chunk(current_req, piece, strlen(piece));
}

static void on_complete(float tk_s)
{
    printf("\n--- %.2f tok/s ---\n", tk_s);
    if (current_req != NULL)
    {
        char buf[64];
        int n = snprintf(buf, sizeof(buf), "\n\n[%.1f token/s]", tk_s);
        httpd_resp_send_chunk(current_req, buf, n);
    }
}

// ------------------------------------------------------------
// Utilita'
// ------------------------------------------------------------
static void url_decode(char *s)
{
    char *o = s;
    while (*s)
    {
        if (*s == '+')
        {
            *o++ = ' ';
            s++;
        }
        else if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2]))
        {
            char hex[3] = {s[1], s[2], 0};
            *o++ = (char)strtol(hex, NULL, 16);
            s += 3;
        }
        else
        {
            *o++ = *s++;
        }
    }
    *o = '\0';
}

static void init_storage(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/data",
        .partition_label = NULL,
        .max_files = 5,
        .format_if_mount_failed = false};
    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK)
        ESP_LOGE(TAG, "Errore SPIFFS (%s)", esp_err_to_name(ret));
}

// ------------------------------------------------------------
// Server web
// ------------------------------------------------------------
static esp_err_t index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, PAGINA, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t story_handler(httpd_req_t *req)
{
    char query[512] = {0};
    char prompt[256] = {0};
    char value[32];
    float temperature = 1.0f;
    int steps = 256;

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK)
    {
        if (httpd_query_key_value(query, "inizio", prompt, sizeof(prompt)) == ESP_OK)
            url_decode(prompt);
        else
            prompt[0] = '\0';
        if (httpd_query_key_value(query, "temp", value, sizeof(value)) == ESP_OK)
            temperature = strtof(value, NULL);
        if (httpd_query_key_value(query, "passi", value, sizeof(value)) == ESP_OK)
            steps = atoi(value);
    }

    if (temperature < 0.0f) temperature = 0.0f;
    if (temperature > 2.0f) temperature = 2.0f;
    if (steps < 1) steps = 1;
    if (steps > transformer.config.seq_len) steps = transformer.config.seq_len;
    sampler.temperature = temperature;

    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");

    ESP_LOGI(TAG, "Nuova storia: inizio='%s' temp=%.1f passi=%d", prompt, temperature, steps);
    current_req = req;
    generate(&transformer, &tokenizer, &sampler, prompt[0] ? prompt : NULL, steps, on_complete);
    current_req = NULL;

    httpd_resp_send_chunk(req, NULL, 0); // fine della risposta
    return ESP_OK;
}

static void start_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 16384; // la generazione gira dentro il server: serve piu' stack
    config.core_id = 0;
    config.send_wait_timeout = 30;
    config.recv_wait_timeout = 30;
    config.max_open_sockets = 5;
    config.lru_purge_enable = true;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) == ESP_OK)
    {
        httpd_uri_t index_uri = {.uri = "/", .method = HTTP_GET, .handler = index_handler};
        httpd_uri_t story_uri = {.uri = "/storia", .method = HTTP_GET, .handler = story_handler};
        httpd_register_uri_handler(server, &index_uri);
        httpd_register_uri_handler(server, &story_uri);
        ESP_LOGI(TAG, "Server web avviato");
    }
    else
    {
        ESP_LOGE(TAG, "Impossibile avviare il server web");
    }
}

// ------------------------------------------------------------
// Fotocamera
// ------------------------------------------------------------
static void camera_init(void)
{
    camera_config_t c = {
        .pin_pwdn = -1,
        .pin_reset = -1,
        .pin_xclk = 10,
        .pin_sccb_sda = 40,
        .pin_sccb_scl = 39,
        .pin_d7 = 48,
        .pin_d6 = 11,
        .pin_d5 = 12,
        .pin_d4 = 14,
        .pin_d3 = 16,
        .pin_d2 = 18,
        .pin_d1 = 17,
        .pin_d0 = 15,
        .pin_vsync = 38,
        .pin_href = 47,
        .pin_pclk = 13,
        .xclk_freq_hz = 20000000,
        .ledc_timer = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,
        .pixel_format = PIXFORMAT_JPEG,
        .frame_size = FRAMESIZE_VGA, // 640x480
        .jpeg_quality = 12,          // 10 = migliore, 30 = piu' leggera
        .fb_count = 2,
        .fb_location = CAMERA_FB_IN_PSRAM,
        .grab_mode = CAMERA_GRAB_LATEST,
    };
    esp_err_t err = esp_camera_init(&c);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Fotocamera non trovata (%s): controlla che la scheda Sense sia innestata", esp_err_to_name(err));
        return;
    }
    cam_ok = true;
    ESP_LOGI(TAG, "Fotocamera pronta");
}

static esp_err_t stream_handler(httpd_req_t *req)
{
    if (!cam_ok)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Fotocamera non disponibile");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "multipart/x-mixed-replace;boundary=" PART_BOUNDARY);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    char part[96];
    ESP_LOGI(TAG, "Video avviato");
    while (true)
    {
        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb)
        {
            ESP_LOGW(TAG, "Fotogramma perso");
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        int n = snprintf(part, sizeof(part),
                         "\r\n--" PART_BOUNDARY "\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n",
                         (unsigned)fb->len);
        esp_err_t r = httpd_resp_send_chunk(req, part, n);
        if (r == ESP_OK)
            r = httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len);
        esp_camera_fb_return(fb);
        if (r != ESP_OK)
            break; // il browser ha chiuso
    }
    ESP_LOGI(TAG, "Video fermato");
    return ESP_OK;
}

// ------------------------------------------------------------
// Microfono
// ------------------------------------------------------------
static void mic_init(void)
{
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    if (i2s_new_channel(&cc, NULL, &mic_rx) != ESP_OK)
        return;
    i2s_pdm_rx_config_t pc = {
        .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(MIC_RATE),
        .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .clk = GPIO_NUM_42,
            .din = GPIO_NUM_41,
            .invert_flags = {.clk_inv = false},
        },
    };
    if (i2s_channel_init_pdm_rx_mode(mic_rx, &pc) != ESP_OK || i2s_channel_enable(mic_rx) != ESP_OK)
    {
        ESP_LOGE(TAG, "Microfono non disponibile");
        return;
    }
    mic_ok = true;
    ESP_LOGI(TAG, "Microfono pronto (%d Hz)", MIC_RATE);
}

static void wav_header(uint8_t *h, uint32_t rate)
{
    uint32_t byte_rate = rate * 2;
    uint32_t big = 0x7FFFFFFF; // durata "infinita": e' uno streaming
    memcpy(h, "RIFF", 4);
    memcpy(h + 4, &big, 4);
    memcpy(h + 8, "WAVEfmt ", 8);
    uint32_t fmt_len = 16;
    uint16_t pcm = 1, ch = 1, align = 2, bits = 16;
    memcpy(h + 16, &fmt_len, 4);
    memcpy(h + 20, &pcm, 2);
    memcpy(h + 22, &ch, 2);
    memcpy(h + 24, &rate, 4);
    memcpy(h + 28, &byte_rate, 4);
    memcpy(h + 32, &align, 2);
    memcpy(h + 34, &bits, 2);
    memcpy(h + 36, "data", 4);
    memcpy(h + 40, &big, 4);
}

static esp_err_t audio_handler(httpd_req_t *req)
{
    if (!mic_ok)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Microfono non disponibile");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "audio/wav");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");

    uint8_t header[44];
    wav_header(header, MIC_RATE);
    if (httpd_resp_send_chunk(req, (const char *)header, sizeof(header)) != ESP_OK)
        return ESP_OK;

    static int16_t buf[1024];
    ESP_LOGI(TAG, "Audio avviato");
    while (true)
    {
        size_t bytes = 0;
        if (i2s_channel_read(mic_rx, buf, sizeof(buf), &bytes, pdMS_TO_TICKS(1000)) != ESP_OK)
            continue;
        int n = bytes / 2;
        for (int i = 0; i < n; i++)
        {
            int v = buf[i] * MIC_GAIN;
            if (v > 32767) v = 32767;
            if (v < -32768) v = -32768;
            buf[i] = (int16_t)v;
        }
        if (httpd_resp_send_chunk(req, (const char *)buf, bytes) != ESP_OK)
            break; // il browser ha chiuso
    }
    ESP_LOGI(TAG, "Audio fermato");
    return ESP_OK;
}

static void start_media_servers(void)
{
    // ogni flusso ha il suo server, cosi' video, audio e storie non si bloccano a vicenda
    httpd_config_t vc = HTTPD_DEFAULT_CONFIG();
    vc.server_port = 81;
    vc.ctrl_port = 32769;
    vc.max_open_sockets = 2;
    vc.lru_purge_enable = true;
    vc.stack_size = 6144;
    vc.core_id = 0;
    httpd_handle_t vs = NULL;
    if (httpd_start(&vs, &vc) == ESP_OK)
    {
        httpd_uri_t u = {.uri = "/stream", .method = HTTP_GET, .handler = stream_handler};
        httpd_register_uri_handler(vs, &u);
    }
    else
        ESP_LOGE(TAG, "Server video non avviato");

    httpd_config_t ac = HTTPD_DEFAULT_CONFIG();
    ac.server_port = 82;
    ac.ctrl_port = 32770;
    ac.max_open_sockets = 2;
    ac.lru_purge_enable = true;
    ac.stack_size = 6144;
    ac.core_id = 0;
    httpd_handle_t as = NULL;
    if (httpd_start(&as, &ac) == ESP_OK)
    {
        httpd_uri_t u = {.uri = "/audio", .method = HTTP_GET, .handler = audio_handler};
        httpd_register_uri_handler(as, &u);
    }
    else
        ESP_LOGE(TAG, "Server audio non avviato");
}

// ------------------------------------------------------------
// Wi-Fi
// ------------------------------------------------------------
static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
    }
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED)
    {
        ESP_LOGW(TAG, "Wi-Fi non connesso, riprovo...");
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_wifi_connect();
    }
    else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        printf("\n==========================================\n");
        printf("  Apri nel browser:  http://" IPSTR "\n", IP2STR(&e->ip_info.ip));
        printf("==========================================\n\n");
        xEventGroupSetBits(wifi_events, WIFI_CONNECTED_BIT);
    }
}

static void wifi_init(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        nvs_flash_erase();
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    wifi_events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    wifi_config_t wc = {0};
    memcpy(wc.sta.ssid, RETE, strlen(RETE));
    memcpy(wc.sta.password, PASSWORD, strlen(PASSWORD));
    wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_ps(WIFI_PS_NONE); // piu' reattivo

    ESP_LOGI(TAG, "Connessione a %s...", RETE);
    xEventGroupWaitBits(wifi_events, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
}

// ------------------------------------------------------------
void app_main(void)
{
    // 0. fotocamera e microfono (prima del modello: la fotocamera vuole memoria DMA libera)
    camera_init();
    mic_init();

    // 1. carica il modello
    init_storage();
    build_transformer(&transformer, "/data/stories260K.bin");
    build_tokenizer(&tokenizer, "/data/tok512.bin", transformer.config.vocab_size);
    unsigned long long seed = esp_random();
    if (seed == 0)
        seed = 1;
    build_sampler(&sampler, transformer.config.vocab_size, 1.0f, 0.9f, seed);
    llm_token_cb = on_token;

    // 2. Wi-Fi e server web
    wifi_init();
    start_server();
    start_media_servers();
    ESP_LOGI(TAG, "Pronto! Apri l'indirizzo mostrato sopra.");
}