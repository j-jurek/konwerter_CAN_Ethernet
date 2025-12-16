#ifndef CAN_TCP_BRIDGE_H
#define CAN_TCP_BRIDGE_H

#include "stm32f4xx_hal.h"
#include "lwip/tcp.h"
#include <stdint.h>
#include "stats.h"

#define CAN_QUEUE_SIZE 32

extern CAN_HandleTypeDef hcan1;

typedef struct {
    uint8_t ide;
    uint8_t rtr;
    uint32_t id;
    uint8_t dlc;
    uint8_t data[8];
} CANFrame;

typedef struct {
    CANFrame buffer[CAN_QUEUE_SIZE];
    uint8_t head;
    uint8_t tail;
} CANQueue;

extern CANQueue queue_can_rx; // CAN -> TCP
extern CANQueue queue_can_tx; // TCP -> CAN

int queue_push(CANQueue *q, const CANFrame *f);
int queue_pop(CANQueue *q, CANFrame *f);


void CAN1_RX0_IRQ_SaveToQueue(void);

// TCP -> CAN
void handle_tcp_received(struct tcp_pcb *tpcb, const uint8_t *buf, uint16_t len);
void send_can_from_queue(void);

// CAN -> TCP
extern struct tcp_pcb *active_tcp_pcb;
void send_can_over_tcp(void);

#endif
