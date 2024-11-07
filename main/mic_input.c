/*
 *
 * Deal with the micphone input, also handle the mocking of mic input using pcm format audio file, meant to provide a standard interface
 * @Author: Hang Zhou
 */

#ifndef MIC_INPUT_H
#define MIC_INPUT_H

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_spiffs.h"
#include <fcntl.h>  // For open, O_RDONLY
#include <unistd.h> // For read, lseek, close

#include "freertos/ringbuf.h"
#include "freertos/timers.h"

#include "audio_core.c"

#define DEMO_FILENAME       "/spiffs/demo.wav"
#define TTAG                "THREAD_STUFF"
#define GTAG                "MIC_INPUT_STUFF"
#define CHUNK_SIZE          PCM_DATA_SIZE
#define PCM_RING_BUFFER_SIZE (PCM_DATA_SIZE * 3)
#define PACKET_RING_BUFFER_SIZE (sizeof(ble_packet_t) * 6) 

RingbufHandle_t ram_queue_in;
RingbufHandle_t ram_queue_out;
// TODO: buffer lives in data segment!!, will not race for stack space?
TaskHandle_t load_task;
TaskHandle_t encoder_task;
SemaphoreHandle_t condition_connected;

/**************
 * declaration
 ****************/
uint8_t* get_next_packet(size_t* size);
void setup_mic_input();
void clean_up();

static void init_spiffs();
static void init_ringbuffers();
static void encode_task(void *args);
static void load_uart_data();
static void load_audio_data(void *args); 
static void load_demo_audio_file();
static int read_wav_header(int fd, int *sample_rate, int *num_channels, int *bits_per_sample);

/********************
 * Encoding task
 ********************/
static void encode_task(void *args) {
/**
 * MEMORY USAGE:
 * just overhead + 2k stack from encode_audio
 */
    size_t item_size;
    uint8_t *item;

    while (1) {
        // if (xSemaphoreTake(conditionSemaphore, portMAX_DELAY) != pdTRUE) continue;
        item = (uint8_t *)xRingbufferReceive(ram_queue_in, &item_size, pdMS_TO_TICKS(20));
        if (item != NULL) {
            // ENCODE
            // ESP_LOGI("debug", "item get from queue 1 is not null");

            size_t num_packets;
            ble_packet_t* packets;
            if (encode_audio((int16_t *)item, &packets, &num_packets) == -1) {
                ESP_LOGE(TAG_CORE, "Error: Failed to encode audio data.");
                vRingbufferReturnItem(ram_queue_in, item);
                continue;
            }

            // ESP_LOGI("debug", "encoded!");

            // SEND
            for (size_t i = 0; i < num_packets; i++) {
                if (xRingbufferSend(ram_queue_out, &packets[i], sizeof(ble_packet_t), pdMS_TO_TICKS(100)) != pdTRUE) {
                    ESP_LOGE(TAG_CORE, "Error: Failed to enqueue packet %d in packet ring buffer.", i);
                    break;
                } 
                // ESP_LOGI(TAG_CORE, "sending encoded to queue2");
            }
            free(packets);
            vRingbufferReturnItem(ram_queue_in, item);
        }
    }
}  


#define TIMER_INTERVAL_MS    500  // 500 ms (0.5 second)
volatile bool timer_flag = false;
void timer_callback(TimerHandle_t xTimer) {
    timer_flag = true;
}
void setup_timer() {
    // Create a FreeRTOS software timer
    TimerHandle_t timer = xTimerCreate(
        "Timer",                          // Timer name
        pdMS_TO_TICKS(TIMER_INTERVAL_MS), // Timer period in ticks
        pdTRUE,                           // Auto-reload
        (void*)0,                         // Timer ID (not used here)
        timer_callback                    // Callback function
    );

    // Start the timer
    if (timer != NULL) {
        xTimerStart(timer, 0);
    }
}

