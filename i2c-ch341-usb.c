/*
 * Driver for the CH341 USB to I2C and GPIO adapter
 *
 * Copyright (c) 2017 Gunar Schorcht (gunar@schorcht.net)
 *
 * Derived from
 *
 *  i2c-ch341-usb.c Copyright (c) 2016 Tse Lun Bien
 *  i2c-ch341.c     Copyright (c) 2014 Marco Gittler
 *  i2c-tiny-usb.c  Copyright (c) 2006-2007 Till Harbaum (Till@Harbaum.org)
 *
 * and extended by GPIO and interrupt handling capabilities.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation, version 2.
 */

// uncomment following line to activate kernel debug handling
// #define DEBUG
   #define DEBUG_PRINTK

#ifdef DEBUG_PRINTK
#define PRINTK(fmt,...) printk("%s: "fmt"\n", __func__, ##__VA_ARGS__)
#else
#define PRINTK(fmt,...)
#endif

#define CH341_IF_ADDR (&(ch341_dev->usb_if->dev))
#define DEV_ERR(d,f,...)  dev_err (d,"%s: "f"\n", __FUNCTION__, ##__VA_ARGS__)
#define DEV_DBG(d,f,...)  dev_dbg (d,"%s: "f"\n", __FUNCTION__, ##__VA_ARGS__)
#define DEV_INFO(d,f,...) dev_info(d,"%s: "f"\n", __FUNCTION__, ##__VA_ARGS__)

// check for condition and return with or without err code if it fails
#define CHECK_PARAM_RET(cond,err) if (!(cond)) return err;
#define CHECK_PARAM(cond)         if (!(cond)) return;

#include <linux/version.h>
#if LINUX_VERSION_CODE < KERNEL_VERSION(3,4,0)
#error The driver requires at least kernel version 3.4
#else

#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/types.h>
#include <linux/slab.h>
#include <linux/usb.h>
#include <linux/i2c.h>
#include <linux/gpio.h>
#include <linux/irq.h>
#include <linux/kthread.h>

/**
  * ATTENTION:
  *
  * CH341_POLL_PERIOD_MS in milliseconds defines the rate at which GPIOs are
  * read from CH341 via USB and thus the maximum rate at which changes on
  * interrupt driven input ports can be recognized at all.
  *
  * This value must be at least 10 ms, but should be 20 ms or more (if
  * possible) dependent on the performance of your system. Please check your
  * syslog for messages like "GPIO poll period is too short by at least %n
  * msecs". This message is thrown if the defined CH341_POLL_PERIOD_MS is
  * shorter than the time required for one reading of the GPIOs.
  */
#define CH341_POLL_PERIOD_MS        10    // see above

#define CH341_GPIO_NUM_PINS         8     // Number of GPIO pins, DO NOT CHANGE

#define CH341_USB_MAX_BULK_SIZE     32    // CH341A wMaxPacketSize for ep_02 and ep_82
#define CH341_USB_MAX_INTR_SIZE     8     // CH341A wMaxPacketSize for ep_81

#define CH341_I2C_LOW_SPEED         0     // low speed - 20kHz
#define CH341_I2C_STANDARD_SPEED    1     // standard speed - 100kHz
#define CH341_I2C_FAST_SPEED        2     // fast speed - 400kHz
#define CH341_I2C_HIGH_SPEED        3     // high speed - 750kHz

#define CH341_CMD_I2C_STREAM        0xAA  // I2C stream command
#define CH341_CMD_UIO_STREAM        0xAB  // UIO stream command

#define CH341_CMD_I2C_STM_STA       0x74  // I2C set start condition
#define CH341_CMD_I2C_STM_STO       0x75  // I2C set stop condition
#define CH341_CMD_I2C_STM_OUT       0x80  // I2C output data
#define CH341_CMD_I2C_STM_IN        0xC0  // I2C input data
#define CH341_CMD_I2C_STM_SET       0x60  // I2C set configuration
#define CH341_CMD_I2C_STM_END       0x00  // I2C end command

#define CH341_CMD_UIO_STM_IN        0x00  // UIO interface IN  command (D0~D7)
#define CH341_CMD_UIO_STM_OUT       0x80  // UIO interface OUT command (D0~D5)
#define CH341_CMD_UIO_STM_DIR       0x40  // UIO interface DIR command (D0~D5)
#define CH341_CMD_UIO_STM_END       0x20  // UIO interface END command
#define CH341_CMD_UIO_STM_US        0xc0  // UIO interface US  command

#define CH341_PIN_MODE_OUT          0
#define CH341_PIN_MODE_IN           1

#define CH341_OK                    0

/**
 *
 *  Change the default values in *ch341_board_config* for your configuraton
 *
 *  Configurable are:
 *
 *  - Pin 15 (D0/CS0  ) as input/output (CH341_PIN_MODE_IN / CH341_PIN_MODE_OUT)
 *  - Pin 16 (D1/CS1  ) as input/output (CH341_PIN_MODE_IN / CH341_PIN_MODE_OUT)
 *  - Pin 17 (D2/CS2  ) as input/output (CH341_PIN_MODE_IN / CH341_PIN_MODE_OUT)
 *  - Pin 18 (D3/CS2  ) as input/output (CH341_PIN_MODE_IN / CH341_PIN_MODE_OUT)
 *  - Pin 19 (D4/DOUT2) as input/output (CH341_PIN_MODE_IN / CH341_PIN_MODE_OUT)
 *  - Pin 20 (D5/DOUT ) as input/output (CH341_PIN_MODE_IN / CH341_PIN_MODE_OUT)
 *  - Pin 21 (D6/DIN2 ) as input        (CH341_PIN_MODE_IN)
 *  - Pin 22 (D7/DIN  ) as input        (CH341_PIN_MODE_IN)
 */

struct ch341_pin_config {
    uint8_t pin;    // pin number of CH341 chip
    uint8_t mode;   // GPIO mode
    char*   name;   // GPIO name
    bool    hwirq;  // connected to hardware interrupt (only one pin can have true)
};

