/* Esptouch example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "tcpip_adapter.h"
#include "esp_smartconfig.h"
#include "smartconfig_ack.h"
#include "esp_netif.h"

#include "driver/gpio.h"
#include "driver/uart.h"
#include "nvs_flash.h"

#include "tunnel_smartconfig.h"

extern const char* TAG;
#define DELAY_MS(x)         (vTaskDelay(x / portTICK_RATE_MS))
char g_ssid[33] = { 0 };
char g_password[65] = { 0 };  
char g_destip[33] = {0};
uint16_t g_destport = 0;
int g_wifi_connect_ok = 0;

//////////////////////////////////////////////////// nvs storage ////////////////////////////////////////
static int nvs_read_test(void)
{ 
    size_t len = 32;
    nvs_handle wifi_handle;
    int err = 0;

    if(ESP_OK != nvs_open("wifi", NVS_READONLY, &wifi_handle))
        return -1;

    if(ESP_OK != nvs_get_str(wifi_handle, "ssid", g_ssid, &len))
        err = -1;
    else
        ESP_LOGI(TAG, "nvs got WiFi ssid %s", g_ssid);

    len = 64;
    if(ESP_OK != nvs_get_str(wifi_handle, "pswd", g_password, &len))
        err = -1;
    else
        ESP_LOGI(TAG, "nvs got WiFi pwd %s", g_password);
    

    len = 32;
    if(ESP_OK == nvs_get_str(wifi_handle, "destip", g_destip, &len))
        ESP_LOGI(TAG, "nvs got WiFi server IP %s", g_destip);
    
    if(ESP_OK == nvs_get_u16(wifi_handle, "destport", &g_destport))
        ESP_LOGI(TAG, "nvs got WiFi server port %d", g_destport);

    nvs_close(wifi_handle);

    DELAY_MS(1000);

    return err;
}

static int nvs_write_test(char *ssid, char *password)
{
    nvs_handle wifi_handle;

    if(ESP_OK != nvs_open("wifi", NVS_READWRITE, &wifi_handle))
        goto ERR_STEP;
    
    if(ESP_OK != nvs_set_str(wifi_handle, "ssid", ssid))
        goto ERR_STEP;
    
    if(ESP_OK != nvs_set_str(wifi_handle, "pswd", password))
        goto ERR_STEP;
    
    if(ESP_OK != nvs_commit(wifi_handle))
        goto ERR_STEP;
    
    DELAY_MS(1000); 

    ESP_LOGI(TAG, "nvs_write_test OK!");
    return 0;
    
ERR_STEP:
    ESP_LOGE(TAG, "nvs_write_test failed");
    nvs_close(wifi_handle);
    return -1;
}

/////////////////////////////////////////////////////smart config ///////////////////////////////////////////

/* The examples use smartconfig type that you can set via project configuration menu.

   If you'd rather not, just change the below entries to enum with
   the config you want - ie #define EXAMPLE_ESP_SMARTCOFNIG_TYPE SC_TYPE_ESPTOUCH
*/
#define EXAMPLE_ESP_SMARTCOFNIG_TYPE      CONFIG_ESP_SMARTCONFIG_TYPE

/* FreeRTOS event group to signal when we are connected & ready to make a request */
static EventGroupHandle_t s_wifi_event_group;

/* The event group allows multiple bits for each event,
   but we only care about one event - are we connected
   to the AP with an IP? */
static const int CONNECTED_BIT = BIT0;
static const int ESPTOUCH_DONE_BIT = BIT1;

static void smartconfig_example_task(void* parm);

static void event_handler(void* arg, esp_event_base_t event_base,
                          int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        xTaskCreate(smartconfig_example_task, "smartconfig_example_task", 4096, NULL, 3, NULL);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_wifi_connect();
        xEventGroupClearBits(s_wifi_event_group, CONNECTED_BIT);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_wifi_event_group, CONNECTED_BIT);
    } else if (event_base == SC_EVENT && event_id == SC_EVENT_SCAN_DONE) {
        ESP_LOGI(TAG, "Scan done");
    } else if (event_base == SC_EVENT && event_id == SC_EVENT_FOUND_CHANNEL) {
        ESP_LOGI(TAG, "Found channel");
    } else if (event_base == SC_EVENT && event_id == SC_EVENT_GOT_SSID_PSWD) {
        ESP_LOGI(TAG, "Got SSID and password");

        smartconfig_event_got_ssid_pswd_t* evt = (smartconfig_event_got_ssid_pswd_t*)event_data;
        wifi_config_t wifi_config;
        char ssid[33] = { 0 };
        char password[65] = { 0 };
        uint8_t rvd_data[33] = { 0 };

        bzero(&wifi_config, sizeof(wifi_config_t));
        memcpy(wifi_config.sta.ssid, evt->ssid, sizeof(wifi_config.sta.ssid));
        memcpy(wifi_config.sta.password, evt->password, sizeof(wifi_config.sta.password));
        wifi_config.sta.bssid_set = evt->bssid_set;

        if (wifi_config.sta.bssid_set == true) {
            memcpy(wifi_config.sta.bssid, evt->bssid, sizeof(wifi_config.sta.bssid));
        }

        memcpy(ssid, evt->ssid, sizeof(evt->ssid));
        memcpy(password, evt->password, sizeof(evt->password));
        ESP_LOGI(TAG, "SSID:%s", ssid);
        ESP_LOGI(TAG, "PASSWORD:%s", password);
        nvs_write_test(ssid, password);
        if (evt->type == SC_TYPE_ESPTOUCH_V2) {
            ESP_ERROR_CHECK( esp_smartconfig_get_rvd_data(rvd_data, sizeof(rvd_data)) );
            ESP_LOGI(TAG, "RVD_DATA:%s", rvd_data);
        }

        ESP_ERROR_CHECK(esp_wifi_disconnect());
        ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
        ESP_ERROR_CHECK(esp_wifi_connect());
    } else if (event_base == SC_EVENT && event_id == SC_EVENT_SEND_ACK_DONE) {
        xEventGroupSetBits(s_wifi_event_group, ESPTOUCH_DONE_BIT);
    }
}

