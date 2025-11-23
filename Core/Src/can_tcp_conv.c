#include "can_tcp_conv.h"
#include "string.h"
#include "main.h"

CANQueue queue_can_rx = {0};
CANQueue queue_can_tx = {0};

struct tcp_pcb *active_tcp_pcb = NULL;
volatile uint8_t can_frame_pending = 0; // flaga odebranej ramki CAN

// Kolejka
int queue_push(CANQueue *q, const CANFrame *f) {
    uint8_t next = (q->head + 1) % CAN_QUEUE_SIZE;
    if(next == q->tail) return 0;
    q->buffer[q->head] = *f;
    q->head = next;
    return 1;
}

int queue_pop(CANQueue *q, CANFrame *f) {
    if(q->head == q->tail) return 0;
    *f = q->buffer[q->tail];
    q->tail = (q->tail + 1) % CAN_QUEUE_SIZE;
    return 1;
}

void CAN1_RX0_IRQ_SaveToQueue(void) {
    CAN_RxHeaderTypeDef rxHeader;
    uint8_t rxData[8];

    if(HAL_CAN_GetRxMessage(&hcan1, CAN_RX_FIFO0, &rxHeader, rxData) == HAL_OK) {
        CANFrame f;
        f.ide = (rxHeader.IDE == CAN_ID_EXT) ? 1 : 0;
        f.rtr = (rxHeader.RTR == CAN_RTR_REMOTE) ? 1 : 0;
        f.id  = (rxHeader.IDE == CAN_ID_EXT) ? rxHeader.ExtId : rxHeader.StdId;
        f.dlc = rxHeader.DLC;
        memcpy(f.data, rxData, f.dlc);

        queue_push(&queue_can_rx, &f);
        can_frame_pending = 1;
    }
}

//TCP -> CAN
void handle_tcp_received(struct tcp_pcb *tpcb, const uint8_t *buf, uint16_t len) {
    const uint8_t *p = buf;

    while(len >= 6) {
        CANFrame f;
        uint8_t flags = p[0];
        f.ide = flags & 0x01;
        f.rtr = (flags & 0x02) ? 1 : 0;
        f.id  = (p[1]<<24) | (p[2]<<16) | (p[3]<<8) | p[4];
        f.dlc = p[5];
        if(f.dlc > 8) f.dlc = 8;

        if(len < (6 + f.dlc)) break; // niepelna ramka

        memcpy(f.data, &p[6], f.dlc);
        queue_push(&queue_can_tx, &f);

        p   += (6 + f.dlc);
        len -= (6 + f.dlc);
    }
}

void send_can_from_queue(void) {
    CANFrame f;
    uint32_t mailbox;
    CAN_TxHeaderTypeDef h;

    while(queue_pop(&queue_can_tx, &f)) {
        h.DLC = f.dlc;
        h.TransmitGlobalTime = DISABLE;
        h.IDE = f.ide ? CAN_ID_EXT : CAN_ID_STD;
        h.RTR = f.rtr ? CAN_RTR_REMOTE : CAN_RTR_DATA;
        h.ExtId = f.ide ? f.id : 0;
        h.StdId = f.ide ? 0 : f.id;

        HAL_CAN_AddTxMessage(&hcan1, &h, f.data, &mailbox);
    }
}

// CAN -> TCP
void send_can_over_tcp(void) {
    if(active_tcp_pcb == NULL) return;
    if(!can_frame_pending) return;

    CANFrame f;
    uint8_t buf[14];

    while(queue_pop(&queue_can_rx, &f)) {
        uint8_t flags = 0;
        if(f.ide) flags |= 0x01;
        if(f.rtr) flags |= 0x02;

        buf[0] = flags;
        buf[1] = (f.id >> 24) & 0xFF;
        buf[2] = (f.id >> 16) & 0xFF;
        buf[3] = (f.id >> 8) & 0xFF;
        buf[4] = f.id & 0xFF;
        buf[5] = f.dlc;
        memcpy(&buf[6], f.data, f.dlc);

        tcp_write(active_tcp_pcb, buf, 6 + f.dlc, TCP_WRITE_FLAG_COPY);
        tcp_output(active_tcp_pcb);
    }

    can_frame_pending = 0;
}
