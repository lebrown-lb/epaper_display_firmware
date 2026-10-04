# epaper_display_firmware
***

Firmware for the [ESP32-S3-ePaper-13.3E6](https://docs.waveshare.com/ESP32-S3-ePaper-13.3E6). Application provides two functions, One to upload a dithered image over a TCP connection to a image buffer and two receive commands to display or draw to the that image buffer.

The tools to dither/upload and send commands can be found in the [epaper_display_tools](https://github.com/lebrown-lb/epaper_display_tools) repository.

## Structure Description

Code implements two tasks the main application and a TCP server task. Main application executes all commands and updates the display the TCP server task provides access to the image buffer only when image data is being uploaded. All commands received by the TCP task are passed to the main task and a response is allways sent back to the TCP task.

So the current code has three shared memory elements between tasks:

1. Image Buffer - `img_mptr`
2. Command Buffer - `cmd_buffer`
3. Response Buffer - `rsp_buffer`

Each shared buffer is protected by a mutex this may not be the best way to facilitate communication between tasks but it is simple.

## ESP-IDF Settings

code is currently using a custom partition table and must be configured for the ESP32-S3-WROOM-2-N32R16V 

**Serial Flasher Config**

Flash SPI speed -> 80MHz\
Flash size -> 32MB\

**Component Config**

ESP PSRAM -> Support for external, SPI-connected RAM\
ESP PSRAM -> SPI RAM config -> Octal Mode PSRAM\
ESP PSRAM -> SPI RAM config -> set RAM clock speed -> 80MHz

**Partition Table**

Partition Table -> Custom partition table CSV