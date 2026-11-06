# Aeolia PCI devices
# LISTED IN INIT ORDER \o/ ...  ._.
# N.B. this driver must be init'd before pretty much everything, but actual dependants are:
# drivers/net/ethernet/marvell/sky2 (implements ps4-gbe)
obj-y += ps4-apcie.o \
    ps4-apcie-uart.o \
    ps4-apcie-icc.o \
    ps4-apcie-pwrbutton.o \
    icc/i2c.o
obj-y += ps4-bpcie.o \
    ps4-bpcie-uart.o \
    ps4-bpcie-icc.o
obj-$(CONFIG_RTC_CLASS) += ps4-bpcie-rtc.o
obj-y += ps4-led.o
obj-y += ps4-fan.o
obj-y += ps4-buzzer.o
obj-$(CONFIG_RTC_CLASS) += ps4-rtc.o
obj-y += ps4-sflash.o
obj-$(CONFIG_PS4_TRNG) += ps4-trng.o
obj-$(CONFIG_PS4_CCP) += ps4-ccp.o