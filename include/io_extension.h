/*****************************************************************************
 * Waveshare ESP32-S3-Touch-LCD-7B IO expander helper.
 ******************************************************************************/
#ifndef __IO_EXTENSION_H
#define __IO_EXTENSION_H

#include "i2c.h"

#define IO_EXTENSION_ADDR          0x24
#define IO_EXTENSION_Mode          0x02
#define IO_EXTENSION_IO_OUTPUT_ADDR 0x03
#define IO_EXTENSION_IO_INPUT_ADDR 0x04
#define IO_EXTENSION_PWM_ADDR      0x05
#define IO_EXTENSION_ADC_ADDR      0x06

#define IO_EXTENSION_IO_0          0x00
#define IO_EXTENSION_IO_1          0x01
#define IO_EXTENSION_IO_2          0x02
#define IO_EXTENSION_IO_3          0x03
#define IO_EXTENSION_IO_4          0x04
#define IO_EXTENSION_IO_5          0x05
#define IO_EXTENSION_IO_6          0x06
#define IO_EXTENSION_IO_7          0x07

typedef struct _io_extension_obj_t {
    i2c_master_dev_handle_t addr;
    uint8_t Last_io_value;
    uint8_t Last_od_value;
} io_extension_obj_t;

void IO_EXTENSION_Init();
void IO_EXTENSION_Output(uint8_t pin, uint8_t value);
uint8_t IO_EXTENSION_Input(uint8_t pin);
void IO_EXTENSION_Pwm_Output(uint8_t Value);
uint16_t IO_EXTENSION_Adc_Input();

#endif
