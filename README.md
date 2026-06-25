qcc -Vgcc_ntoaarch64le -std=gnu11 -O2 -I/home/yasmine/hardware-component-samples/common/system/gpio pd_test.c rpi_gpio.c -o pd_test

qcc -Vgcc_ntoaarch64le -std=gnu11 -O2   -I/home/yasmine/hardware-component-samples/common/system/gpio   -I/home/yasmine/hardware-component-samples/common/rpi_gpio/public   motor_controller.c   /home/yasmine/hardware-component-samples/common/rpi_gpio/rpi_gpio.c   ./rpi_spi.c -o motor_controller


scp motor_controller qnxuser@192.168.1.62:/data/home/qnxuser/

ifconfig cgem0 192.168.1.62 netmask 255.255.255.0 up

/system/etc/spi.conf → dev0 at clock_rate=4000000, cpha= (whatever your working grep cpha showed), cpol=0, word_width=8, idle_insert=0, then restart spi-dwc.
The wiring (CE0→PB12, SCLK→PB13, MISO→PB14, MOSI→PB15, GPIO17→PB0, common GND) and the 1 kΩ pull-down on GPIO17.

sed -i 's/clock_rate=4000000/clock_rate=1000000/' /system/etc/spi.conf; slay spi-dwc; spi-dwc -c /system/etc/spi.conf &

and the two runtime settings that aren't in the code live in /system/etc/spi.conf (4 MHz, mode 0, 8-bit) and the hardware (CE0→NSS wire, 1 kΩ pull-down on GPIO17). Worth jotting those last two somewhere in the repo's README, since a fresh image won't have them and that's exactly the rabbit hole we just climbed out of.

[dev]
parent_busno=0
devno=0
name=dev0
clock_rate=4000000
cpha=1
cpol=0
bit_order=msb
word_width=8
idle_insert=0

sed -i 's/clock_rate=100000000/clock_rate=4000000/; s/cpha=1/cpha=0/; s/word_width=32/word_width=8/; s/idle_insert=1/idle_insert=0/' /system/etc/spi.conf   
slay spi-dwc
spi-dwc -c /system/etc/spi.conf &
grep -E 'clock_rate|cpha|word_width' /system/etc/spi.conf

