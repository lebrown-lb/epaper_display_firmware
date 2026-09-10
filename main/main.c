#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "GUI_Paint.h"
#include "GUI_BMPfile.h"
#include "epaper_port.h"
#include "gowdata.h"
#include "sdcard_bsp.h"
#include <string.h>

#include "lwip/sockets.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"


#define WIFI_SSID      "SSID"
#define WIFI_PASS      "PASSWORD"
#define MAXIMUM_RETRY  5
#define CMD_BUFFER_LEN 75
#define RSP_BUFFER_LEN 75


static uint8_t *img_mptr = NULL;
static SemaphoreHandle_t img_mptr_mutex = NULL;
char cmd_buffer[CMD_BUFFER_LEN];
static SemaphoreHandle_t cmd_buffer_mutex = NULL;
SemaphoreHandle_t cmd_semaphore = NULL;
char rsp_buffer[RSP_BUFFER_LEN];
static SemaphoreHandle_t rsp_buffer_mutex = NULL;
SemaphoreHandle_t rsp_semaphore = NULL;

static const char *TAG = "main";
size_t buffer_size =  EPD_WIDTH * EPD_HEIGHT / 2;
static int s_retry_num = 0;
volatile uint8_t ip_flg = 0;


void wifi_init_sta(void); 
static void tcp_server_task(void *pvParameters);
static void tcp_response(const char * str);
static void cmd_handler(char * buffer);
static void text_cmd_parse_exec(char * args);
static void rect_cmd_parse_exec(char * args);
static void circle_cmd_parse_exec(char * args);
static void line_cmd_parse_exec(char * args);
static DOT_PIXEL string_to_pixel_cnt(char * str);
static sFONT * string_to_font(char * str);
static UWORD string_to_color(char * str);
static int string_split(char * str,char **tokens, const char * delim, uint8_t ntokens);
static unsigned long string_to_uint(const char *nptr, char **endptr);
static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);
    
void app_main(void)
{
    UWORD timeout = 0;
    ESP_LOGI(TAG,"1.e-Paper Init and Clear...");
    EPD_Port_Init();
    EPD_Init();
    
    if((img_mptr = (uint8_t *)heap_caps_malloc(buffer_size,MALLOC_CAP_SPIRAM)) == NULL)
    {
        ESP_LOGE(TAG,"Failed to apply for black memory...");
        return;
    }

    img_mptr_mutex = xSemaphoreCreateMutex();
    if (img_mptr_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create Mutex!");
        return;
    }

    cmd_buffer_mutex = xSemaphoreCreateMutex();
    if (cmd_buffer_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create Mutex!");
        return;
    }

    cmd_semaphore = xSemaphoreCreateBinary();
    if (cmd_semaphore == NULL) {
        ESP_LOGE(TAG, "Failed to create Semaphore!");
        return;
    }
    
    rsp_buffer_mutex = xSemaphoreCreateMutex();
    if (rsp_buffer_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create Mutex!");
        return;
    }

    rsp_semaphore = xSemaphoreCreateBinary();
    if (rsp_semaphore == NULL) {
        ESP_LOGE(TAG, "Failed to create Semaphore!");
        return;
    }

    // Initialize NVS (Required for Wi-Fi storage)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "ESP_WIFI_MODE_STA");
    wifi_init_sta();

    while(!ip_flg)
    {
        vTaskDelay(pdMS_TO_TICKS(10));
        timeout++;
        if (timeout == 1000)
        {
            ESP_LOGI(TAG, "TIMOUT: FAILED TO AQUIRE IP");
            break;
        }
    }

    if(ip_flg)
    {
        xTaskCreatePinnedToCore(
        tcp_server_task,    // Task function
        "tcp_server",       // Task name
        4096,               // Stack size in bytes
        NULL,               // Parameters
        5,                  // Priority
        NULL,               // Task handle
        1                   // Core ID (Core 1)
        );
        ESP_LOGI(TAG, "TCP SERVER SPAWNED");
    }


    Paint_NewImage(img_mptr, EPD_WIDTH, EPD_HEIGHT, 90, EPD_WHITE);
    Paint_SetScale(6);
    Paint_SelectImage(img_mptr);
    while(1)
    {
        if (xSemaphoreTake(cmd_semaphore, pdMS_TO_TICKS(10)) == pdTRUE)
        {

            xSemaphoreTake(cmd_buffer_mutex, portMAX_DELAY);
            if(strcmp(cmd_buffer, "UPDATE") == 0)
            {
                tcp_response("EPD UPDATE!");
                xSemaphoreTake(img_mptr_mutex, portMAX_DELAY);
                EPD_Display(img_mptr);
                xSemaphoreGive(img_mptr_mutex);
            }
            else if(strcmp(cmd_buffer, "DEMO") == 0)
            {
                tcp_response("Epaper Demo Image");
                EPD_Display(gowImage);
            }
            else if(strcmp(cmd_buffer, "CLEAR") == 0)
            {
                tcp_response("Clear Epaper Display");
                EPD_Clear(EPD_WHITE);
            }
            else if(strcmp(cmd_buffer, "SLEEP") == 0)
            {
                tcp_response("close 5V, Module enters 0 power consumption ...");
                EPD_Sleep();
            }
            else
            {
                cmd_handler(cmd_buffer);
            }

            memset(cmd_buffer,'\0',CMD_BUFFER_LEN);
            xSemaphoreGive(cmd_buffer_mutex);
        }
    }
}