struct ch341_pin_config ch341_board_config[CH341_GPIO_NUM_PINS] =
{
    // pin  GPIO mode           GPIO name   hwirq
    {   15, CH341_PIN_MODE_OUT , "gpio0"    , 0 }, // used as output
    {   16, CH341_PIN_MODE_OUT , "gpio1"    , 0 }, // used as output
    {   17, CH341_PIN_MODE_OUT , "gpio2"    , 0 }, // used as output
    {   18, CH341_PIN_MODE_OUT , "gpio3"    , 0 }, // used as output
    {   19, CH341_PIN_MODE_OUT , "gpio4"    , 1 }, // used as output with hardware IRQ
    {   20, CH341_PIN_MODE_OUT , "gpio5"    , 0 }, // used as ouput
    {   21, CH341_PIN_MODE_IN  , "gpio6"    , 0 }, // used as input
    {   22, CH341_PIN_MODE_IN  , "gpio7"    , 0 }  // used as input
};

// device specific structure
struct ch341_device
{
    // USB device description
    struct usb_device*    usb_dev;  // usb device
    struct usb_interface* usb_if;   // usb interface

    struct usb_endpoint_descriptor *ep_in;     // usb endpoint bulk in
    struct usb_endpoint_descriptor *ep_out;    // usb endpoint bulk out
    struct usb_endpoint_descriptor *ep_intr;   // usb endpoint interrupt in

    uint8_t in_buf  [CH341_USB_MAX_BULK_SIZE]; // usb input buffer
    uint8_t out_buf [CH341_USB_MAX_BULK_SIZE]; // usb outpu buffer
    uint8_t intr_buf[CH341_USB_MAX_INTR_SIZE]; // usb interrupt buffer

    struct urb* intr_urb;

    // I2C device description
    struct i2c_adapter i2c_dev;   // i2c related things

    // GPIO device description
    struct gpio_chip         gpio;                              // chip descriptor for GPIOs
    uint8_t                  gpio_num;                          // number of pins used as GPIOs
    uint8_t                  gpio_mask;                         // configuratoin mask defines IN/OUT pins
    uint8_t                  gpio_io_data;                      // current value of CH341 I/O register
    struct task_struct *     gpio_thread;                       // GPIO poll thread
    struct ch341_pin_config* gpio_pins   [CH341_GPIO_NUM_PINS]; // pin configurations (gpio_num elements)
    uint8_t                  gpio_bits   [CH341_GPIO_NUM_PINS]; // bit of I/O data byte (gpio_num elements)
    uint8_t                  gpio_values [CH341_GPIO_NUM_PINS]; // current values (gpio_num elements)
    char*                    gpio_names  [CH341_GPIO_NUM_PINS]; // pin names  (gpio_num elements)
    int                      gpio_irq_map[CH341_GPIO_NUM_PINS]; // GPIO to IRQ map (gpio_num elements)

    // IRQ device description
    struct irq_chip   irq;                                // chip descriptor for IRQs
    uint8_t           irq_num;                            // number of pins with IRQs
    int               irq_base;                           // base IRQ allocated
    struct irq_desc*  irq_descs    [CH341_GPIO_NUM_PINS]; // IRQ descriptors used (irq_num elements)
    int               irq_types    [CH341_GPIO_NUM_PINS]; // IRQ types (irq_num elements)
    bool              irq_enabled  [CH341_GPIO_NUM_PINS]; // IRQ enabled flag (irq_num elements)
    int               irq_gpio_map [CH341_GPIO_NUM_PINS]; // IRQ to GPIO pin map (irq_num elements)
    int               irq_hw;                             // IRQ for GPIO with hardware IRQ (default -1)
};

// ----- variables configurable during runtime ---------------------------

static uint speed      = CH341_I2C_STANDARD_SPEED;    // module parameter speed, default standard speed
static uint speed_last = CH341_I2C_FAST_SPEED + 1;    // last used speed, default invalid

static uint poll_period = CH341_POLL_PERIOD_MS;       // module parameter poll period

// ----- function prototypes ---------------------------------------------

static int ch341_usb_transfer (struct ch341_device *dev, int out_len, int in_len);

// ----- board configuration layer begin ---------------------------------

static int ch341_cfg_probe (struct ch341_device* ch341_dev)
{
    struct ch341_pin_config* cfg;
    int i;

    CHECK_PARAM_RET (ch341_dev, -EINVAL);

    ch341_dev->gpio_mask   = 0x3f; // default
    ch341_dev->gpio_num    = 0;
    ch341_dev->gpio_thread = 0;

    ch341_dev->irq_num     = 0;
    ch341_dev->irq_base    = 0;
    ch341_dev->irq_hw      = -1;

    for (i = 0; i < CH341_GPIO_NUM_PINS; i++)
    {
        cfg = ch341_board_config + i;

        if (cfg->pin == 0)
            continue;

        // --- check correct pin configuration ------------

        // is pin configurable at all
        if (cfg->pin < 15 || cfg->pin > 22)
        {
            DEV_ERR(CH341_IF_ADDR, "pin %d: is not configurable", cfg->pin);
            return -EINVAL;
        }

        // is pin configured correctly as input in case of pin 21 and 22
        if ((cfg->pin == 21 || cfg->pin == 22) && cfg->mode != CH341_PIN_MODE_IN)
        {
            DEV_ERR(CH341_IF_ADDR, "pin %d: must be an input", cfg->pin);
            return -EINVAL;
        }

        // --- read in pin configuration

        // if pin is not configured as CS signal, set GPIO configuration
        ch341_dev->gpio_names  [ch341_dev->gpio_num] = cfg->name;
        ch341_dev->gpio_pins   [ch341_dev->gpio_num] = cfg;
        ch341_dev->gpio_irq_map[ch341_dev->gpio_num] = -1; // no valid IRQ

        // map CH341 pin to bit D0...D7 in the CH341 I/O data byte
        ch341_dev->gpio_bits[ch341_dev->gpio_num] = (1 << (cfg->pin - 15));

        // GPIO pins can generate IRQs when set to input mode
        ch341_dev->gpio_irq_map[ch341_dev->gpio_num] = ch341_dev->irq_num;
        ch341_dev->irq_gpio_map[ch341_dev->irq_num]  = ch341_dev->gpio_num;

        if (cfg->hwirq)
        {
            if (ch341_dev->irq_hw != -1)
            {
                DEV_ERR(CH341_IF_ADDR,
                        "pin %d: only one GPIO can be connected to the hardware IRQ",
                        cfg->pin);
                return -EINVAL;
            }

            ch341_dev->irq_hw = ch341_dev->irq_num;
        }

        if (cfg->mode == CH341_PIN_MODE_IN)
            // if pin is INPUT, it has to be masked out in GPIO direction mask
            ch341_dev->gpio_mask &= ~ch341_dev->gpio_bits[ch341_dev->gpio_num];

        DEV_INFO (CH341_IF_ADDR, "%s %s gpio=%d irq=%d %s",
                  cfg->mode == CH341_PIN_MODE_IN ? "input " : "output",
                  cfg->name, ch341_dev->gpio_num, ch341_dev->irq_num,
                  cfg->hwirq ? "(hwirq)" : "");

        ch341_dev->irq_num++;
        ch341_dev->gpio_num++;
    }

    return CH341_OK;
}

