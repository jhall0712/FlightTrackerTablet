/*****************************************************************************
 * Waveshare ESP32-S3-Touch-LCD-7B I2C helper, from Waveshare's public examples.
 ******************************************************************************/
#ifndef __I2C_H
#define __I2C_H

#include <stdio.h>
#include <string.h>
#include "driver/i2c_master.h"
#include "esp_log.h"

#define EXAMPLE_I2C_MASTER_SDA GPIO_NUM_8
#define EXAMPLE_I2C_MASTER_SCL GPIO_NUM_9
#define EXAMPLE_I2C_MASTER_FREQUENCY (400 * 1000)
#define EXAMPLE_I2C_MASTER_NUM I2C_NUM_0

typedef struct {
    i2c_master_bus_handle_t bus;
    i2c_master_dev_handle_t dev;
} DEV_I2C_Port;

DEV_I2C_Port DEV_I2C_Init();
void DEV_I2C_Set_Slave_Addr(i2c_master_dev_handle_t *dev_handle, uint8_t Addr);
void DEV_I2C_Write_Byte(i2c_master_dev_handle_t dev_handle, uint8_t Cmd, uint8_t value);
uint8_t DEV_I2C_Read_Byte(i2c_master_dev_handle_t dev_handle);
uint16_t DEV_I2C_Read_Word(i2c_master_dev_handle_t dev_handle, uint8_t Cmd);
void DEV_I2C_Write_Nbyte(i2c_master_dev_handle_t dev_handle, uint8_t *pdata, uint8_t len);
void DEV_I2C_Read_Nbyte(i2c_master_dev_handle_t dev_handle, uint8_t Cmd, uint8_t *pdata, uint8_t len);

#endif
