# Aeolia PCI devices
# LISTED IN INIT ORDER \o/ ...  ._.
# N.B. this driver must be init'd before pretty much everything, but actual dependants are:
# drivers/net/ethernet/marvell/sky2 (implements ps4-gbe)
obj-y += sb-aeolia.o \
    sb-baikal.o \
    sb-uart.o \
    sb-icc.o \
    sb-icc-pwrbutton.o \
    sb-icc-i2c.o
obj-$(CONFIG_RTC_CLASS) += sb-rtc.o
obj-y += ps4-led.o
obj-y += ps4-fan.o
obj-y += ps4-buzzer.o
obj-$(CONFIG_PS4_SFLASH) += sb-sflash.o
obj-$(CONFIG_PS4_TRNG) += ps4-trng.o
obj-$(CONFIG_PS4_CCP) += ps4-ccp.o