static void ch341_cfg_remove (struct ch341_device* ch341_dev)
{
    CHECK_PARAM (ch341_dev);

    return;
}

// ----- board configuration layer end -----------------------------------

// ----- i2c layer begin -------------------------------------------------

static struct mutex ch341_lock;

static int ch341_i2c_set_speed (struct ch341_device *ch341_dev)
{
    static char* ch341_i2c_speed_desc[] = { "20 kbps", "100 kbps", "400 kbps", "750 kbps" };
    int result;

    CHECK_PARAM_RET (speed != speed_last, CH341_OK)

    if (speed < CH341_I2C_LOW_SPEED || speed > CH341_I2C_HIGH_SPEED)
    {
        DEV_ERR (CH341_IF_ADDR, "parameter speed can only have values from 0 to 3");
        speed = speed_last;
        return -EINVAL;
    }

    DEV_INFO (CH341_IF_ADDR, "Change i2c bus speed to %s", ch341_i2c_speed_desc[speed]);

    mutex_lock (&ch341_lock);

    ch341_dev->out_buf[0] = CH341_CMD_I2C_STREAM;
    ch341_dev->out_buf[1] = CH341_CMD_I2C_STM_SET | speed;
    ch341_dev->out_buf[2] = CH341_CMD_I2C_STM_END;
    result = ch341_usb_transfer (ch341_dev, 3, 0);

    mutex_unlock (&ch341_lock);

    if (result < 0)
    {
        DEV_ERR (CH341_IF_ADDR, "failure setting speed %d\n", result);
        return result;
    }

    speed_last = speed;

    return result;
}

static int ch341_i2c_read_inputs (struct ch341_device* ch341_dev)
{
    int result;

    if ((result = ch341_i2c_set_speed (ch341_dev)) < 0)
        return result;

    mutex_lock (&ch341_lock);

    ch341_dev->out_buf[0] = CH341_CMD_UIO_STREAM;
    ch341_dev->out_buf[1] = CH341_CMD_UIO_STM_DIR | ch341_dev->gpio_mask;
    ch341_dev->out_buf[2] = CH341_CMD_UIO_STM_IN;
    ch341_dev->out_buf[3] = CH341_CMD_UIO_STM_END;

    result = ch341_usb_transfer(ch341_dev, 4, 1);

    ch341_dev->gpio_io_data &= ch341_dev->gpio_mask;
    ch341_dev->gpio_io_data |= ch341_dev->in_buf[0] & ~ch341_dev->gpio_mask;

    mutex_unlock (&ch341_lock);

    return (result < 0) ? result : CH341_OK;
}

static int ch341_i2c_write_outputs (struct ch341_device* ch341_dev)
{
    int result;

    if ((result = ch341_i2c_set_speed (ch341_dev)) < 0)
        return result;

    mutex_lock (&ch341_lock);

    ch341_dev->out_buf[0] = CH341_CMD_UIO_STREAM;
    ch341_dev->out_buf[1] = CH341_CMD_UIO_STM_DIR | ch341_dev->gpio_mask;
    ch341_dev->out_buf[2] = CH341_CMD_UIO_STM_OUT | (ch341_dev->gpio_io_data & ch341_dev->gpio_mask);
    ch341_dev->out_buf[3] = CH341_CMD_UIO_STM_END;

    // DEV_DBG(CH341_IF_ADDR, "%02x", ch341_dev->out_buf[2]);

    result = ch341_usb_transfer(ch341_dev, 4, 0);

    mutex_unlock (&ch341_lock);

    return (result < 0) ? result : CH341_OK;
}

static int ch341_i2c_transfer (struct i2c_adapter *adpt, struct i2c_msg *msgs, int num)
{
    struct ch341_device* ch341_dev;
    int result;
    int i, j, k;

    uint8_t* ob;
    uint8_t* ib;

    CHECK_PARAM_RET (adpt, EIO);
    CHECK_PARAM_RET (msgs, EIO);
    CHECK_PARAM_RET (num > 0, EIO);

    ch341_dev = (struct ch341_device *)adpt->algo_data;

    CHECK_PARAM_RET (ch341_dev, EIO);

    mutex_lock (&ch341_lock);

    ob = ch341_dev->out_buf;
    ib = ch341_dev->in_buf;

    for (i = 0; i < num; i++)
    {
        // size larger than endpoint max transfer size
        if ((msgs[i].len + (i == num-1 ? 6 : 5) > 32))
        {
            DEV_ERR (CH341_IF_ADDR, "size of data is too large for existing USB endpoints");
            result = -EIO;
            break;
        }

        if (msgs[i].flags & I2C_M_TEN)
        {
            DEV_ERR (CH341_IF_ADDR, "10 bit i2c addresses not supported");
            result = -EINVAL;
            break;
        }

        k = 0;

        if (msgs[i].flags & I2C_M_RD) // i2c read operation
        {
            ob[k++] = CH341_CMD_I2C_STREAM;
            ob[k++] = CH341_CMD_I2C_STM_STA;       // START condition
            ob[k++] = CH341_CMD_I2C_STM_OUT | 0x1; // write len (only address byte)
            ob[k++] = (msgs[i].addr << 1) | 0x1;   // address byte with read flag

            if (msgs[i].len)
            {
                for (j = 0; j < msgs[i].len-1; j++)
                    ob[k++] = CH341_CMD_I2C_STM_IN | 1;

                ob[k++] = CH341_CMD_I2C_STM_IN;
            }

            // if the message is the last one, add STOP condition
            if (i == num-1)
                ob[k++]  = CH341_CMD_I2C_STM_STO;

            ob[k++] = CH341_CMD_I2C_STM_END;

            // wirte address byte and read data
            result = ch341_usb_transfer(ch341_dev, k, msgs[i].len);

            // if data were read
            if (result > 0)
            {
                if (msgs[i].flags & I2C_M_RECV_LEN)
                {
                    msgs[i].buf[0] = result;  // length byte
                    memcpy(msgs[i].buf+1, ib, msgs[i].len);
                }
                else
                {
                    memcpy(msgs[i].buf, ib, msgs[i].len);
                }
            }
        }
        else // i2c write operation
        {
            ob[k++] = CH341_CMD_I2C_STREAM;
            ob[k++] = CH341_CMD_I2C_STM_STA;  // START condition
            ob[k++] = CH341_CMD_I2C_STM_OUT | (msgs[i].len + 1);
            ob[k++] = msgs[i].addr << 1;  // address byte

            memcpy(&ob[k], msgs[i].buf, msgs[i].len);
            k = k + msgs[i].len;

            // if the message is the last one, add STOP condition
            if (i == num-1)
                ob[k++]  = CH341_CMD_I2C_STM_STO;

            ob[k++]  = CH341_CMD_I2C_STM_END;

            // write address byte and data
            result = ch341_usb_transfer (ch341_dev, k, 0);
        }

        if (result < 0)
            break;
    }

    mutex_unlock (&ch341_lock);

    if (result < 0)
        return result;

    return num;
}