static void load_demo_audio_file() {
    // Allocate chunk buffer on the heap
    // uint8_t *chunk = (uint8_t *)malloc(CHUNK_SIZE);
    // TODO maybe not using heap here? we got plenty of stack space
    // chunk_size is 10K for a frame
    // compressed is 2K for a frame
    // for a 8xxx task stack, it's 32k stack size which is huge

    
    do {
        if (xSemaphoreTake(condition_connected, portMAX_DELAY) != pdTRUE) continue;
        
        // Before calling fread
        // UBaseType_t stack_before = uxTaskGetStackHighWaterMark(NULL);
        

        uint8_t chunk[CHUNK_SIZE];
        

        //UBaseType_t stack_alloc = uxTaskGetStackHighWaterMark(NULL);
        // ESP_LOGI("STACK", "Stack used by allocating chunk: %u words", stack_before - stack_alloc);

        size_t bytes_read;
        int sample_rate, num_channels, bits_per_sample;

        ESP_LOGI(TTAG, "Opening WAV file");

        //FILE* file = fopen(DEMO_FILENAME, "rb");
        int file = open(DEMO_FILENAME, O_RDONLY);
        if (/*!file*/ file<0) {
            ESP_LOGE(TTAG, "Failed to open audio file");
            return;
        }

        if (read_wav_header(file, &sample_rate, &num_channels, &bits_per_sample) != 0) {
            //fclose(file);
            close(file);
            ESP_LOGE(TTAG, "Invalid WAV header");
            return;
        }

        ESP_LOGI(TTAG, "WAV file opened. Sample rate: %d, Channels: %d, Bits per sample: %d", sample_rate, num_channels, bits_per_sample);

        setup_timer();
        while (1) {
            // fseek(file, 44, SEEK_SET); // Assuming 44 bytes of WAV header
            lseek(file, 44, SEEK_SET);

            while ((bytes_read = /*fread(chunk, 1, CHUNK_SIZE, file))*/read(file, chunk, CHUNK_SIZE)) > 0) {
                //UBaseType_t stack_after_fread = uxTaskGetStackHighWaterMark(NULL);
                //ESP_LOGI("STACK", "Stack used by fread: %u words", stack_before - stack_after_fread);
                while (1) {
                    if (timer_flag) { // TODO use semaphore instead of this
                        timer_flag = false;  // Reset flag
                        // if(xRingbufferSend(ram_queue_in, chunk, bytes_read, pdMS_TO_TICKS(20)) != pdTRUE) {
                        //     ESP_LOGE(TTAG, "Queue 1 is full or timed out");
                        //     continue;
                        // }
                        // TODO resend mechansim
                        while(xRingbufferSend(ram_queue_in, chunk, bytes_read, pdMS_TO_TICKS(20)) != pdTRUE) {
                            ESP_LOGE(TTAG, "Queue 1 is full or timed out, resending");
                        }
                        ESP_LOGE(TTAG, "send it to queue 1");
#ifdef DEBUG_Q
                        ESP_LOGE(TTAG, "send it to queue 1");
#endif
                        vTaskDelay(pdMS_TO_TICKS(10)); // Short delay to avoid busy-waiting
                        break;
                    }
                    vTaskDelay(pdMS_TO_TICKS(10));
                }
                // UBaseType_t stack_after_send = uxTaskGetStackHighWaterMark(NULL);
                //ESP_LOGI("STACK", "Stack used by xRingbufferSend: %u words", stack_after_fread - stack_after_send);
            }
            ESP_LOGI(TTAG, "Finished transmitting audio file once");
        }

        // Clean up
        close(file);
    } while(0);
}

static void load_uart_data() {
    // keeps retriving data from uart buffer, and throw it in queue 1
}

static void load_audio_data(void *args) {
    // input: some signal? to indicate when to load uart/demo data
    // public access point
    // run in background
    // move data from file/uart to queue1
    // if(flag) load_uart_data()
    // else load_demo_data()
}

/**************
 * file related
 **************/
static void init_spiffs() {
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = NULL,
        .max_files = 3,
        .format_if_mount_failed = true
    };

    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(GTAG, "Failed to initialize SPIFFS (%s)", esp_err_to_name(ret));
        return;
    }

    size_t total = 0, used = 0;
    ret = esp_spiffs_info(NULL, &total, &used);
    if (ret == ESP_OK) {
        ESP_LOGI(GTAG, "SPIFFS total: %d, used: %d", total, used);
    } else {
        ESP_LOGE(GTAG, "Failed to get SPIFFS partition information (%s)", esp_err_to_name(ret));
    }
}

/*
static int read_wav_header(FILE *file, int *sample_rate, int *num_channels, int *bits_per_sample) {
    uint8_t header[44];

    // Read the WAV header
    if (fread(header, sizeof(header), 1, file) != 1) {
        ESP_LOGE("WAV", "Failed to read WAV header");
        return -1;
    }

    // Parse sample rate, channels, and bit depth from header
    *sample_rate = *(int *)(header + 24);        // Sample rate at byte 24
    *num_channels = *(short *)(header + 22);     // Number of channels at byte 22
    *bits_per_sample = *(short *)(header + 34);  // Bits per sample at byte 34

    // Confirm it's 16-bit mono audio for QOA compatibility
    if (*bits_per_sample != 16 || *num_channels != 1) {
        ESP_LOGE("WAV", "Incompatible WAV format: Requires 16-bit mono PCM");
        return -1;
    }

    return 0;  // Success
}
*/
static int read_wav_header(int fd, int *sample_rate, int *num_channels, int *bits_per_sample) {
    uint8_t header[44];

    // Read the WAV header
    if (read(fd, header, sizeof(header)) != sizeof(header)) {
        ESP_LOGE("WAV", "Failed to read WAV header");
        return -1;
    }

    // Parse sample rate, channels, and bit depth from header
    *sample_rate = *(int *)(header + 24);        // Sample rate at byte 24
    *num_channels = *(short *)(header + 22);     // Number of channels at byte 22
    *bits_per_sample = *(short *)(header + 34);  // Bits per sample at byte 34

    // Confirm it's 16-bit mono audio for QOA compatibility
    if (*bits_per_sample != 16 || *num_channels != 1) {
        ESP_LOGE("WAV", "Incompatible WAV format: Requires 16-bit mono PCM");
        return -1;
    }

    return 0;  // Success
}

