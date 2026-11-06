#include <linux/serial_8250.h>
#include <linux/serial_core.h>
#include <linux/serial_reg.h>

#include "ps4sb.h"
#ifdef CONFIG_SERIAL_8250
int ps4sb_uart_init(struct ps4sb_dev *sc)
{
	const struct ps4sb_family *f = sc->family;
	struct uart_8250_port uart;
	int i;

	for (i = 0; i < sc->family->uart_count; i++) {
		sc->serial_line[i] = -1;
	}

	for (i = 0; i < sc->family->uart_count; i++) {
		u32 off = f->uart_base + (i << 12);
		memset(&uart, 0, sizeof(uart));
		uart.port.irq		= ps4sb_irqnum(sc, f->uart_subfunc + i);
		uart.port.uartclk	= f->uart_clock;
		uart.port.flags		= UPF_SHARE_IRQ;
		uart.port.iotype	= UPIO_MEM32;
		uart.port.mapbase	= pci_resource_start(sc->pdev, f->uart_bar) + off;
		uart.port.membase	= ps4sb_bar(sc, f->uart_bar) + off;
		uart.port.regshift	= 2;
		uart.port.dev		= &sc->pdev->dev;

		sc->serial_line[i] = serial8250_register_8250_port(&uart);
		if (sc->serial_line[i] < 0) {
			sc_err("Failed to register serial port %d\n", i);
			ps4sb_uart_remove(sc);
			return -EIO;
		}
	}
	return 0;
}

void ps4sb_uart_remove(struct ps4sb_dev *sc)
{
	int i;
	for (i = 0; i < sc->family->uart_count; i++) {
		if (sc->serial_line[i] >= 0) {
			serial8250_unregister_port(sc->serial_line[i]);
			sc->serial_line[i] = -1;
		}
	}
}

#ifdef CONFIG_PM
void ps4sb_uart_suspend(struct ps4sb_dev *sc, pm_message_t state)
{
	int i;
	for (i = 0; i < sc->family->uart_count; i++)
		if (sc->serial_line[i] >= 0)
			serial8250_suspend_port(sc->serial_line[i]);
}

void ps4sb_uart_resume(struct ps4sb_dev *sc)
{
	int i;
	for (i = 0; i < sc->family->uart_count; i++)
		if (sc->serial_line[i] >= 0)
			serial8250_resume_port(sc->serial_line[i]);
}
#endif

#else
int ps4sb_uart_init(struct ps4sb_dev *sc)
{
	return 0;
}

void ps4sb_uart_remove(struct ps4sb_dev *sc)
{
}

#ifdef CONFIG_PM
void ps4sb_uart_suspend(struct ps4sb_dev *sc, pm_message_t state)
{
}

void ps4sb_uart_resume(struct ps4sb_dev *sc)
{
}
#endif
#endif