static u32 ch341_i2c_func (struct i2c_adapter *dev)
{
    return I2C_FUNC_I2C | I2C_FUNC_SMBUS_EMUL;
}


static const struct i2c_algorithm ch341_i2c_algorithm =
{
    .master_xfer   = ch341_i2c_transfer,
    .functionality = ch341_i2c_func,
};

static int ch341_i2c_probe (struct ch341_device* ch341_dev)
{
    int result;

    CHECK_PARAM_RET (ch341_dev, -EINVAL);

    DEV_DBG (CH341_IF_ADDR, "start");

    // setup i2c adapter description
    ch341_dev->i2c_dev.owner = THIS_MODULE;
    ch341_dev->i2c_dev.class = 0; // I2C_CLASS_HWMON;
    ch341_dev->i2c_dev.algo  = &ch341_i2c_algorithm;
    ch341_dev->i2c_dev.algo_data = ch341_dev;
    snprintf(ch341_dev->i2c_dev.name, sizeof(ch341_dev->i2c_dev.name),
             "i2c-ch341-usb at bus %03d device %03d",
             ch341_dev->usb_dev->bus->busnum, ch341_dev->usb_dev->devnum);

    ch341_dev->i2c_dev.dev.parent = &ch341_dev->usb_if->dev;

    // finally attach to i2c layer
    if ((result = i2c_add_adapter(&ch341_dev->i2c_dev)) < 0)
        return result;

    DEV_INFO (CH341_IF_ADDR, "created i2c device /dev/i2c-%d", ch341_dev->i2c_dev.nr);

    mutex_init (&ch341_lock);

    // set ch341 i2c speed
    speed_last = CH341_I2C_FAST_SPEED+1;
    if ((result = ch341_i2c_set_speed (ch341_dev)) < 0)
        return result;

    DEV_DBG (CH341_IF_ADDR, "done");

    return CH341_OK;
}

static void ch341_i2c_remove (struct ch341_device* ch341_dev)
{
    CHECK_PARAM (ch341_dev);

    if (ch341_dev->i2c_dev.nr)
        i2c_del_adapter (&ch341_dev->i2c_dev);

    return;
}

// ----- i2c layer end ---------------------------------------------------

// ----- irq layer begin -------------------------------------------------

void ch341_irq_enable_disable (struct irq_data *data, bool enable)
{
    struct ch341_device *ch341_dev;
    int irq;

    CHECK_PARAM (data && (ch341_dev = irq_data_get_irq_chip_data(data)));

    // calculate local IRQ
    irq = data->irq - ch341_dev->irq_base;

    // valid IRQ is in range 0 ... ch341_dev->irq_num-1, invalid IRQ is -1
    if (irq < 0 || irq >= ch341_dev->irq_num) return;

    // enable local IRQ
    ch341_dev->irq_enabled[irq] = enable;

    DEV_INFO (CH341_IF_ADDR, "irq=%d enabled=%d",
              data->irq, ch341_dev->irq_enabled[irq] ? 1 : 0);
}

void ch341_irq_enable (struct irq_data *data)
{
    ch341_irq_enable_disable (data, true);
}

void ch341_irq_disable (struct irq_data *data)
{
    ch341_irq_enable_disable (data, false);
}

int ch341_irq_set_type (struct irq_data *data, unsigned int type)
{
    struct ch341_device *ch341_dev;
    int irq;

    CHECK_PARAM_RET (data && (ch341_dev = irq_data_get_irq_chip_data(data)), -EINVAL);

    // calculate local IRQ
    irq = data->irq - ch341_dev->irq_base;

    // valid IRQ is in range 0 ... ch341_dev->irq_num-1, invalid IRQ is -1
    if (irq < 0 || irq >= ch341_dev->irq_num) return -EINVAL;

    ch341_dev->irq_types[irq] = type;

    DEV_INFO (CH341_IF_ADDR, "irq=%d flow_type=%d", data->irq, type);

    return CH341_OK;
}

static int ch341_irq_check (struct ch341_device* ch341_dev, uint8_t irq,
                            uint8_t old, uint8_t new, bool hardware)
{
    int type;

    CHECK_PARAM_RET (old != new, CH341_OK)
    CHECK_PARAM_RET (ch341_dev, -EINVAL);
    CHECK_PARAM_RET (irq < ch341_dev->irq_num, -EINVAL);

    // valid IRQ is in range 0 ... ch341_dev->irq_num-1, invalid IRQ is -1
    if (irq < 0 || irq >= ch341_dev->irq_num) return -EINVAL;

    // if IRQ is disabled, just return with success
    if (!ch341_dev->irq_enabled[irq]) return CH341_OK;

    type = ch341_dev->irq_types[irq];

    // for software IRQs dont check if IRQ is the hardware IRQ for rising edges
    if (!hardware && irq == ch341_dev->irq_hw && new > old)
        return CH341_OK;

    if ((type & IRQ_TYPE_EDGE_FALLING && old > new) ||
        (type & IRQ_TYPE_EDGE_RISING  && new > old))
    {
        // DEV_DBG (CH341_IF_ADDR, "%s irq=%d %d %s",
        //          hardware ? "hardware" : "software",
        //          irq, type, (old > new) ? "falling" : "rising");

        #if LINUX_VERSION_CODE >= KERNEL_VERSION(4,3,0)
        handle_simple_irq (ch341_dev->irq_descs[irq]);
        #else
        handle_simple_irq (ch341_dev->irq_base+irq, ch341_dev->irq_descs[irq]);
        #endif
    }

    return CH341_OK;
}

