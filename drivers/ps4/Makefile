# Aeolia PCI devices
# LISTED IN INIT ORDER \o/ ...  ._.
# N.B. this driver must be init'd before pretty much everything, but actual dependants are:
# drivers/net/ethernet/marvell/sky2 (implements ps4-gbe)
obj-y += ps4-apcie.o \
    ps4-apcie-uart.o \
    ps4-apcie-icc.o \
    ps4-apcie-pwrbutton.o \
    icc/i2c.o
obj-$(CONFIG_X86_PS4_BAIKAL) += ps4-bpcie.o \
    ps4-bpcie-uart.o \
    ps4-bpcie-icc.o
ifdef CONFIG_RTC_CLASS
obj-$(CONFIG_X86_PS4_BAIKAL) += ps4-bpcie-rtc.o
endif
obj-y += ps4-led.o
obj-y += ps4-fan.o
obj-y += ps4-buzzer.o
obj-$(CONFIG_PS4_TRNG) += ps4-trng.o
obj-$(CONFIG_PS4_CCP) += ps4-ccp.o