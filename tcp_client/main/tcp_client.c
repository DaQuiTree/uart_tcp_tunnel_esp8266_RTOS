/* BSD Socket API Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#include <string.h>
#include <sys/param.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "protocol_examples_common.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <lwip/netdb.h>

#include "driver/uart.h"
#include "driver/gpio.h"

#include "tunnel_smartconfig.h"

////////////////////////////////////////////global//////////////////////////////////////
const char *TAG = "Tunnel";
static uint16_t flashingDelay = 2000;
volatile int8_t sockReady = -1, notReadyCode = -1;

#define BUF_SIZE (1024)
#define QUE_LEN (10)

QueueHandle_t xMsgQueue;
typedef struct{
    uint8_t data[BUF_SIZE];
    uint16_t len;
} MsgArray_t;

MsgArray_t array[QUE_LEN];

#define DELAY_MS(x)         (vTaskDelay(x / portTICK_RATE_MS))

//////////////////////////////////////////// GPIO ////////////////////////////////////////
#define GPIO_OUTPUT_IO_2    2
#define GPIO_OUTPUT_PIN_SEL  (1ULL<<GPIO_OUTPUT_IO_2)
#define GPIO_INPUT_IO_0     0
#define GPIO_INPUT_PIN_SEL  (1ULL<<GPIO_INPUT_IO_0) 
     
void init_input_gpio0(void)
{
    gpio_config_t io_conf;

    //interrupt of rising edge
    io_conf.intr_type =  GPIO_INTR_DISABLE;;
    //bit mask of the pins, use GPIO0 here
    io_conf.pin_bit_mask = GPIO_INPUT_PIN_SEL;
    //set as input mode
    io_conf.mode = GPIO_MODE_INPUT;

    io_conf.pull_up_en = 1;

    gpio_config(&io_conf);
}

static void gpio_flashing_task()
{
    gpio_config_t io_conf;
    //disable interrupt
    io_conf.intr_type = GPIO_INTR_DISABLE;
    //set as output mode
    io_conf.mode = GPIO_MODE_OUTPUT;
    //bit mask of the pins that you want to set,e.g.GPIO2
    io_conf.pin_bit_mask = GPIO_OUTPUT_PIN_SEL;
    //disable pull-down mode
    io_conf.pull_down_en = 0;
    //disable pull-up mode
    io_conf.pull_up_en = 0;
    //configure GPIO with the given settings
    gpio_config(&io_conf);
    int sta = 0;

    for(;;)
    {
        vTaskDelay(flashingDelay / portTICK_RATE_MS);
        gpio_set_level(GPIO_OUTPUT_IO_2, sta=(1-sta));
    }
}


////////////////////////////////////////////UART Task////////////////////////////////////
//SSSipSSS192.168.1.100SSS
typedef enum{
    CMD_IP,
    CMD_PORT,
    NO_CMD,
}cmd_e;

cmd_e check_local_cmd(char *data, int len)
{
    char *pS;

    if(!strncmp(data, "SipS", 4)){ //SipS192.168.66.6S
        pS = strchr(data+4, 'S');
        *pS = 0;
        strcpy(data, data+4);
        return CMD_IP;
    }else if(!strncmp(data, "SportS", 6)){//SportS3333S  
        pS = strchr(data+6, 'S');
        *pS = 0;
        strcpy(data, data+6);
        return CMD_PORT;
    }
    
    return NO_CMD;
}

static void echo_task()
{
    MsgArray_t *pArraySend;
    volatile int i = 0;

    // Configure parameters of an UART driver,
    // communication pins and install the driver
    uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE
    };
    uart_param_config(UART_NUM_0, &uart_config);
    uart_driver_install(UART_NUM_0, BUF_SIZE * 2, 0, 0, NULL, 0);
    
    while (1) {
        if(sockReady != -1){ //socket ok
            // Read data from the UART
            array[i].len = uart_read_bytes(UART_NUM_0, array[i].data, BUF_SIZE-1, 10 / portTICK_RATE_MS);
            // Write data back to the UART
            if(array[i].len > 0){
                pArraySend = &array[i];
                array[i].data[array[i].len] = 0;
                //ESP_LOGI(TAG, "MsgQueue %d index, str %s, len %d", i, array[i].data, array[i].len);
                xQueueSendToBack(xMsgQueue, &pArraySend, portMAX_DELAY);
                if(++i >= QUE_LEN)i = 0;
            }
        }else{ //socket not ok, local process uart
             uint8_t data[BUF_SIZE]={0};
             uint16_t len = uart_read_bytes(UART_NUM_0, data, BUF_SIZE-1, 10 / portTICK_RATE_MS);
             data[len] = 0;
             if(len == 0)
                continue;
             ESP_LOGI(TAG, "Uart local recieve str %s, len %d", data, len);
             switch(check_local_cmd((char *)data, len))
             {
                 case CMD_IP:
                    smartconfig_nvs_set_serverinfo((char *)data, 0);
                    break;
                case CMD_PORT:
                    smartconfig_nvs_set_serverinfo(NULL, (uint16_t)atoi((char *)data));
                    break;
                default:
                    break;
             }
        }
    }
}

/////////////////////////////////socket Task/////////////////////////////////////////
#define HOST_IP_ADDR CONFIG_EXAMPLE_IPV4_ADDR

#define PORT CONFIG_EXAMPLE_PORT

int sock;

static void socket_change_state(int8_t sockSta, uint16_t fdelay_ms, uint16_t pdelay_ms)
{
    sockReady = sockSta;
    if(fdelay_ms > 0)
        flashingDelay = fdelay_ms;
    if(pdelay_ms > 0)
        vTaskDelay(pdelay_ms / portTICK_PERIOD_MS);
}

static void tcp_client_send_task(void *pvParameters)
{
    MsgArray_t *pArrayRecv = NULL;
    char addr_str[128];
    int addr_family;
    int ip_protocol;

    char server_ip[32] = { 0 };
    uint16_t server_port = 0;

    while (1) {
        struct sockaddr_in destAddr;
        if(0 == smartconfig_nvs_get_serverip(server_ip, 32)) 
        {
            destAddr.sin_addr.s_addr = inet_addr(HOST_IP_ADDR);//server ip default
        }else{
            ESP_LOGE(TAG, "Unable to create socket: serverip %s", server_ip);
            destAddr.sin_addr.s_addr = inet_addr(server_ip); //server ip from nvs
        }
        destAddr.sin_family = AF_INET;
        if((server_port = smartconfig_nvs_get_serverport()) == 0){
            destAddr.sin_port = htons(PORT);
        }else{
            ESP_LOGE(TAG, "Unable to create socket: serverport %d", server_port);
            destAddr.sin_port = htons(server_port);
        }
        addr_family = AF_INET;
        ip_protocol = IPPROTO_IP;
        inet_ntoa_r(destAddr.sin_addr, addr_str, sizeof(addr_str) - 1);

        sock =  socket(addr_family, SOCK_STREAM, ip_protocol);
        if (sock < 0) {
            ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
            socket_change_state(notReadyCode, 500, 500);
            break;
        }
        ESP_LOGI(TAG, "Socket created %d", sock);

        int err = connect(sock, (struct sockaddr *)&destAddr, sizeof(destAddr));
        if (err != 0) {
            close(sock);
            ESP_LOGE(TAG, "Socket unable to connect: errno %d", errno);
            socket_change_state(notReadyCode, 500, 500);
            continue;
        }
        ESP_LOGI(TAG, "Successfully connected");
        socket_change_state(1, 2000, 0);
        notReadyCode = 0;

        while (1) {
            //blocking receiving
            xQueueReceive(xMsgQueue, &pArrayRecv, portMAX_DELAY);

            int err = send(sock, pArrayRecv->data, pArrayRecv->len, 0);
            if (err < 0) {
                ESP_LOGE(TAG, "Error occured during sending: errno %d", errno);
                break;
            }

        }

        if (sock != -1) {
            shutdown(sock, 0);
            close(sock);
            ESP_LOGE(TAG, "Shutting down socket and restarting...");
            socket_change_state(notReadyCode, 500, 100);
        }
    }
    vTaskDelete(NULL);
}

static void tcp_client_recv_task(void *pvParameters)
{
    char rx_buffer[128] = {0};

    while(1)
    {
        if(sockReady > 0){
            int len = recv(sock, rx_buffer, sizeof(rx_buffer) - 1, 0);
            // Error occured during receiving
            if (len < 0) {
                socket_change_state(notReadyCode, 2000, 0);
                ESP_LOGE(TAG, "recv failed: errno %d", errno);
            }else {
                //rx_buffer[len++] = 0x0D; // Null-terminate whatever we received and treat like a string
                rx_buffer[len++] = 0x0A;
                for(int i = 0; i < len; i++){
                    uart_write_bytes(UART_NUM_0, rx_buffer+i, 1);
                    vTaskDelay(100 / portTICK_PERIOD_MS);
                    //ESP_LOGI(TAG, "Received %c", *(rx_buffer+i));
                }
            }
        }
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }
}

static void tasks_manager_task()
{
    xTaskCreate(gpio_flashing_task, "gpio_flashing_task", 2048, NULL, 6, NULL);

    //not ready
    while(!smartconfig_get_wifi_connect_state())
        DELAY_MS(100);

    //real tasks
    xMsgQueue = xQueueCreate(QUE_LEN, sizeof(MsgArray_t *)); 
    if(xMsgQueue != NULL)
    {
        ESP_LOGI(TAG, "MsgQueue Created!");
        xTaskCreate(echo_task, "uart_echo_task", 4096, NULL, 10, NULL);
        xTaskCreate(tcp_client_send_task, "tcp_send", 4096, NULL, 8, NULL);
        xTaskCreate(tcp_client_recv_task, "tcp_recv", 4096, NULL, 5, NULL);
    }

    vTaskDelete(NULL);
}

////////////////////////////////main   loop ///////////////////////////////////////
void app_main()
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    //ESP_ERROR_CHECK(example_connect());
    //init smartconfig pin
    init_input_gpio0();
    DELAY_MS(1000);
    int gpio_level = gpio_get_level(GPIO_INPUT_IO_0);
    ESP_LOGI(TAG, "got gpio value %d", gpio_level);

    if(gpio_level == 1 && !smartconfig_check_nvs()){//normal connect
        socket_change_state(-1, 500, 0);
        smartconfig_normal_wifi_connect();
    }else{//smartconfig connect
        socket_change_state(-1, 100, 0);
        smartconfig_mode_start();
    }

    xTaskCreate(tasks_manager_task, "tasks_manager_task", 4096, NULL, 2, NULL);

}

////////////////////////////////////////// output //////////////////////////////////////// 
void tunnel_set_flashing_delay(int delay_ms)
{
    flashingDelay = delay_ms;
}