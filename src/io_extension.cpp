#include "io_extension.h"

io_extension_obj_t IO_EXTENSION;

void IO_EXTENSION_IO_Mode(uint8_t pin)
{
    uint8_t data[2] = {IO_EXTENSION_Mode, pin};
    DEV_I2C_Write_Nbyte(IO_EXTENSION.addr, data, 2);
}

void IO_EXTENSION_Init()
{
    DEV_I2C_Set_Slave_Addr(&IO_EXTENSION.addr, IO_EXTENSION_ADDR);
    IO_EXTENSION_IO_Mode(0xff);
    IO_EXTENSION.Last_io_value = 0xFF;
    IO_EXTENSION.Last_od_value = 0xFF;
}

void IO_EXTENSION_Output(uint8_t pin, uint8_t value)
{
    if (value == 1) {
        IO_EXTENSION.Last_io_value |= (1 << pin);
    } else {
        IO_EXTENSION.Last_io_value &= (~(1 << pin));
    }

    uint8_t data[2] = {IO_EXTENSION_IO_OUTPUT_ADDR, IO_EXTENSION.Last_io_value};
    DEV_I2C_Write_Nbyte(IO_EXTENSION.addr, data, 2);
}

uint8_t IO_EXTENSION_Input(uint8_t pin)
{
    uint8_t value = 0;
    DEV_I2C_Read_Nbyte(IO_EXTENSION.addr, IO_EXTENSION_IO_INPUT_ADDR, &value, 1);
    return ((value & (1 << pin)) > 0);
}

void IO_EXTENSION_Pwm_Output(uint8_t Value)
{
    if (Value >= 97) {
        Value = 97;
    }

    uint8_t data[2] = {IO_EXTENSION_PWM_ADDR, Value};
    data[1] = Value * (255 / 100.0);
    DEV_I2C_Write_Nbyte(IO_EXTENSION.addr, data, 2);
}

uint16_t IO_EXTENSION_Adc_Input()
{
    return DEV_I2C_Read_Word(IO_EXTENSION.addr, IO_EXTENSION_ADC_ADDR);
}