static int ch341_irq_probe (struct ch341_device* ch341_dev)
{
    int i;
    int result;

    CHECK_PARAM_RET (ch341_dev, -EINVAL);

    DEV_DBG (CH341_IF_ADDR, "start");

    ch341_dev->irq.name         = "ch341";
    ch341_dev->irq.irq_enable   = ch341_irq_enable;
    ch341_dev->irq.irq_disable  = ch341_irq_disable;
    ch341_dev->irq.irq_set_type = ch341_irq_set_type;

    if (!ch341_dev->irq_num) return CH341_OK;

    if ((result = irq_alloc_descs(-1, 0, ch341_dev->irq_num, 0)) < 0)
    {
        DEV_ERR (CH341_IF_ADDR, "failed to allocate IRQ descriptors");
        return result;
    }

    ch341_dev->irq_base = result;

    DEV_DBG (CH341_IF_ADDR, "irq_base=%d", ch341_dev->irq_base);

    for (i = 0; i < ch341_dev->irq_num; i++)
    {
        ch341_dev->irq_descs[i]   = irq_to_desc(ch341_dev->irq_base + i);
        ch341_dev->irq_enabled[i] = false;

        irq_set_chip          (ch341_dev->irq_base + i, &ch341_dev->irq);
        irq_set_chip_data     (ch341_dev->irq_base + i, ch341_dev);
        irq_clear_status_flags(ch341_dev->irq_base + i, IRQ_NOREQUEST | IRQ_NOPROBE);
    }

    DEV_DBG (CH341_IF_ADDR, "done");

    return CH341_OK;
}

static void ch341_irq_remove (struct ch341_device* ch341_dev)
{
    CHECK_PARAM (ch341_dev);

    if (ch341_dev->irq_base)
        irq_free_descs (ch341_dev->irq_base, ch341_dev->irq_num);

    return;
}

// ----- irq layer end ---------------------------------------------------

// ----- gpio layer begin ------------------------------------------------

void ch341_gpio_read_inputs (struct ch341_device* ch341_dev)
{
    uint8_t old_io_data;
    uint8_t old_value;
    uint8_t new_value;
    uint8_t gpio;
    int i;

    CHECK_PARAM (ch341_dev);

    // DEV_DBG (CH341_IF_ADDR, "start");

    // save old value
    old_io_data = ch341_dev->gpio_io_data;

    // read current values
    ch341_i2c_read_inputs (ch341_dev);

    for (i = 0; i < ch341_dev->irq_num; i++)
    {
        // determine local GPIO for each IRQ
        gpio = ch341_dev->irq_gpio_map[i];

        // determin old an new value of the bit
        old_value = (old_io_data & ch341_dev->gpio_bits[gpio]) ? 1 : 0;
        new_value = (ch341_dev->gpio_io_data & ch341_dev->gpio_bits[gpio]) ? 1 : 0;

        // check for interrupt
        ch341_irq_check (ch341_dev, i, old_value, new_value, false);
    }

    // DEV_DBG (CH341_IF_ADDR, "done");
}

// #define CH341_POLL_WITH_SLEEP

static int ch341_gpio_poll_function (void* argument)
{
    struct ch341_device* ch341_dev = (struct ch341_device*)argument;
    unsigned int next_poll_ms = jiffies_to_msecs(jiffies);
    unsigned int jiffies_ms;
    int drift_ms = 0;
    int corr_ms  = 0;
    int sleep_ms = 0;

    CHECK_PARAM_RET (ch341_dev, -EINVAL);

    DEV_DBG (CH341_IF_ADDR, "start");

    while (!kthread_should_stop())
    {
        // current time in ms
        jiffies_ms = jiffies_to_msecs(jiffies);
        drift_ms   = jiffies_ms - next_poll_ms;

        if (poll_period == 0)
        {
            poll_period = CH341_POLL_PERIOD_MS;
            DEV_ERR (CH341_IF_ADDR,
                     "Poll period 0 ms is invalid, set back to the default of %d ms",
                     CH341_POLL_PERIOD_MS);
        }

        if (drift_ms < 0)
        {
            // period was to short, increase corr_ms by 1 ms
            // DEV_DBG (CH341_IF_ADDR, "polling GPIO is %u ms too early", -drift_ms);
            corr_ms = (corr_ms > 0) ? corr_ms - 1 : 0;
        }
        else if (drift_ms > 0 && drift_ms < poll_period)
        {
            // period was to long, decrease corr_ms by 1 ms
            // DEV_DBG (CH341_IF_ADDR, "polling GPIO is %u ms too late", drift_ms);
            corr_ms = (corr_ms < poll_period) ? corr_ms + 1 : 0;
        }

        next_poll_ms = jiffies_ms + poll_period;

        // DEV_DBG (CH341_IF_ADDR, "read CH341 GPIOs");
        ch341_gpio_read_inputs (ch341_dev);

        jiffies_ms = jiffies_to_msecs(jiffies);

        // if GPIO read took longer than poll period, do not sleep
        if (jiffies_ms > next_poll_ms)
        {
            DEV_ERR (CH341_IF_ADDR,
                     "GPIO poll period is too short by at least %u msecs",
                     jiffies_ms - next_poll_ms);
        }
        else
        {
            sleep_ms = next_poll_ms - jiffies_ms - corr_ms;

            #ifdef CH341_POLL_WITH_SLEEP
            msleep ((sleep_ms <= 0) ? 1 : sleep_ms);
            #else
            set_current_state(TASK_UNINTERRUPTIBLE);
            schedule_timeout(msecs_to_jiffies((sleep_ms <= 0) ? 1 : sleep_ms));
            #endif
        }
    }
    #ifndef CH341_POLL_WITH_SLEEP
    __set_current_state(TASK_RUNNING);
    #endif

    DEV_DBG (CH341_IF_ADDR, "stop");

    return 0;
}

