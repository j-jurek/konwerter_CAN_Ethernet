#include <can_tcp_conv.h>
#include "tcpServer.h"
#include "lwip/tcp.h"
#include <string.h>

/* TCP server states */
enum tcp_server_states
{
    ES_NONE = 0,
    ES_ACCEPTED,
    ES_RECEIVED,
    ES_CLOSING
};

/* structure for maintaining connection info */
struct tcp_server_struct
{
    u8_t state;
    u8_t retries;
    struct tcp_pcb *pcb;
    struct pbuf *p;
};

/* global pointer to active TCP client (for CAN->TCP) */
struct tcp_pcb *tcp_client_pcb = NULL;

/* function prototypes */
static err_t tcp_server_accept(void *arg, struct tcp_pcb *newpcb, err_t err);
static err_t tcp_server_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err);
static void tcp_server_error(void *arg, err_t err);
static err_t tcp_server_poll(void *arg, struct tcp_pcb *tpcb);
static err_t tcp_server_sent(void *arg, struct tcp_pcb *tpcb, u16_t len);
static void tcp_server_send(struct tcp_pcb *tpcb, struct tcp_server_struct *es);
static void tcp_server_connection_close(struct tcp_pcb *tpcb, struct tcp_server_struct *es);
static void tcp_server_handle(struct tcp_pcb *tpcb, struct tcp_server_struct *es);

/* Initialize TCP server */
void tcp_server_init(void)
{
    struct tcp_pcb *tpcb = tcp_new();
    if (!tpcb) return;

    ip_addr_t myIPADDR;
    IP_ADDR4(&myIPADDR, 192, 168, 0, 172);

    if (tcp_bind(tpcb, &myIPADDR, 29200) != ERR_OK)
    {
        memp_free(MEMP_TCP_PCB, tpcb);
        return;
    }

    tpcb = tcp_listen(tpcb);
    tcp_accept(tpcb, tcp_server_accept);
}

/* tcp_accept callback */
static err_t tcp_server_accept(void *arg, struct tcp_pcb *newpcb, err_t err)
{
    LWIP_UNUSED_ARG(arg);
    LWIP_UNUSED_ARG(err);

    tcp_setprio(newpcb, TCP_PRIO_MIN);

    struct tcp_server_struct *es = mem_malloc(sizeof(struct tcp_server_struct));
    if (!es) {
        tcp_close(newpcb);
        return ERR_MEM;
    }

    es->state = ES_ACCEPTED;
    es->pcb = newpcb;
    es->retries = 0;
    es->p = NULL;

    tcp_arg(newpcb, es);
    tcp_recv(newpcb, tcp_server_recv);
    tcp_err(newpcb, tcp_server_error);
    tcp_poll(newpcb, tcp_server_poll, 0);

    active_tcp_pcb = newpcb;

    return ERR_OK;
}

/* tcp_recv callback */
static err_t tcp_server_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err)
{
    struct tcp_server_struct *es = (struct tcp_server_struct*)arg;
    if (!es) return ERR_ARG;

    if (!p) {
        es->state = ES_CLOSING;
        if(!es->p)
            tcp_server_connection_close(tpcb, es);
        else {
            tcp_sent(tpcb, tcp_server_sent);
            tcp_server_send(tpcb, es);
        }
        return ERR_OK;
    }

    if (err != ERR_OK) {
        if(pbuf_free(p) != 0) {}
        es->p = NULL;
        return err;
    }

    if (es->state == ES_ACCEPTED) {
        es->state = ES_RECEIVED;
        es->p = p;
        tcp_sent(tpcb, tcp_server_sent);
        tcp_server_handle(tpcb, es);
        return ERR_OK;
    } else if (es->state == ES_RECEIVED) {
        if(es->p) {
            pbuf_chain(es->p, p);
        } else {
            es->p = p;
        }
        tcp_server_handle(tpcb, es);
        return ERR_OK;
    } else if (es->state == ES_CLOSING) {
        tcp_recved(tpcb, p->tot_len);
        pbuf_free(p);
        return ERR_OK;
    }

    tcp_recved(tpcb, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
}

/* tcp_err callback */
static void tcp_server_error(void *arg, err_t err)
{
    LWIP_UNUSED_ARG(err);
    struct tcp_server_struct *es = (struct tcp_server_struct*)arg;
    if (es) mem_free(es);
}

/* tcp_poll callback */
static err_t tcp_server_poll(void *arg, struct tcp_pcb *tpcb)
{
    struct tcp_server_struct *es = (struct tcp_server_struct*)arg;
    if (!es) {
        tcp_abort(tpcb);
        return ERR_ABRT;
    }

    if (es->p) tcp_server_send(tpcb, es);
    else if(es->state == ES_CLOSING) tcp_server_connection_close(tpcb, es);

    return ERR_OK;
}

/* tcp_sent callback */
static err_t tcp_server_sent(void *arg, struct tcp_pcb *tpcb, u16_t len)
{
    struct tcp_server_struct *es = (struct tcp_server_struct*)arg;
    LWIP_UNUSED_ARG(len);

    es->retries = 0;
    if(es->p) tcp_server_send(tpcb, es);
    else if(es->state == ES_CLOSING) tcp_server_connection_close(tpcb, es);

    return ERR_OK;
}

/* Send data on TCP */
static void tcp_server_send(struct tcp_pcb *tpcb, struct tcp_server_struct *es)
{
    struct pbuf *ptr;
    err_t wr_err = ERR_OK;

    while((wr_err == ERR_OK) && es->p && (es->p->len <= tcp_sndbuf(tpcb))) {
        ptr = es->p;
        wr_err = tcp_write(tpcb, ptr->payload, ptr->len, TCP_WRITE_FLAG_COPY);
        if(wr_err == ERR_OK) {
            u16_t plen = ptr->len;
            es->p = ptr->next;
            if(es->p) pbuf_ref(es->p);
            pbuf_free(ptr);
            tcp_recved(tpcb, plen);
        } else if(wr_err == ERR_MEM) {
            es->p = ptr;
        }
    }
}

/* Close TCP connection */
static void tcp_server_connection_close(struct tcp_pcb *tpcb, struct tcp_server_struct *es)
{
    tcp_arg(tpcb, NULL);
    tcp_sent(tpcb, NULL);
    tcp_recv(tpcb, NULL);
    tcp_err(tpcb, NULL);
    tcp_poll(tpcb, NULL, 0);

    if(es) mem_free(es);
    if(tcp_client_pcb == tpcb) tcp_client_pcb = NULL;

    tcp_close(tpcb);
}

/* Handle received TCP data */
static void tcp_server_handle(struct tcp_pcb *tpcb, struct tcp_server_struct *es)
{
    if(es->p) {
        handle_tcp_received(tpcb, (uint8_t*)es->p->payload, es->p->tot_len);
        pbuf_free(es->p);
        es->p = NULL;
    }

}
