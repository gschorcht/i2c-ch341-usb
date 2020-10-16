[![noswpatv3](http://zoobab.wdfiles.com/local--files/start/noupcv3.jpg)](https://ffii.org/donate-now-to-save-europe-from-software-patents-says-ffii/)
# CH341A USB to I2C and GPIO Linux kernel driver

The driver can be used with **CH341A** USB to UART/I2C/SPI adapter boards to connect I2C devices to a Linux host.

Additionally, CH341A data pins that are not used for synchronous serial interfaces can be configured as **GPIO** pins. The driver can generate **software interrupts** for all input pins. **One input** pin can be connected with the CH341A interrupt pin to generate **hardware interrupts**. 

The I2C interface driver was initially derived from CH341 I2C driver from Tse Lun Bien [https://github.com/allanbian1017/i2c-ch341-usb.git] and extended by GPIO and interrupt handling capabilities.

## I2C interface limitations

By default, the driver uses the standard I2C bus speed of **100 kbps**. I2C bus speeds of 20 kbps, **400 kbps** and 750 kbps are also available. 

Currently only basic I2C read and write functions (**```I2C_FUNC_I2C```**) are supported natively. However, SMBus protocols are emulated (**```I2C_FUNC_SMBUS_EMUL```**) with the exception of SMBus Block Read (```I2C_FUNC_SMBUS_READ_BLOCK_DATA```) and SMBus Block Process Call (```I2C_FUNC_SMBUS_BLOCK_PROC_CALL```).

The CH341A only supports **7 bit addressing**.

Due of the limited CH341A USB endpoint buffer size of 32 byte that is used for I2C data as well as adapter in-band signaling, the driver supports only I2C messages with a **maximum data size of 26 bytes**.

## GPIO limitations

Data pins D0...D7, which are normally used for SPI interface but not for the I2C interface, can be configured as GPIO pins as following. 

| Pin | Name | SPI Function (default) | Configurable as (**CH341 default in bold face**)   |
| --- | ---- | ---------------------- | ------------------ |
| 15  | D0   | CS0                    | Input, **Output**  |
| 16  | D1   | CS0                    | Input, **Output**  |
| 17  | D2   | CS0                    | Input, **Output**  |
| 18  | D3   | SCK                    | Input, **Output**  |
| 19  | D4   | OUT2                   | Input, **Output**  |
| 20  | D5   | OUT                    | Input, **Output**  |
| 21  | D6   | IN2                    | **Input**          |
| 22  | D7   | IN                     | **Input**          |

**Please note:** 
- Direction of pins 15 to 20 can be changed during runtime.
- Pin 21 and pin 22 can only be configured as input and their direction can't be changed during runtime.
- One of the inputs can be configured to generate **hardware interrupts for rising edges** of signals. For that purpose, the pin has to be connected with the CH341A **INT** pin 7. 

Since USB access is asynchronous, it is **not possible to guarantee exact timings** for GPIOs and interrupts.

## Installation of the driver

#### Prerequisites

To compile the driver, you must have installed current **kernel header files**. 

Even though it is not mandatory, it is highly recommended to use **DKMS** (dynamic kernel module support) for the installation of the driver. DKMS allows to manage kernel modules whose sources reside outside the kernel source tree. Such modules are then automatically rebuilt when a new kernel version is installed.

To use DKMS, it has to be installed before, e.g., with command
```
sudo apt-get install dkms
```
on Debian based systems.

#### Installaton

The driver can be compiled with following commands:

```
git clone https://github.com/gschorcht/i2c-ch341-usb.git
cd i2c-ch341-usb
make

sudo make install
```

If **DKMS** is installed (**recommended**), command ```sudo make install``` adds the driver to the DKMS tree so that the driver is recompiled automatically when a new kernel version is installed.

In case you have not installed DKMS, command ```sudo make install``` simply copies the driver after compilation to the kernel modules directory. However, the module will not be loadable anymore and have to be recompiled explicitly when kernel version changes.

If you do not want to install the driver in the kernel directory at all because you only want to load it manually when needed, simply omit the ```sudo make install```.

#### Loading

Once the driver is installed, it should be loaded automatically when you connect a device with USB device id ```1a86:5512```. If not try to figure out, whether the USB device is detected correctly using command

```
lsusb
```
and try to load it manually with command:
```
insmod i2c-ch341-usb.ko
```

#### Uninstallation

To uninstall the module simply use command
```
make uninstall
```
in the source directory.

#### Conflicts with CH341A USB to SPI Linux kernel driver

Since the CH341A also provides an SPI interface as USB device with same id, you have to unload the driver module with

```
rmmod i2c-ch341-usb
```

before you can load the driver module for the SPI interface.

## Configuration of the driver

By default, the driver uses an I2C bus speed of 100 kbps, configures the GPIOs as follows, and polls the inputs every 10 ms. Outputs are always written directly when they change.

| Pin | Name | GPIO name | GPIO function | Direction changable | IRQ      |
| --- | ---- |---------- |-------------- | --- | ---------|
| 15  | D0   | gpio0     | Output        | yes | software |
| 16  | D1   | gpio1     | Output        | yes | software |
| 17  | D2   | gpio2     | Output        | yes | software |
| 18  | D3   | gpio3     | Output        | yes | software |
| 19  | D4   | gpio4     | Input         | yes | hardware |
| 20  | D5   | gpio5     | Input         | yes | software |
| 21  | D6   | gpio6     | Input         | no  | software |
| 22  | D7   | gpio7     | Input         | no  | software |

### GPIO configuration

To change the **GPIO configuration**, simply change the variable ```ch341_board_config``` before compilation that should be self-explaining. It contains an entry for each configurable pin. Each entry consists of the pin number, the GPIO mode used for the pin, the name used for the GPIO in the Linux host and a flag whether the pin is connected with the CH341A hardware interrupt pin **INT**. Default configuration is:

```
struct ch341_pin_config ch341_board_config[CH341_GPIO_NUM_PINS] = 
{
    // pin  GPIO mode           GPIO name   hwirq
    {   15, CH341_PIN_MODE_OUT , "gpio0"    , 0 }, // used as output
    {   16, CH341_PIN_MODE_OUT , "gpio1"    , 0 }, // used as output
    {   17, CH341_PIN_MODE_OUT , "gpio2"    , 0 }, // used as output
    {   18, CH341_PIN_MODE_OUT , "gpio3"    , 0 }, // used as output
    {   19, CH341_PIN_MODE_IN  , "gpio4"    , 1 }, // used as input with hardware IRQ
    {   20, CH341_PIN_MODE_IN  , "gpio5"    , 0 }, // used as input
    {   21, CH341_PIN_MODE_IN  , "gpio6"    , 0 }, // used as input
    {   22, CH341_PIN_MODE_IN  , "gpio7"    , 0 }  // used as input
};

```
In this configuration, pins 15 to 18 are used as outputs while pins 19 to 22 are used as inputs. Additionally, pin 19 is connected with the CH341A hardware interrupt pin **INT** that produces hardware interrupts on rising edge of the signal connected to pin 19.

**Please note:** 
- Pin 21 and pin 22 can only be configured as input. Their direction can't be changed during runtime.
- Hardware interrupts can only be generated for rising edges of signals.
- Only one of the input pins can be configured to generate hardware interrupts (```hwirq``` set to 1).
- The signal at the input pin that is configured to generate hardware interrupts (```hwirq``` set to 1) **MUST** also be connected to the CH341A **INT** pin 7.
- If there is no input that should generate hardware interrupts, set ```hwirq``` to 0 for all entries.

If you want to use 6 GPIOs in output mode, you have to configure it with:

```
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
```

### Configuration of GPIO polling rate

GPIO inputs are polled periodically by a kernel thread. GPIO polling rate defines the **rate at which the kernel thread polls the GPIO inputs** and determines whether to generate **software interrupts**. That is, it defines the maximum rate at which changes at GPIO inputs can be recognized at all and software interrupts can be generated. 

The GPIO polling rate is defined by its period in milliseconds using the constant ```CH341_POLL_PERIOD_MS```. The period must be at least 10 ms, but should be 20 ms or more, if possible, depending on the performance of your system. Please check your ```syslog``` for messages like ```"GPIO poll period is too short by at least %n msecs"```. This message is thrown if the defined ```CH341_POLL_PERIOD_MS``` is shorter than the time required for one reading of the GPIOs. 

The higher GPIO polling rate is, the higher is the system usage by the kernel thread. On the other hand, the probability that short interrupt events will be lost grows, the lower the GPIO polling rate becomes.

GPIO polling rate can also be changed using the module parameter ```poll_rate``` either when loading the module, e.g.,

```
sudo modprobe i2c_ch341_usb poll_rate=50
```
or as real ```root``` during runtime using sysfs, e.g.,
```
echo 50 > /sys/module/i2c_ch341_usb/parameters/poll_period
```

**Please note:** Since the CH341A hardware interrupt signal **INT** uses a separate USB endpoint, the maximum rate of hardware interrupts is independent on the GPIO polling rate and can reach up to 400 Hz.

### Configuration of I2C bus speed

The I2C bus speed can be configured using the module parameter ```speed```. The following I2C bus speeds are supported.

| Parameter value | I2C bus speed |
| --------------- | ------------- |
| 0  | 20 kbps    |
| 1  | 100 kbps   |
| 2  | 400 kbps   |
| 0  | 750 kbps   |

By default the driver uses an I2C bus speed of 100 kbps (speed=0). It can be changed using the module parameter ```speed``` either when loading the module, e.g.,
```
sudo modprobe i2c-ch341-usb speed=2
```
or as real ```root``` during runtime using sysf, e.g.,
```
echo 2 > /sys/module/i2c_ch341_usb/parameters/speed 
```

## Usage from user space

### Using I2C slaves

Once the driver is loaded successfully, it provides an new I2C bus as device, e.g.,

```
/dev/i2c-2
```

according to the naming scheme ```/dev/i2c-<bus>``` where ```<bus>``` is the bus number selected automatically by the driver. Standard I/O functions such as ```open```, ```read```, ```write```, ```ioctl``` and ```close``` can then be used to communicate with slaves which are connected to this I2C bus.

#### Open the I2C device

To open the I2C bus device for data transfer simply use function ```open```, e.g.,
```
int i2c_bus = open ("/dev/i2c-2", O_RDWR));
```
Once the device is opened successfully, you can communicate with the slaves connected to the I2C bus.

Function ```close``` can be used to close the device anytime.

#### Data transfer with function ```ioctl```

Before data are transfered using function ```ioctl```, a data structure of type ```struct i2c_rdwr_ioctl_data``` has to be created. This can either contain only a single I2C message of type ```struct i2c_msg``` or an array of I2C messages of type ```struct i2c_msg```, all of which are transfered together as a combined transaction. In latter case each I2C message begins with start condition, but only the last ends with stop condition to indicate the end of the combined transaction.

Each I2C message consists of 

- a slave address, 
- some flags combined into a single value, e.g., read/write flag, 
- a pointer to the buffer for data bytes written to or read from the slave, and
- the length of data in bytes written to or read from the slave.

The following example shows an array of messages with two command messages written to the slave and two data messages to read the results from the slave.
```
#define I2C_SLAVE_ADDR   0x18

uint8_t i2c_id_cmd  [] = { 0x0f };  // get ID command 
uint8_t i2c_rd_cmd  [] = { 0xa8 };  // read data command

uint8_t i2c_id_data [1];            // ID is one byte
uint8_t i2c_rd_data [6];            // data are 6 bytes

struct i2c_msg i2c_messages[] = 
{
    {
        .addr  = I2C_SLAVE_ADDR,
        .flags = 0,
        .buf   = i2c_id_cmd,
        .len   = sizeof(i2c_id_cmd)        
    },
    {
        .addr  = I2C_SLAVE_ADDR,
        .flags = I2C_M_RD,
        .buf   = i2c_id_data,
        .len   = sizeof(i2c_id_data)
    },
    {
        .addr  = I2C_SLAVE_ADDR,
        .flags = 0,
        .buf   = i2c_rd_cmd,
        .len   = sizeof(i2c_rd_cmd)
    },
    {
        .addr  = I2C_SLAVE_ADDR,
        .flags = I2C_M_RD,
        .buf   = i2c_rd_data,
        .len   = sizeof(i2c_rd_data)
    }
};
```
These messages can then be transfered to the slave by filling a data structure of type ```struct i2c_rdwr_ioctl_data``` with them and calling function ```ioctl```, e.g., 
```
struct i2c_rdwr_ioctl_data ioctl_data = 
{
    .msgs  = i2c_messages,
    .nmsgs = 4
};

if (ioctl(i2c_bus, I2C_RDWR, &ioctl_data) < 0)
{
    perror("ioctl");
    return -1;
}
```

#### Using ```read``` and ```write``` for data transfer

Functions ```read``` and ```write``` can also be used for data transfer. However, since these functions do not allow to specify the slave address, it has to be set before using function ```ioctl```.

```
if (ioctl(i2c_bus, I2C_SLAVE_FORCE, I2C_SLAVE_ADDR) < 0)
{
    perror("Could not set i2c slave addr");
    return -1;
}
```

This slave address is then used for all subsequent ```read``` and ```write``` function calls until it is changed again with function ```ioctl```.

Supposing the data are preparated as in the example with ```ioctl```, the transfer of them is quite simple.

```
if (write(i2c_bus, i2c_id_cmd, sizeof(i2c_id_cmd)) != sizeof(i2c_id_cmd))
{
    perror("Could not write id command to i2c slave");
    return -1;
}

if (read (i2c_bus, i2c_id_data, sizeof(i2c_id_data)) != sizeof(i2c_id_data))
{
    perror("Could not write read id from i2c slave");
    return -1;
}

if (write(i2c_bus, i2c_rd_cmd, sizeof(i2c_rd_cmd)) != sizeof(i2c_rd_cmd))
{
    perror("Could not write read data command to i2c slave");
    return -1;
}

if (read (i2c_bus, i2c_rd_data, sizeof(i2c_rd_data)) != sizeof(i2c_rd_data))
{
    perror("Could not write read data from i2c slave");
    return -1;
}

```

### Using GPIOs

To access GPIOs from user space, ```sysfs``` can be used . For each configured GPIO, a directory 
```
/sys/class/gpio/<gpio>/
```
is created by the system, where ```<gpio>``` is the name of the GPIO as defined in the driver variable ```ch341_board_config```. These directories contain

- the file ```value``` that can be used to read from and write to GPIOs,
- the file ```edge``` that can be used to control whether and what type of interrupt is enabled, and
- the file ```direction``` that can be used to change the direction of the GPIO if possible.

**Please note:** For read and write operations from and/or to these files, the user requires read and/or write permissions, respectively.

#### Open a GPIO

Before a GPIO can be used, file ```value``` has to be opened

```
int  fd;

if ((fd = open("/sys/class/gpio/<gpio>/value", O_RDWR)) == -1) 
{
    perror("open");
    return -1;
}
```
where ```<gpio>``` is again the name of the GPIO.

#### Write GPIO output

Once the file ```value``` is opened, you can use standard I/O functions to read and write. To write a GPIO value, simply use function ```write``` as following. The value is written to the GPIO out immediately.

```
if (write(fd, value ? "1" : "0", 1) == -1) 
{
    perror ("write");
	return -1;
}
```

#### Read GPIO input

To read values from GPIOs immediately, you can simply use function ```read``` as following:

```
char buf;

if (read(fd, &buf, 1) == -1) 
{
    perror("read");
    return -1;
}

value = (buf == '0') ? 0 : 1;
```
After each read operation, file position has to be rewound to first character before the next value can be read.
```
if (lseek(fd, 0, SEEK_SET) == -1) {
    perror("lseek");
    return -1;
}
```

#### Reacting on GPIO input interrupt

Function ```poll``` can be used before function ```read``` to react and read values from the GPIO only on interrupts.

```
struct pollfd fds[1];

fds[0].fd = fd;
fds[0].events = POLLPRI;

if (poll(fds, 1, -1) == -1) 
{
    perror("poll");
    return -1;
}
```

Function ```poll``` blocks until the specified event on the file descriptor happened.

**Please note**: The interrupt has to be activated before as ```root``` with command
```
echo <type> > /sys/class/gpio/<gpio>/edge
```
where ```<gpio>``` is again the name of the GPIO and ```<type>``` is the type of the interrupt that should be used. Possible interrupt types are 

- ```rising``` for interrupts on rising signal edges,
- ```falling``` for interrupts on falling signal edges, and
- ```both``` for interrupts on rising as well as falling signal edges.

For example, following command would activate interrupts for rising edges of the signal connected to ```gpio4```. The command has to be executed as real ```root```, using ```sudo``` command doesn't work.

```
echo rising > /sys/class/gpio/gpio4/edge
```

Even though the driver defines software interrupts for GPIO inputs as well as GPIO outputs, they can be activated only for GPIO inputs.

Full examples for GPIO output and interrupt input can be found in the driver's directory.

#### Change the GPIO direction

To change the direction of a GPIO pin configured as input or output, simply write as real ```root``` keyword ```in``` or keyword ```out``` to the file ```direction```, e.g.

```
echo out > /sys/class/gpio/gpio4/direction
```
