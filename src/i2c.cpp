#include "i2c.h"

static const char *TAG = "i2c";
DEV_I2C_Port handle;

DEV_I2C_Port DEV_I2C_Init()
{
    if (handle.bus) {
        return handle;
    }

    i2c_master_bus_config_t i2c_bus_config = {
        .i2c_port = EXAMPLE_I2C_MASTER_NUM,
        .sda_io_num = EXAMPLE_I2C_MASTER_SDA,
        .scl_io_num = EXAMPLE_I2C_MASTER_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
    };

    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_config, &handle.bus));

    i2c_device_config_t i2c_dev_conf = {
        .scl_speed_hz = EXAMPLE_I2C_MASTER_FREQUENCY,
    };

    if (i2c_master_bus_add_device(handle.bus, &i2c_dev_conf, &handle.dev) != ESP_OK) {
        ESP_LOGE(TAG, "I2C device creation failed");
    }

    return handle;
}

void DEV_I2C_Set_Slave_Addr(i2c_master_dev_handle_t *dev_handle, uint8_t Addr)
{
    i2c_device_config_t i2c_dev_conf = {
        .device_address = Addr,
        .scl_speed_hz = EXAMPLE_I2C_MASTER_FREQUENCY,
    };

    if (i2c_master_bus_add_device(handle.bus, &i2c_dev_conf, dev_handle) != ESP_OK) {
        ESP_LOGE(TAG, "I2C address modification failed");
    }
}

void DEV_I2C_Write_Byte(i2c_master_dev_handle_t dev_handle, uint8_t Cmd, uint8_t value)
{
    uint8_t data[2] = {Cmd, value};
    ESP_ERROR_CHECK(i2c_master_transmit(dev_handle, data, sizeof(data), 100));
}

uint8_t DEV_I2C_Read_Byte(i2c_master_dev_handle_t dev_handle)
{
    uint8_t data[1] = {0};
    ESP_ERROR_CHECK(i2c_master_receive(dev_handle, data, 1, 100));
    return data[0];
}

uint16_t DEV_I2C_Read_Word(i2c_master_dev_handle_t dev_handle, uint8_t Cmd)
{
    uint8_t data[2] = {Cmd};
    ESP_ERROR_CHECK(i2c_master_transmit_receive(dev_handle, data, 1, data, 2, 100));
    return data[1] << 8 | data[0];
}

void DEV_I2C_Write_Nbyte(i2c_master_dev_handle_t dev_handle, uint8_t *pdata, uint8_t len)
{
    ESP_ERROR_CHECK(i2c_master_transmit(dev_handle, pdata, len, 100));
}

void DEV_I2C_Read_Nbyte(i2c_master_dev_handle_t dev_handle, uint8_t Cmd, uint8_t *pdata, uint8_t len)
{
    ESP_ERROR_CHECK(i2c_master_transmit_receive(dev_handle, &Cmd, 1, pdata, len, 100));
}
