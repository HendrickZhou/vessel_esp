/*
 *
 *
 * This is the implementation of the audio message for BLE
 *
 * @ Author: Hang Zhou
 *
 */

#ifndef AUDIO_CORE_H
#define AUDIO_CORE_H

#include<stdint.h>
#include<stdio.h>
#include<string.h>
#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"


#define QOA_IMPLEMENTATION
#include "qoa.c"

#define TAG_CORE    "CORE_AUDIO"

/* qoa related */
#define SAMPLE_RATE 10000  // 10kHz sampling rate
#define QOA_SLICES 256
#define QOA_SLICE_SAMPLES 20
#define FRAME_SAMPLES (QOA_SLICES * QOA_SLICE_SAMPLES)
#define PCM_DATA_SIZE (FRAME_SAMPLES * sizeof(int16_t))

/* packet related */
#define BLE_MTU_SIZE 512
#define PACKET_HEADER_SIZE 4
#define PAYLOAD_SIZE (BLE_MTU_SIZE - PACKET_HEADER_SIZE)


typedef struct {
    uint16_t frame_id;      // Unique identifier for each QOA frame
    uint8_t segment_id;     // Segment index within the frame
    uint8_t total_segments; // Total number of segments for the frame
    uint8_t data[PAYLOAD_SIZE];  // Data payload
} ble_packet_t;


/*******************
 * declaration
 */
int encode_audio(const int16_t *pcm_data, ble_packet_t **packets_out, size_t* num_packets);
static int encode_qoa_frame(const int16_t *pcm_data, unsigned int *out_len, uint8_t* encoded_data);
static size_t wrap_qoa_frame(const uint8_t *frame_data, size_t frame_len, uint16_t frame_id, ble_packet_t *packets_out);

int encode_audio(const int16_t *pcm_data, ble_packet_t **packets_out, size_t* num_packets) {
/**
 * MEMORY USAGE:
 * 2k stack for unwrapped frame
 * 2k+ heap for packet
 *
 *  API to encode audio and generate BLE-ready packets
 */
    if (!pcm_data || !packets_out || !num_packets) {
        ESP_LOGE(TAG_CORE, "Error: Invalid input parameters.");
        return -1;
    }

    // Encode PCM data into a QOA frame
    unsigned int frame_len;
    uint8_t encoded_frame[QOA_FRAME_SIZE(1,QOA_SLICES)];
    if(encode_qoa_frame(pcm_data, &frame_len, encoded_frame) < 0) {
        ESP_LOGE(TAG_CORE, "Error: Failed to encode QOA frame.");
        return -1;
    }

    // Calculate the required number of packets
    *num_packets = (frame_len + PAYLOAD_SIZE - 1) / PAYLOAD_SIZE;

#ifdef HEAP_MONITOR
    size_t free_heap = esp_get_free_heap_size();
    ESP_LOGI("HEAP", "stage 2 Free heap size: %u bytes", free_heap);
    size_t free_long = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    ESP_LOGI("HEAP", "largest free heap size: %u bytes", free_long);
    ESP_LOGI("HEAP", "need space: %d", (*num_packets) * sizeof(ble_packet_t));
#endif
    *packets_out = (ble_packet_t *)malloc(*num_packets * sizeof(ble_packet_t));
        if (!*packets_out) {
        ESP_LOGE(TAG_CORE, "Error: Memory allocation failed for BLE packets.");
        return -1;
    }

    // Wrap the encoded QOA frame in BLE packets
    // TODO frame id 
    size_t wrapped_packets = wrap_qoa_frame(encoded_frame, frame_len, 1 /* Frame ID */, *packets_out);

    // Check if wrapping was successful
    if (wrapped_packets != *num_packets) {
        ESP_LOGE(TAG_CORE, "Error: Packet wrapping mismatch.");
        free(*packets_out);
        *packets_out = NULL;
        *num_packets = 0;
        return -1;
    }

    return 0;  // Success
}

// Function to split QOA frame into BLE packets
static int encode_qoa_frame(const int16_t *pcm_data, unsigned int *out_len, uint8_t* encoded_data) {
/**
 * MEMORY USAGE: 
 * only overhead for some function calls, no big data space needed
 */
    if (!pcm_data) {
        ESP_LOGE(TAG_CORE, "Error: PCM data input is NULL.");
        return -1;
    }

    qoa_desc qoa_config = {
        .channels = 1,          // Mono
        .samplerate = SAMPLE_RATE,
        .samples = FRAME_SAMPLES
    };

    *out_len = qoa_encode_frame((const short *)pcm_data, &qoa_config, FRAME_SAMPLES, (unsigned char *)encoded_data);

    if (*out_len == 0) {
        ESP_LOGE(TAG_CORE, "Error: QOA encoding failed.");
        return -1;
    }

    return 1;
}

static size_t wrap_qoa_frame(const uint8_t *frame_data, size_t frame_len, uint16_t frame_id, ble_packet_t *packets_out) {
/**
 * MEMORY USAGE:
 * only overheads, no data space needed
 */
    size_t total_segments = (frame_len + PAYLOAD_SIZE - 1) / PAYLOAD_SIZE;

    for (uint8_t segment_id = 0; segment_id < total_segments; segment_id++) {
        ble_packet_t *packet = &packets_out[segment_id];
        packet->frame_id = frame_id;
        packet->segment_id = segment_id;
        packet->total_segments = total_segments;

        size_t offset = segment_id * PAYLOAD_SIZE;
        size_t segment_len = (frame_len - offset) > PAYLOAD_SIZE ? PAYLOAD_SIZE : (frame_len - offset);

        memcpy(packet->data, frame_data + offset, segment_len);
    }

    return total_segments;
}

#endif /* AUDIO_CORE_H */
