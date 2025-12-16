#include "tcpClient.h"
#include "can_tcp_conv.h"
#include "lwip/tcp.h"
#include "string.h"
#include "main.h"

/* IP config */
#define DEST_IP_ADDR0   192
#define DEST_IP_ADDR1   168
#define DEST_IP_ADDR2   0
#define DEST_IP_ADDR3   164

#define DEST_PORT       29200
#define RECONNECT_DELAY 2000
#define SYN_TIMEOUT     5000

/* TCP client states */
typedef enum {
    ES_CLIENT_NONE = 0,
    ES_CLIENT_CONNECTING,
    ES_CLIENT_CONNECTED,
    ES_CLIENT_CLOSING
} client_states;

/* Client structure */
struct client_struct {
    client_states state;
    struct tcp_pcb *pcb;
    struct pbuf *p_tx;
};

/* Global TCP pointer */
static struct tcp_pcb *client_pcb = NULL;
static struct client_struct *client_es = NULL;

static uint32_t last_reconnect_time = 0;
static uint32_t syn_start_tick = 0;

/* Function prototypes */
static void tcp_client_connection_close(struct tcp_pcb *tpcb, struct client_struct *es);
static err_t tcp_client_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err);
static void tcp_client_error(void *arg, err_t err);
static err_t tcp_client_poll(void *arg, struct tcp_pcb *tpcb);
static err_t tcp_client_sent(void *arg, struct tcp_pcb *tpcb, u16_t len);
static err_t tcp_client_connected(void *arg, struct tcp_pcb *tpcb, err_t err);

/* Initialize TCP client */
void tcp_client_init(void)
{
    if (client_pcb != NULL) return;

    client_pcb = tcp_new();
    if (!client_pcb) return;

    client_es = (struct client_struct *)mem_malloc(sizeof(struct client_struct));
    if (!client_es) {
        tcp_abort(client_pcb);
        client_pcb = NULL;
        return;
    }

    client_es->state = ES_CLIENT_CONNECTING;
    client_es->pcb = client_pcb;
    client_es->p_tx = NULL;

    tcp_arg(client_pcb, client_es);

    ip_addr_t DestIPaddr;
    IP_ADDR4(&DestIPaddr, DEST_IP_ADDR0, DEST_IP_ADDR1, DEST_IP_ADDR2, DEST_IP_ADDR3);

    err_t ret = tcp_connect(client_pcb, &DestIPaddr, DEST_PORT, tcp_client_connected);
    if (ret != ERR_OK) {
        tcp_abort(client_pcb);
        mem_free(client_es);
        client_pcb = NULL;
        client_es = NULL;
    } else {
        syn_start_tick = HAL_GetTick();
    }
}

/* Periodic TCP client loop */
void tcp_client_loop(void)
{
    uint32_t now = HAL_GetTick();

    if (client_es && client_es->state == ES_CLIENT_CONNECTING) {
        // Handle SYN timeout
        if ((now - syn_start_tick) > SYN_TIMEOUT) {
            if (client_pcb) {
                tcp_abort(client_pcb);
                client_pcb = NULL;
            }
            mem_free(client_es);
            client_es = NULL;
            last_reconnect_time = now;
        }
    }

    // Reconnect
    if (!client_pcb && (now - last_reconnect_time) > RECONNECT_DELAY) {
        tcp_client_init();
        last_reconnect_time = now;
    }
}

/* TCP connected callback */
static err_t tcp_client_connected(void *arg, struct tcp_pcb *tpcb, err_t err)
{
    struct client_struct *es = (struct client_struct *)arg;

    if (err == ERR_OK) {
        es->state = ES_CLIENT_CONNECTED;

        tcp_recv(tpcb, tcp_client_recv);
        tcp_sent(tpcb, tcp_client_sent);
        tcp_poll(tpcb, tcp_client_poll, 1);
        tcp_err(tpcb, tcp_client_error);

        extern struct tcp_pcb *active_tcp_pcb;
        active_tcp_pcb = tpcb;

        return ERR_OK;
    }

    tcp_client_connection_close(tpcb, es);
    return err;
}

/* TCP receive callback */
static err_t tcp_client_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err)
{
    struct client_struct *es = (struct client_struct *)arg;

    if (!p) {
        es->state = ES_CLIENT_CLOSING;
        tcp_client_connection_close(tpcb, es);
        return ERR_OK;
    }

    if (err != ERR_OK) {
        pbuf_free(p);
        return err;
    }

    if (es->state == ES_CLIENT_CONNECTED) {
        handle_tcp_received(tpcb, (uint8_t *)p->payload, p->tot_len);
    }

    tcp_recved(tpcb, p->tot_len);
    pbuf_free(p);

    return ERR_OK;
}

/* TCP error callback */
static void tcp_client_error(void *arg, err_t err)
{
    struct client_struct *es = (struct client_struct *)arg;

    extern struct tcp_pcb *active_tcp_pcb;
    if (active_tcp_pcb == es->pcb) active_tcp_pcb = NULL;

    if (es) mem_free(es);
    client_es = NULL;
    client_pcb = NULL;
}

/* TCP poll callback */
static err_t tcp_client_poll(void *arg, struct tcp_pcb *tpcb)
{
    struct client_struct *es = (struct client_struct *)arg;

    if (!es) {
        tcp_abort(tpcb);
        return ERR_ABRT;
    }

    return ERR_OK;
}

/* TCP sent callback */
static err_t tcp_client_sent(void *arg, struct tcp_pcb *tpcb, u16_t len)
{
    return ERR_OK;
}

/* Close TCP connection */
static void tcp_client_connection_close(struct tcp_pcb *tpcb, struct client_struct *es)
{
    extern struct tcp_pcb *active_tcp_pcb;
    if (active_tcp_pcb == tpcb) active_tcp_pcb = NULL;

    tcp_arg(tpcb, NULL);
    tcp_sent(tpcb, NULL);
    tcp_recv(tpcb, NULL);
    tcp_err(tpcb, NULL);
    tcp_poll(tpcb, NULL, 0);

    if (es) mem_free(es);
    client_es = NULL;

    tcp_abort(tpcb);
    client_pcb = NULL;
}