void wifi_init_sta(void) 
{
    // 1. Initialize the underlying network interface
    ESP_ERROR_CHECK(esp_netif_init());

    // 2. Create default event loop
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    // 3. Initialize Wi-Fi driver configuration
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // 4. Register event handlers
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    // 5. Configure the Wi-Fi connection parameters
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold.rssi = -127,
            .pmf_cfg = {
                .capable = true,
                .required = false
            },
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    
    // 6. Start the Wi-Fi driver
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "wifi_init_sta finished.");
}


static void tcp_server_task(void *pvParameters) 
{
    static const char * TCP_TAG = "tcp_server";
    char rx_buffer[1210];
    int addr_family = AF_INET;
    int ip_protocol = IPPROTO_IP;

    struct sockaddr_storage dest_addr;
    struct sockaddr_in *dest_addr_ip4 = (struct sockaddr_in *)&dest_addr;
    dest_addr_ip4->sin_addr.s_addr = htonl(INADDR_ANY);
    dest_addr_ip4->sin_family = AF_INET;
    dest_addr_ip4->sin_port = htons(3333); // Port number

    int listen_sock = socket(addr_family, SOCK_STREAM, ip_protocol);
    bind(listen_sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    listen(listen_sock, 1);

    while (1) {
        ESP_LOGI(TCP_TAG, "Socket listening...");
        struct sockaddr_storage source_addr;
        socklen_t addr_len = sizeof(source_addr);
        
        // This blocks the task, but NOT the whole system
        int sock = accept(listen_sock, (struct sockaddr *)&source_addr, &addr_len);
        if (sock >= 0) {
            ESP_LOGI(TCP_TAG, "Client connected!");
            
            // Simple Echo Server Logic
            while(1)
            {
                int len = recv(sock, rx_buffer, sizeof(rx_buffer), 0);
                if (len > 0) {
                    if ((rx_buffer[0] == 'C') && (rx_buffer[1] == 'M') && (rx_buffer[2] == 'D') && (rx_buffer[3] == ':'))
                    {
                        uint16_t cmd_len = ((uint16_t)rx_buffer[4] << 8) | rx_buffer[5];

                        if(cmd_len <= (len - 6))
                        {
                            xSemaphoreTake(cmd_buffer_mutex, portMAX_DELAY);
                            memcpy(cmd_buffer,rx_buffer + 6,cmd_len);
                            xSemaphoreGive(cmd_buffer_mutex);
                            xSemaphoreGive(cmd_semaphore);
                        }
                        if (xSemaphoreTake(rsp_semaphore, portMAX_DELAY) == pdTRUE)
                        {

                            xSemaphoreTake(rsp_buffer_mutex, portMAX_DELAY);
                            send(sock, rsp_buffer, strlen(rsp_buffer), 0);
                            memset(rsp_buffer,'\0',RSP_BUFFER_LEN);
                            xSemaphoreGive(rsp_buffer_mutex);
                        }
                    }
                    else if ((rx_buffer[0] == 'D') && (rx_buffer[1] == 'A') && (rx_buffer[2] == 'T') && (rx_buffer[3] == ':'))
                    {
                        uint16_t data_len = ((uint16_t)rx_buffer[4] << 8) | rx_buffer[5];
                        uint32_t data_ofst = ((uint32_t)rx_buffer[6] << 24) | ((uint32_t)rx_buffer[7] << 16) | ((uint32_t)rx_buffer[8] << 8) | rx_buffer[9];
                        if ((data_ofst >= buffer_size) || ((data_ofst + data_len) > buffer_size))
                        {
                            ESP_LOGI(TCP_TAG, "OFFSET ERROR DATA IGNORED!");
                            continue;
                        }

                        if(data_len > (len - 10))
                        {

                            int tmp = 0;
                            uint8_t timeout = 0;
                            while (((len - 10 ) < data_len) || (timeout == 6))
                            {
                                uint16_t cnt = data_len - (len -10);
                                if ((len + cnt) <= 1210)
                                {
                                    tmp = recv(sock, rx_buffer + len, cnt,0);
                                    if(tmp != cnt)
                                        ESP_LOGI(TCP_TAG, "READ MISSING DATA %d bytes of %d", tmp, cnt);
                                    len += tmp;
                                }
                                else
                                    break;
                                timeout++;
                            }
                        }

                        xSemaphoreTake(img_mptr_mutex, portMAX_DELAY);
                        memcpy(img_mptr + data_ofst, rx_buffer + 10, data_len);
                        xSemaphoreGive(img_mptr_mutex);

                    }
                    else if ((rx_buffer[0] == 'E') && (rx_buffer[1] == 'N') && (rx_buffer[2] == 'D') && (rx_buffer[3] == '!'))
                        break;
                }
                else
                    break;
            }
            shutdown(sock, 0);
            close(sock);
        }
    }
    vTaskDelete(NULL); // Clean up task if loop ever breaks
}


static void tcp_response(const char * str)
{
    xSemaphoreTake(rsp_buffer_mutex, portMAX_DELAY);
    strncpy(rsp_buffer,str,RSP_BUFFER_LEN-1);
    xSemaphoreGive(rsp_buffer_mutex);
    xSemaphoreGive(rsp_semaphore);

}


/* Helper Function Definitions -----------------------------------------------*/


static void cmd_handler(char * buffer)
{
    char * cmd = NULL;
    char * args = NULL;
    char *tokens[2] = {NULL,NULL};

    int tmp = string_split(buffer,tokens,":",2);
    if(tmp < 0)
        cmd = buffer;
    else
    {
        cmd = tokens[0];
        args = tokens[1];
    }

    if (strcmp(cmd,"clear") == 0)
    {
        if (args == NULL)
            tcp_response("USAGE clear:<COLOR>");
        else
        {
            tcp_response("ACK!");
            Paint_Clear(string_to_color(args));
        }
    }
    else if (strcmp(cmd,"text") == 0)
    {

        if (args == NULL)
            tcp_response("USAGE text:<x>,<y>,<str>,<font>,<bgcolor>,<color>");
        else
            text_cmd_parse_exec(args);
    }
    else if (strcmp(cmd,"rect") == 0)
    {

        if (args == NULL)
            tcp_response("USAGE rect:<x>,<y>,<l>,<w>,<color>,<pixel_width>,<fill>");
        else
            rect_cmd_parse_exec(args);
    }
    else if (strcmp(cmd,"circle") == 0)
    {

        if (args == NULL)
            tcp_response("USAGE circle:<x>,<y>,<r>,<color>,<pixel_width>,<fill>");
        else
            circle_cmd_parse_exec(args);
    }
    else if (strcmp(cmd,"line") == 0)
    {

        if (args == NULL)
            tcp_response("USAGE line:<x1>,<y1>,<x2>,<y2>,<color>,<pixel_width>,<style>");
        else
            line_cmd_parse_exec(args);
    }
    else
        tcp_response("INVALID COMMAND!");
}


static void text_cmd_parse_exec(char * args)
{
    char *tokens[6] = {NULL,NULL,NULL,NULL,NULL,NULL};

    int tmp = string_split(args,tokens,",",6);
    if (tmp < 6)
    {
        tcp_response("MISSING ARGUMENTS CHECK USAGE!");
        return;
    }

    char * endptr = NULL;
    uint16_t x = (uint16_t)string_to_uint(tokens[0],&endptr);
    if ((endptr == tokens[0]) || (x > 1600))
    {
        tcp_response("PROBLEM WITH <x> VALUE");
        return;
    }
    uint16_t y = (uint16_t)string_to_uint(tokens[1],&endptr);
    if ((endptr == tokens[1]) || (y > 1600))
    {
        tcp_response("PROBLEM WITH <y> VALUE");
        return;
    }

    tcp_response("ACK!");
    Paint_DrawString_EN(x, y, tokens[2], string_to_font(tokens[3]), string_to_color(tokens[4]), string_to_color(tokens[5]));


}

static void rect_cmd_parse_exec(char * args)
{
    char *tokens[7] = {NULL,NULL,NULL,NULL,NULL,NULL,NULL};

    int tmp = string_split(args,tokens,",",7);
    if (tmp < 7)
    {
        tcp_response("MISSING ARGUMENTS CHECK USAGE!");
        return;
    }

    char * endptr = NULL;
    uint16_t x = (uint16_t)string_to_uint(tokens[0],&endptr);
    if ((endptr == tokens[0]) || (x > 1600))
    {
        tcp_response("PROBLEM WITH <x> VALUE");
        return;
    }
    uint16_t y = (uint16_t)string_to_uint(tokens[1],&endptr);
    if ((endptr == tokens[1]) || (y > 1600))
    {
        tcp_response("PROBLEM WITH <y> VALUE");
        return;
    }
    uint16_t l = (uint16_t)string_to_uint(tokens[2],&endptr);
    if (endptr == tokens[2])
    {
        tcp_response("PROBLEM WITH <l> VALUE");
        return;
    }
    uint16_t w = (uint16_t)string_to_uint(tokens[3],&endptr);
    if (endptr == tokens[3])
    {
        tcp_response("PROBLEM WITH <w> VALUE");
        return;
    }

    tcp_response("ACK!");
    if (strcmp(tokens[6], "FILL") == 0)
        Paint_DrawRectangle(x, y, l, w, string_to_color(tokens[4]), string_to_pixel_cnt(tokens[5]), DRAW_FILL_FULL);
    else
        Paint_DrawRectangle(x, y, l, w, string_to_color(tokens[4]), string_to_pixel_cnt(tokens[5]), DRAW_FILL_EMPTY);

}

static void circle_cmd_parse_exec(char * args)
{
    char *tokens[6] = {NULL,NULL,NULL,NULL,NULL,NULL};

    int tmp = string_split(args,tokens,",",6);
    if (tmp < 6)
    {
        tcp_response("MISSING ARGUMENTS CHECK USAGE!");
        return;
    }

    char * endptr = NULL;
    uint16_t x = (uint16_t)string_to_uint(tokens[0],&endptr);
    if ((endptr == tokens[0]) || (x > 1600))
    {
        tcp_response("PROBLEM WITH <x> VALUE");
        return;
    }
    uint16_t y = (uint16_t)string_to_uint(tokens[1],&endptr);
    if ((endptr == tokens[1]) || (y > 1600))
    {
        tcp_response("PROBLEM WITH <y> VALUE");
        return;
    }
    uint16_t r = (uint16_t)string_to_uint(tokens[2],&endptr);
    if (endptr == tokens[2])
    {
        tcp_response("PROBLEM WITH <r> VALUE");
        return;
    }

    tcp_response("ACK!");
    if (strcmp(tokens[5], "FILL") == 0)
        Paint_DrawCircle(x, y, r, string_to_color(tokens[3]), string_to_pixel_cnt(tokens[4]), DRAW_FILL_FULL);
    else
        Paint_DrawCircle(x, y, r, string_to_color(tokens[3]), string_to_pixel_cnt(tokens[4]), DRAW_FILL_EMPTY);
}

static void line_cmd_parse_exec(char * args)
{
    char *tokens[7] = {NULL,NULL,NULL,NULL,NULL,NULL,NULL};

    int tmp = string_split(args,tokens,",",7);
    if (tmp < 7)
    {
        tcp_response("MISSING ARGUMENTS CHECK USAGE!");
        return;
    }

    char * endptr = NULL;
    uint16_t x1 = (uint16_t)string_to_uint(tokens[0],&endptr);
    if ((endptr == tokens[0]) || (x1 > 1600))
    {
        tcp_response("PROBLEM WITH <x1> VALUE");
        return;
    }
    uint16_t y1 = (uint16_t)string_to_uint(tokens[1],&endptr);
    if ((endptr == tokens[1]) || (y1 > 1600))
    {
        tcp_response("PROBLEM WITH <y1> VALUE");
        return;
    }
    uint16_t x2 = (uint16_t)string_to_uint(tokens[2],&endptr);
    if ((endptr == tokens[2]) || (x2 > 1600))
    {
        tcp_response("PROBLEM WITH <x2> VALUE");
        return;
    }
    uint16_t y2 = (uint16_t)string_to_uint(tokens[3],&endptr);
    if ((endptr == tokens[3]) || (y2 > 1600))
    {
        tcp_response("PROBLEM WITH <y2> VALUE");
        return;
    }

    tcp_response("ACK!");
    if(strcmp(tokens[6],"DOTS") == 0)
        Paint_DrawLine(x1, y1, x2, y2, string_to_color(tokens[4]), string_to_pixel_cnt(tokens[5]), LINE_STYLE_DOTTED);
    else
        Paint_DrawLine(x1, y1, x2, y2, string_to_color(tokens[4]), string_to_pixel_cnt(tokens[5]), LINE_STYLE_SOLID);
}

static DOT_PIXEL string_to_pixel_cnt(char * str)
{
    if(strcmp(str,"1x1") == 0)
        return DOT_PIXEL_1X1;
    else if(strcmp(str,"2x2") == 0)
        return DOT_PIXEL_2X2;
    else if(strcmp(str,"3x3") == 0)
        return DOT_PIXEL_3X3;
    else if(strcmp(str,"4x4") == 0)
        return DOT_PIXEL_4X4;
    else if(strcmp(str,"5x5") == 0)
        return DOT_PIXEL_5X5;
    else if(strcmp(str,"6x6") == 0)
        return DOT_PIXEL_6X6;
    else if(strcmp(str,"7x7") == 0)
        return DOT_PIXEL_7X7;
    else if(strcmp(str,"8x8") == 0)
        return DOT_PIXEL_8X8;
    else
        return DOT_PIXEL_1X1;
}

static sFONT * string_to_font(char * str)
{
    if(strcmp(str,"12") == 0)
        return &Font12;
    else if(strcmp(str,"16") == 0)
        return &Font16;
    else if(strcmp(str,"24") == 0)
        return &Font24;
    else if(strcmp(str,"28") == 0)
        return &Font28;
    else if(strcmp(str,"36") == 0)
        return &Font36;
    else if(strcmp(str,"48") == 0)
        return &Font48;
    else
        return &Font16;
}

static UWORD string_to_color(char * str)
{
    if(strcmp(str,"WHITE") == 0)
        return EPD_WHITE;
    else if(strcmp(str,"RED") == 0)
        return EPD_RED;
    else if(strcmp(str,"GREEN") == 0)
        return EPD_GREEN;
    else if(strcmp(str,"BLUE") == 0)
        return EPD_BLUE;
    else if(strcmp(str,"YELLOW") == 0)
        return EPD_YELLOW;
    else if(strcmp(str,"BLACK") == 0)
        return EPD_BLACK;
    else
        return EPD_BLACK;
}

static int string_split(char * str,char **tokens, const char * delim, uint8_t ntokens)
{
    char *token = NULL;
    int cnt = 0;

    token = strtok(str, delim);
    if (token == NULL)
        return -1;

    tokens[cnt] = token;
    cnt++;
    while ((token != NULL) && (cnt <= ntokens))
    {
        token = strtok(NULL, delim);
        if (token != NULL)
        {
            tokens[cnt] = token;
            cnt++;
        }
    }
    return cnt;
}

static unsigned long string_to_uint(const char *nptr, char **endptr)
{

    const char *s = nptr;
    unsigned long acc = 0;
    int c;
    int any = 0;
    int neg = 0;

    // 1. Skip leading whitespace
    do {
        c = (unsigned char)*s++;
    } while (isspace(c));

    // 2. Handle optional sign
    if (c == '-') {
        neg = 1;
        c = *s++;
    } else if (c == '+') {
        c = *s++;
    }

    // 3. Pre-calculated limits for base 10 overflow protection
    const unsigned long cutoff = ULONG_MAX / 10;
    const int cutlim = (int)(ULONG_MAX % 10);

    // 4. Convert digits
    while (isdigit(c)) {
        c -= '0';
        
        // Check for overflow before multiplying
        if (any < 0 || acc > cutoff || (acc == cutoff && c > cutlim)) {
            any = -1;
        } else {
            any = 1;
            acc = acc * 10 + c;
        }
        c = (unsigned char)*s++;
    }

    // 5. Finalize output and track end pointer
    if (any < 0) {
        acc = ULONG_MAX;
    } else if (neg) {
        acc = -acc; // Standard two's complement negation
    }

    if (endptr != 0) {
        // Step back by 1 because the while loop read one character past the last valid digit
        *endptr = (char *)(any ? s - 1 : nptr);
    }

    return acc;
}


static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) 
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "retry to connect to the AP");
        } else {
            ESP_LOGI(TAG, "connect to the AP fail");
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        ip_flg = 1;
        s_retry_num = 0;
    }
}
