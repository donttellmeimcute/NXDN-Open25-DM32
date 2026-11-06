/* Host-only double for the real interrupt-port translation unit. */
#ifndef NATIVE_HOST_PLATFORM_H
#define NATIVE_HOST_PLATFORM_H
#include <stdbool.h>
#include <stdint.h>
enum { IRQ_TIMER3=3, IRQ_TIMER6=6, IRQ_MODEM_SYS=29, IRQ_MODEM_TS_TX,
       IRQ_MODEM_TS_RX, IRQ_MODEM_RF_TX, TIMER_PA=3 };
typedef void (*hrc_irq_fn)(void);
extern hrc_irq_fn hrc_irq_table[64];
extern uint32_t mock_pic[2],mock_tmr[7];
extern uint32_t mock_pic_history;
#define PIC_MASK mock_pic[0]
#define PIC_MASK_1 mock_pic[1]
#define PIC_INT_ST_1 mock_pic_history
#define TMR_CTRL(n) mock_tmr[n]
uint32_t hrc_irq_save(void);
void hrc_irq_restore(uint32_t);
void hrc_irq_register(uint8_t,hrc_irq_fn);
void hrc_irq_enable(uint8_t,bool);
void hrc_timer_ack(unsigned);
void hrc_timer_stop(unsigned);
void hrc_timer_periodic(unsigned,uint32_t);
uint32_t hrc_micros(void);
#endif