int ch341_gpio_get (struct gpio_chip *chip, unsigned offset)
{
    #if LINUX_VERSION_CODE >= KERNEL_VERSION(4,5,0)
    struct ch341_device* ch341_dev = (struct ch341_device*)gpiochip_get_data(chip);
    #else
    struct ch341_device* ch341_dev = container_of(chip, struct ch341_device, gpio);
    #endif
    int value;

    CHECK_PARAM_RET (ch341_dev, -EINVAL);
    CHECK_PARAM_RET (offset < ch341_dev->gpio_num, -EINVAL);

    value = (ch341_dev->gpio_io_data & ch341_dev->gpio_bits[offset]) ? 1 : 0;

    // DEV_DBG (CH341_IF_ADDR, "offset=%u value=%d io_data=%02x",
    //          offset, value, ch341_dev->gpio_io_data);

    return value;
}

// FIXME: not tested at the moment (will be introduced with kernel 4.15.0)
int ch341_gpio_get_multiple (struct gpio_chip *chip,
                             unsigned long *mask, unsigned long *bits)
{
    #if LINUX_VERSION_CODE >= KERNEL_VERSION(4,5,0)
    struct ch341_device* ch341_dev = (struct ch341_device*)gpiochip_get_data(chip);
    #else
    struct ch341_device* ch341_dev = container_of(chip, struct ch341_device, gpio);
    #endif
    int i;

    CHECK_PARAM_RET (ch341_dev, -EINVAL);
    CHECK_PARAM_RET (mask, -EINVAL);
    CHECK_PARAM_RET (bits, -EINVAL);

    for (i = 0; i < ch341_dev->gpio_num; i++)
        if (*mask & (1 << i))
        {
            *bits &= ~(1 << i);
            *bits |= (((ch341_dev->gpio_io_data & ch341_dev->gpio_bits[i]) ? 1 : 0) << i);
        }

    // DEV_DBG (CH341_IF_ADDR, "mask=%08lx bit=%08lx io_data=%02x",
    //          *mask, *bits, ch341_dev->gpio_io_data);

    return CH341_OK;
}

void ch341_gpio_set (struct gpio_chip *chip, unsigned offset, int value)
{
    #if LINUX_VERSION_CODE >= KERNEL_VERSION(4,5,0)
    struct ch341_device* ch341_dev = (struct ch341_device*)gpiochip_get_data(chip);
    #else
    struct ch341_device* ch341_dev = container_of(chip, struct ch341_device, gpio);
    #endif

    CHECK_PARAM (ch341_dev);
    CHECK_PARAM (offset < ch341_dev->gpio_num);

    if (value)
        ch341_dev->gpio_io_data |= ch341_dev->gpio_bits[offset];
    else
        ch341_dev->gpio_io_data &= ~ch341_dev->gpio_bits[offset];

    // DEV_DBG (CH341_IF_ADDR, "offset=%u value=%d io_data=%02x",
    //          offset, value, ch341_dev->gpio_io_data);

    ch341_i2c_write_outputs (ch341_dev);
}

void ch341_gpio_set_multiple (struct gpio_chip *chip,
                              unsigned long *mask, unsigned long *bits)
{
    #if LINUX_VERSION_CODE >= KERNEL_VERSION(4,5,0)
    struct ch341_device* ch341_dev = (struct ch341_device*)gpiochip_get_data(chip);
    #else
    struct ch341_device* ch341_dev = container_of(chip, struct ch341_device, gpio);
    #endif
    int i;

    CHECK_PARAM (ch341_dev);
    CHECK_PARAM (mask);
    CHECK_PARAM (bits);

    for (i = 0; i < ch341_dev->gpio_num; i++)
        if (*mask & (1 << i) && ch341_dev->gpio_pins[i]->mode == CH341_PIN_MODE_OUT)
        {
            if (*bits & (1 << i))
                ch341_dev->gpio_io_data |= ch341_dev->gpio_bits[i];
            else
                ch341_dev->gpio_io_data &= ~ch341_dev->gpio_bits[i];
        }

    // DEV_DBG (CH341_IF_ADDR, "mask=%08lx bit=%08lx io_data=%02x",
    //          *mask, *bits, ch341_dev->gpio_io_data);

    ch341_i2c_write_outputs (ch341_dev);

    return;
}


int ch341_gpio_get_direction (struct gpio_chip *chip, unsigned offset)
{
    #if LINUX_VERSION_CODE >= KERNEL_VERSION(4,5,0)
    struct ch341_device* ch341_dev = (struct ch341_device*)gpiochip_get_data(chip);
    #else
    struct ch341_device* ch341_dev = container_of(chip, struct ch341_device, gpio);
    #endif
    int mode;

    CHECK_PARAM_RET (ch341_dev, -EINVAL);
    CHECK_PARAM_RET (offset < ch341_dev->gpio_num, -EINVAL);

    mode = (ch341_dev->gpio_pins[offset]->mode == CH341_PIN_MODE_IN) ? 1 : 0;

    DEV_DBG (CH341_IF_ADDR, "gpio=%d dir=%d", offset, mode);

    return mode;
}

int ch341_gpio_set_direction (struct gpio_chip *chip, unsigned offset, bool input)
{
    #if LINUX_VERSION_CODE >= KERNEL_VERSION(4,5,0)
    struct ch341_device* ch341_dev = (struct ch341_device*)gpiochip_get_data(chip);
    #else
    struct ch341_device* ch341_dev = container_of(chip, struct ch341_device, gpio);
    #endif

    CHECK_PARAM_RET (ch341_dev, -EINVAL);
    CHECK_PARAM_RET (offset < ch341_dev->gpio_num, -EINVAL);

    // pin configured correctly if it is pin 21
    if (ch341_dev->gpio_pins[offset]->pin == 21 && !input)
    {
        DEV_ERR(CH341_IF_ADDR, "pin 21: must be an input");
        return -EINVAL;
    }

    DEV_INFO (CH341_IF_ADDR, "gpio=%d direction=%s", offset, input ? "input" :  "output");

    ch341_dev->gpio_pins[offset]->mode = input ? CH341_PIN_MODE_IN : CH341_PIN_MODE_OUT;

    // mask in / mask out the according bit in direction mask
    if (ch341_dev->gpio_pins[offset]->mode == CH341_PIN_MODE_OUT)
        ch341_dev->gpio_mask |= ch341_dev->gpio_bits[offset];
    else
        ch341_dev->gpio_mask &= ~ch341_dev->gpio_bits[offset];

    return CH341_OK;
}

int ch341_gpio_direction_input (struct gpio_chip *chip, unsigned offset)
{
    return ch341_gpio_set_direction (chip, offset, true);
}