/***************
 * Queue setup
 **************/
static void init_ringbuffers() {
    // Create a ring buffer for UART data (e.g., raw audio data)
    ram_queue_in = xRingbufferCreate(PCM_RING_BUFFER_SIZE, RINGBUF_TYPE_NOSPLIT);
    if (ram_queue_in == NULL) {
        ESP_LOGE(TTAG, "Error: Ring buffer 1 creation failed.");
    }
    // Create a ring buffer for BLE data (e.g., encoded audio data)
    ram_queue_out = xRingbufferCreate(PACKET_RING_BUFFER_SIZE, RINGBUF_TYPE_NOSPLIT);
    if (ram_queue_out == NULL) {
        ESP_LOGE(TTAG, "Error: Ring buffer 2 creation failed.");
    }
    ESP_LOGI(TTAG," finish ringbuffer !");
}

/***********************
 * interface
 ***********************/

void setup_mic_input() {
    init_spiffs();
    init_ringbuffers();

        // TODO ret
    // TODO args

    void * loader_args = NULL;
    if(xTaskCreate(load_demo_audio_file, "load_audio_data", CHUNK_SIZE+4096, NULL, 10, &load_task) != pdPASS) {
        ESP_LOGE(TTAG, "fail to set load file task");
        return;
    } else{
        ESP_LOGI(TTAG,"1 settup!");
    }
    void * encoder_args = NULL;
    if(xTaskCreate(encode_task, "encode_task",4096+2048, NULL, 10, &encoder_task)!=pdPASS) {
        ESP_LOGE(TTAG, "fail to set encode task");
    } else {
        ESP_LOGI(TTAG," all mic settup!");
    }
}

/*int8_t get_next_packet(uint8_t* item, size_t* size) { 
        uint8_t *packet = (uint8_t *)xRingbufferReceive(ram_queue_out, size, pdMS_TO_TICKS(100));
    if (packet != NULL) {
        memcpy(item, packet, *size);
        vRingbufferReturnItem(ram_queue_out, packet);
        return 1;
    } else {
        return -1;
    }
}*/
uint8_t* get_next_packet(size_t* size) { 
/*
 * MEMORY USAGE:
 * just system overhead, since all data processed are living on the heap
 * estimated size is around 2kB
 *
 * user is responsible for free the returned item
 * if next packet not available, return NULL
 *
 * use heap because conceptually, user don't konw the size of next packet, and it can't be calculated
 *
**/
#ifdef DEBUG_Q
    ESP_LOGI("debug", "getting packet from queue2!");
#endif
#ifdef STACK_MONITOR
    UBaseType_t stack = uxTaskGetStackHighWaterMark(NULL);
    ESP_LOGI("stack", "stack in get_next_packet: %u", stack);
#endif
    uint8_t *packet = (uint8_t *)xRingbufferReceive(ram_queue_out, size, pdMS_TO_TICKS(100));
    if (packet != NULL) {
#ifdef HEAP_MONITOR
        size_t free_heap = esp_get_free_heap_size();
        ESP_LOGI("HEAP", "Free heap size: %u bytes", free_heap);
#endif
        // Allocate memory on the heap for the packet copy
        uint8_t *heap_packet = (uint8_t *)malloc(*size);
        if (heap_packet != NULL) {
            memcpy(heap_packet, packet, *size);
        }
#ifdef STACK_MONITOR
        stack = uxTaskGetStackHighWaterMark(NULL);
        ESP_LOGI("stack", "stack in get_next_packet 2: %u", stack);
#endif
        vRingbufferReturnItem(ram_queue_out, packet);
        return heap_packet;
    } else {
        return NULL;
    }
}

void clean_up() {
    vRingbufferDelete(ram_queue_in);
    vRingbufferDelete(ram_queue_out);
    vTaskDelete(load_task);
    vTaskDelete(encoder_task);
}

#endif
