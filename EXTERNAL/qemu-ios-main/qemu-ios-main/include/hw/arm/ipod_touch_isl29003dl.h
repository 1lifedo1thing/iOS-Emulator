#ifndef HW_ISL29003DL_H
#define HW_ISL29003DL_H

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "hw/core/sysbus.h"
#include "hw/i2c/i2c.h"
#include "hw/core/irq.h"

#define TYPE_ISL29003DL                 "isl29003dl"
OBJECT_DECLARE_SIMPLE_TYPE(ISL29003DLState, ISL29003DL)

#define LIGHTSENSOR_COMMAND    0x00
#define LIGHTSENSOR_CONTROL    0x01
#define LIGHTSENSOR_IT_HI      0x02
#define LIGHTSENSOR_IT_LO      0x03
#define LIGHTSENSOR_LSB_SENSOR 0x04
#define LIGHTSENSOR_MSB_SENSOR 0x05
#define LIGHTSENSOR_LSB_TIMER  0x06
#define LIGHTSENSOR_MSB_TIMER  0x07
#define LIGHTSENSOR_NUM_REGS   0x08

/* COMMAND */
#define LIGHTSENSOR_CMD_ENABLE      0x80
#define LIGHTSENSOR_CMD_ADC_PD      0x40   /* ADC power-down */
#define LIGHTSENSOR_CMD_MODE_SHIFT  2      /* 0 diode 1, 1 diode 2 (infrared), 2 diode 1 - diode 2 */
#define LIGHTSENSOR_CMD_WIDTH_MASK  0x03   /* 16, 12, 8 or 4 bits */

/* CONTROL */
#define LIGHTSENSOR_CTRL_INT_FLAG    0x20
#define LIGHTSENSOR_CTRL_RANGE_SHIFT 2      /* full scale 1000, 4000, 16000, 64000 lux at 100 kOhm */
#define LIGHTSENSOR_CTRL_PERSIST     0x03   /* 1, 4, 8 or 16 conversions out of the window */

typedef struct ISL29003DLState {
    I2CSlave i2c;
    qemu_irq irq;
    QEMUTimer *timer;
    uint8_t reg;          /* register pointer */
    bool reg_written;     /* this write transfer's register byte has arrived */
    uint8_t regs[LIGHTSENSOR_NUM_REGS];
    uint8_t out_count;    /* consecutive conversions outside the threshold window */
    uint32_t lux;         /* ambient light, settable at runtime (qom-set) */
    uint32_t rext;        /* external scaling resistor, ohms */
} ISL29003DLState;

#endif