int ch341_gpio_direction_output (struct gpio_chip *chip, unsigned offset, int value)
{
    int result = CH341_OK;

    if ((result = ch341_gpio_set_direction (chip, offset, false)) == CH341_OK)
        // set initial output value
        ch341_gpio_set (chip, offset, value);

    return result;
}

int ch341_gpio_to_irq (struct gpio_chip *chip, unsigned offset)
{
    #if LINUX_VERSION_CODE >= KERNEL_VERSION(4,5,0)
    struct ch341_device* ch341_dev = (struct ch341_device*)gpiochip_get_data(chip);
    #else
    struct ch341_device* ch341_dev = container_of(chip, struct ch341_device, gpio);
    #endif
    int irq;

    CHECK_PARAM_RET (ch341_dev, -EINVAL);
    CHECK_PARAM_RET (offset < ch341_dev->gpio_num, -EINVAL);

    // valid IRQ is in range 0 ... ch341_dev->irq_num, invalid IRQ is -1
    irq = ch341_dev->gpio_irq_map[offset];
    irq = (irq >= 0 ? ch341_dev->irq_base + irq : 0);

    DEV_DBG (CH341_IF_ADDR, "gpio=%d irq=%d", offset, irq);

    return irq;
}


static int ch341_gpio_probe (struct ch341_device* ch341_dev)
{
    struct gpio_chip *gpio = &ch341_dev->gpio;
    int result;
    int i, j = 0;

    CHECK_PARAM_RET (ch341_dev, -EINVAL);

    DEV_DBG (CH341_IF_ADDR, "start");

    gpio->label     = "ch341";

    #if LINUX_VERSION_CODE >= KERNEL_VERSION(4,5,0)
    gpio->parent = &ch341_dev->usb_dev->dev;
    #else
    gpio->dev    = &ch341_dev->usb_dev->dev;
    #endif

    gpio->owner  = THIS_MODULE;
    gpio->request= NULL;
    gpio->free   = NULL;

    gpio->base   = -1;   // request dynamic ID allocation
    gpio->ngpio  = ch341_dev->gpio_num;

    gpio->can_sleep = 1;
    gpio->names     = (void*)ch341_dev->gpio_names;

    #if LINUX_VERSION_CODE >= KERNEL_VERSION(3,9,0)
    gpio->get_direction     = ch341_gpio_get_direction;
    #endif
    gpio->direction_input   = ch341_gpio_direction_input;
    gpio->direction_output  = ch341_gpio_direction_output;
    gpio->get               = ch341_gpio_get;
    #if LINUX_VERSION_CODE >= KERNEL_VERSION(4,15,0)
    gpio->get_multiple      = ch341_gpio_get_multiple;  // FIXME: NOT TESTED
    #endif
    gpio->set               = ch341_gpio_set;
    #if LINUX_VERSION_CODE >= KERNEL_VERSION(3,19,0)
    gpio->set_multiple      = ch341_gpio_set_multiple;  // FIXME: NOT TESTED
    #endif

    gpio->to_irq            = ch341_gpio_to_irq;

    #if LINUX_VERSION_CODE >= KERNEL_VERSION(4,5,0)
    if ((result = gpiochip_add_data (gpio, ch341_dev)))
    #else
    if ((result = gpiochip_add (gpio)))
    #endif
    {
        DEV_ERR (CH341_IF_ADDR, "failed to register GPIOs: %d", result);
        // in case of error, reset gpio->base to avoid crashes during free
        gpio->base   = -1;
        return result;
    }

    DEV_DBG (CH341_IF_ADDR, "registered GPIOs from %d to %d",
             gpio->base, gpio->base + gpio->ngpio - 1);

    for (i = 0; i < CH341_GPIO_NUM_PINS; i++)
    {
        // in case the pin as CS signal, it is an GPIO pin
        if ((result = gpio_request(gpio->base + j, ch341_board_config[i].name)) ||
            (result = gpio_export (gpio->base + j, ch341_board_config[i].pin != 21 ? true : false)))
        {
            DEV_ERR (CH341_IF_ADDR, "failed to export GPIO %s: %d",
                     ch341_board_config[i].name, result);
            // reduce number of GPIOs to avoid crashes during free in case of error
            ch341_dev->gpio_num = j ? j-1 : 0;
            return result;
        }
        j++;
    }

    ch341_dev->gpio_thread = kthread_run (&ch341_gpio_poll_function, ch341_dev, "i2c-ch341-usb-poll");

    DEV_DBG (CH341_IF_ADDR, "done");

    return 0;
}

static void ch341_gpio_remove (struct ch341_device* ch341_dev)
{
    int i;
    #if LINUX_VERSION_CODE < KERNEL_VERSION(3,17,0)
    int result;
    #endif

    CHECK_PARAM (ch341_dev);

    if (ch341_dev->gpio_thread)
    {
        kthread_stop(ch341_dev->gpio_thread);
        wake_up_process (ch341_dev->gpio_thread);
    }

    if (ch341_dev->gpio.base > 0)
    {
        for (i = 0; i < ch341_dev->gpio_num; ++i)
           gpio_free(ch341_dev->gpio.base + i);

        #if LINUX_VERSION_CODE >= KERNEL_VERSION(3,17,0)
        gpiochip_remove(&ch341_dev->gpio);
        #else
        result = gpiochip_remove(&ch341_dev->gpio);
        #endif
    }

    return;
}

// ----- gpio layer end --------------------------------------------------

// ----- usb layer begin -------------------------------------------------

static const struct usb_device_id ch341_usb_table[] = {
    { USB_DEVICE(0x1a86, 0x5512) },
    { }
};

MODULE_DEVICE_TABLE(usb, ch341_usb_table);

static int ch341_usb_transfer(struct ch341_device *ch341_dev, int out_len, int in_len)
{
    int retval;
    int actual;

    // DEV_DBG (CH341_IF_ADDR, "bulk_out %d bytes, bulk_in %d bytes",
    //          out_len, (in_len == 0) ? 0 : CH341_USB_MAX_BULK_SIZE);

    retval = usb_bulk_msg(ch341_dev->usb_dev,
                          usb_sndbulkpipe(ch341_dev->usb_dev,
                                          usb_endpoint_num(ch341_dev->ep_out)),
                          ch341_dev->out_buf, out_len,
                          &actual, 2000);
    if (retval < 0)
        return retval;

    if (in_len == 0)
        return actual;

    memset(ch341_dev->in_buf, 0, sizeof(ch341_dev->in_buf));
    retval = usb_bulk_msg(ch341_dev->usb_dev,
                          usb_rcvbulkpipe(ch341_dev->usb_dev,
                                          usb_endpoint_num(ch341_dev->ep_in)),
                          ch341_dev->in_buf, CH341_USB_MAX_BULK_SIZE,
                          &actual, 2000);

    if (retval < 0)
        return retval;

    return actual;
}