static void initialise_wifi(void)
{
 //   tcpip_adapter_init();
    s_wifi_event_group = xEventGroupCreate();

 //   ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();

    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(SC_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
}

static void smartconfig_example_task(void* parm)
{
    EventBits_t uxBits;
    ESP_ERROR_CHECK(esp_smartconfig_set_type(EXAMPLE_ESP_SMARTCOFNIG_TYPE));
    smartconfig_start_config_t cfg = SMARTCONFIG_START_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_smartconfig_start(&cfg));

    while (1) {
        uxBits = xEventGroupWaitBits(s_wifi_event_group, CONNECTED_BIT | ESPTOUCH_DONE_BIT, true, false, portMAX_DELAY);

        if (uxBits & CONNECTED_BIT) {
            ESP_LOGI(TAG, "WiFi Connected to ap");
        }

        if (uxBits & ESPTOUCH_DONE_BIT) {
            ESP_LOGI(TAG, "smartconfig over");
            esp_smartconfig_stop();
            g_wifi_connect_ok = 1;
            vTaskDelete(NULL);
        }
    }
}

static EventGroupHandle_t s_connect_event_group;
static ip4_addr_t s_ip_addr;

static void on_wifi_disconnect(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    system_event_sta_disconnected_t *event = (system_event_sta_disconnected_t *)event_data;

    ESP_LOGI(TAG, "Wi-Fi disconnected, trying to reconnect...");
    if (event->reason == WIFI_REASON_BASIC_RATE_NOT_SUPPORT) {
        /*Switch to 802.11 bgn mode */
        esp_wifi_set_protocol(ESP_IF_WIFI_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);
    }
    tunnel_set_flashing_delay(500);
    ESP_ERROR_CHECK(esp_wifi_connect());
}

static void on_got_ip(void *arg, esp_event_base_t event_base,
                      int32_t event_id, void *event_data)
{
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    memcpy(&s_ip_addr, &event->ip_info.ip, sizeof(s_ip_addr));
    xEventGroupSetBits(s_connect_event_group, BIT(0));
}

static void wifi_connect_test(char *ssid, char *password)
{
    if (s_connect_event_group != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    s_connect_event_group = xEventGroupCreate();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &on_wifi_disconnect, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &on_got_ip, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    wifi_config_t wifi_config = { 0 };

    strncpy((char *)&wifi_config.sta.ssid, ssid, 32);
    strncpy((char *)&wifi_config.sta.password, password, 64);

    ESP_LOGI(TAG, "Connecting to %s...", wifi_config.sta.ssid);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_connect());  

    xEventGroupWaitBits(s_connect_event_group, BIT(0), true, true, portMAX_DELAY);
    ESP_LOGI(TAG, "Connected to %s", ssid);
    ESP_LOGI(TAG, "IPv4 address: " IPSTR, IP2STR(&s_ip_addr));

    g_wifi_connect_ok = 1;
}

int smartconfig_get_wifi_connect_state(void)
{
    return g_wifi_connect_ok;
}

int smartconfig_check_nvs(void)
{
    return(nvs_read_test());
}

void smartconfig_normal_wifi_connect(void)
{
    ESP_LOGI(TAG, "coming to normal_wifi_connect");
    wifi_connect_test(g_ssid, g_password);
}

void smartconfig_mode_start(void)
{
    ESP_LOGI(TAG, "coming to smartconfig");
    initialise_wifi();
}

int smartconfig_nvs_get_serverip(char *ip, int len)
{
    int glen = strlen(g_destip);

    len > glen ? len = strlen(g_destip) : 0;

    strncpy(ip, g_destip, len);

    return len;
}

uint16_t smartconfig_nvs_get_serverport(void)
{
    return g_destport;
}


int smartconfig_nvs_set_serverinfo(char *ip, uint16_t port)
{
    nvs_handle wifi_handle;

    if(ESP_OK != nvs_open("wifi", NVS_READWRITE, &wifi_handle))
        goto ERR_STEP;

    if(ip != NULL){
        if(ESP_OK != nvs_set_str(wifi_handle, "destip", ip))
            goto ERR_STEP;
        strcpy(g_destip, ip);
        ESP_LOGI(TAG, "nvs_write_server_info ip %s!", ip);
    }   
    if(port != 0){
        if(ESP_OK != nvs_set_u16(wifi_handle, "destport", port))
            goto ERR_STEP;   
        g_destport=port;
        ESP_LOGI(TAG, "nvs_write_server_info port %d", port);
    }
    
    return 0;

    ERR_STEP:
        ESP_LOGE(TAG, "nvs_write_server_info failed");
        nvs_close(wifi_handle);
        return -1;
}