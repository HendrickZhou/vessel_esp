idf_component_register(SRCS "qoa.c" "audio_core.c" "mic_input.c" "vessel.c"
                    INCLUDE_DIRS "." 
                    )
spiffs_create_partition_image(storage ../spiffs FLASH_IN_PROJECT)