static void ch341_usb_complete_intr_urb (struct urb *urb)
{
    struct ch341_device *ch341_dev;

    CHECK_PARAM (urb);
    CHECK_PARAM (ch341_dev = urb->context);

    if (!urb->status)
    {
        // hardware IRQs are only generated for one IRQ and rising edges 0 -> 1
        DEV_DBG (CH341_IF_ADDR, "%d", urb->status);

        // because of asynchronous GPIO read, the GPIO value has to be set to 1
        ch341_dev->gpio_io_data |= ch341_dev->gpio_bits[ch341_dev->irq_gpio_map[ch341_dev->irq_hw]];

        // IRQ has to be triggered
        ch341_irq_check (ch341_dev, ch341_dev->irq_hw, 0, 1, true);

        // submit next request
        usb_submit_urb(ch341_dev->intr_urb, GFP_ATOMIC);
    }
}

static void ch341_usb_free_device (struct ch341_device* ch341_dev)
{
    CHECK_PARAM (ch341_dev)

    ch341_gpio_remove (ch341_dev);
    ch341_irq_remove  (ch341_dev);
    ch341_i2c_remove  (ch341_dev);
    ch341_cfg_remove  (ch341_dev);

    if (ch341_dev->intr_urb) usb_free_urb (ch341_dev->intr_urb);

    usb_set_intfdata (ch341_dev->usb_if, NULL);
    usb_put_dev (ch341_dev->usb_dev);

    kfree (ch341_dev);
}

static int ch341_usb_probe (struct usb_interface* usb_if,
                            const struct usb_device_id* usb_id)
{
    struct usb_device* usb_dev = usb_get_dev(interface_to_usbdev(usb_if));
    struct usb_endpoint_descriptor *epd;
    struct usb_host_interface *settings;
    struct ch341_device* ch341_dev;
    int i;
    int error;

    DEV_DBG (&usb_if->dev, "connect device");

    // create and initialize a new device data structure
    if (!(ch341_dev = kzalloc(sizeof(struct ch341_device), GFP_KERNEL)))
    {
        DEV_ERR (&usb_if->dev, "could not allocate device memor");
        usb_put_dev (ch341_dev->usb_dev);
        return -ENOMEM;
    }

    // save USB device data
    ch341_dev->usb_dev = usb_dev;
    ch341_dev->usb_if  = usb_if;

    // find endpoints
    settings = usb_if->cur_altsetting;
    DEV_DBG (CH341_IF_ADDR, "bNumEndpoints=%d", settings->desc.bNumEndpoints);

    for (i = 0; i < settings->desc.bNumEndpoints; i++)
    {
        epd = &settings->endpoint[i].desc;

        DEV_DBG (CH341_IF_ADDR, "  endpoint=%d type=%d dir=%d addr=%0x", i,
                 usb_endpoint_type(epd), usb_endpoint_dir_in(epd),
                 usb_endpoint_num(epd));

        if (usb_endpoint_is_bulk_in (epd)) ch341_dev->ep_in   = epd; else
        if (usb_endpoint_is_bulk_out(epd)) ch341_dev->ep_out  = epd; else
        if (usb_endpoint_xfer_int   (epd)) ch341_dev->ep_intr = epd;
    }
    // create URBs for handling interrupts
    if (!(ch341_dev->intr_urb = usb_alloc_urb(0, GFP_KERNEL)))
    {
        DEV_ERR (&usb_if->dev, "failed to alloc URB");
        ch341_usb_free_device (ch341_dev);
        return -ENOMEM;
    }

    usb_fill_int_urb (ch341_dev->intr_urb, ch341_dev->usb_dev,
                      usb_rcvintpipe(ch341_dev->usb_dev,
                                     usb_endpoint_num(ch341_dev->ep_intr)),
                      ch341_dev->intr_buf, CH341_USB_MAX_INTR_SIZE,
                      ch341_usb_complete_intr_urb, ch341_dev,
                      ch341_dev->ep_intr->bInterval);

    // save the pointer to the new ch341_device in USB interface device data
    usb_set_intfdata(usb_if, ch341_dev);

    if ((error = ch341_cfg_probe (ch341_dev)) ||  // initialize board configuration
        (error = ch341_i2c_probe (ch341_dev)) ||  // initialize I2C adapter
        (error = ch341_irq_probe (ch341_dev)) ||  // initialize IRQs
        (error = ch341_gpio_probe(ch341_dev)))    // initialize GPIOs
    {
        ch341_usb_free_device (ch341_dev);
        return error;
    }

    usb_submit_urb (ch341_dev->intr_urb, GFP_ATOMIC);

    DEV_INFO (CH341_IF_ADDR, "connected");

    return CH341_OK;
}

static void ch341_usb_disconnect(struct usb_interface *usb_if)
{
    struct ch341_device* ch341_dev = usb_get_intfdata(usb_if);

    DEV_INFO (CH341_IF_ADDR, "disconnected");

    ch341_usb_free_device (ch341_dev);
}

static struct usb_driver ch341_usb_driver = {
    .name       = "i2c-ch341-usb",
    .id_table   = ch341_usb_table,
    .probe      = ch341_usb_probe,
    .disconnect = ch341_usb_disconnect
};

module_usb_driver(ch341_usb_driver);

// ----- usb layer end ---------------------------------------------------

MODULE_ALIAS("i2c:ch341");
MODULE_AUTHOR("Gunar Schorcht <gunar@schorcht.net>");
MODULE_DESCRIPTION("i2c-ch341-usb driver v1.0.0");
MODULE_LICENSE("GPL");

module_param(speed, uint, 0644);
MODULE_PARM_DESC(speed, " I2C bus speed: 0 (20 kbps), 1 (100 kbps), 2 (400 kbps), 3 (750 kbps): ");

module_param(poll_period, uint, 0644);
MODULE_PARM_DESC(poll_period, "GPIO polling period in ms (default 10 ms)");
#endif // LINUX_VERSION_CODE < KERNEL_VERSION(3,4,0)

