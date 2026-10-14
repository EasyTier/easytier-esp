#include <iostream>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "esp_log.h"

extern "C" void app_main(void)
{
    while(true){
        ESP_LOGI("main", "Hello\n");
        ESP_LOGE("main", "World\n");
        std::cout << "Hello World" << std::endl;
        vTaskDelay(2000);
    }
}
