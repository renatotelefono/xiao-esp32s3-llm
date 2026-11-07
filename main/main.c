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
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "llm.h"

// ---- Dati della rete Wi-Fi ----
#include "wifi_config.h" // credenziali Wi-Fi (file non pubblicato, vedi wifi_config.example.h)

static const char *TAG = "MAIN";

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
    ESP_LOGI(TAG, "Pronto! Apri l'indirizzo mostrato sopra.");
}