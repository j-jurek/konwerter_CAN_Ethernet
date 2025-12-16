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
#define RECONNECT_DELAY 500

/* TCP server states */
typedef enum {
	ES_CLIENT_NONE = 0,
	ES_CLIENT_CONNECTING,
	ES_CLIENT_CONNECTED,
	ES_CLIENT_CLOSING
}client_states;

/* structure for maintaining connection info */
struct client_struct
{
	uint8_t state;
	struct tcp_pcb *pcb;
	struct pbuf *p_tx;
};

/* global pointer to active TCP server (for CAN->TCP) */
static struct tcp_pcb *client_pcb = NULL;
static uint32_t last_reconnect_time = 0;

/* function prototypes */
static void tcp_client_connection_close(struct tcp_pcb *tpcb, struct client_struct *es);
static err_t tcp_client_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err);
static void tcp_client_error(void *arg, err_t err);
static err_t tcp_client_poll(void *arg, struct tcp_pcb *tpcb);
static err_t tcp_client_sent(void *arg, struct tcp_pcb *tpcb, u16_t len);
static err_t tcp_client_connected(void *arg, struct tcp_pcb *tpcb, err_t err);

/* Initialize TCP client */
void tcp_client_init(void)
{
	if (client_pcb != NULL)
	        return;
  struct client_struct *es = NULL;
  ip_addr_t DestIPaddr;

  client_pcb = tcp_new();
  if (client_pcb != NULL)
  {
    es = (struct client_struct *)mem_malloc(sizeof(struct client_struct));

    if (es != NULL)
    {
      es->state = ES_CLIENT_CONNECTING;
      es->pcb = client_pcb;
      es->p_tx = NULL;

      tcp_arg(client_pcb, es);

      IP_ADDR4(&DestIPaddr, DEST_IP_ADDR0, DEST_IP_ADDR1, DEST_IP_ADDR2, DEST_IP_ADDR3);

      err_t ret = tcp_connect(client_pcb, &DestIPaddr, DEST_PORT, tcp_client_connected);

      if (ret != ERR_OK)
      {
         memp_free(MEMP_TCP_PCB, client_pcb);
         mem_free(es);
      }
    }
    else
    {
      memp_free(MEMP_TCP_PCB, client_pcb);
    }
  }
}
/**
  * check and reconnect
  */
void tcp_client_loop(void)
{
    //NULL -> not connected
    if (client_pcb == NULL)
    {
        uint32_t now = HAL_GetTick();
        if (now - last_reconnect_time > RECONNECT_DELAY)
        {
            tcp_client_init(); // reconnect
            last_reconnect_time = now;
        }
    }
}

  /* tcp_connected callback */
static err_t tcp_client_connected(void *arg, struct tcp_pcb *tpcb, err_t err)
{
  struct client_struct *es = (struct client_struct *)arg;

  if (err == ERR_OK)
  {
    es->state = ES_CLIENT_CONNECTED;

    tcp_recv(tpcb, tcp_client_recv);
    tcp_sent(tpcb, tcp_client_sent);
    tcp_poll(tpcb, tcp_client_poll, 1); 
    tcp_err(tpcb, tcp_client_error);

    extern struct tcp_pcb *active_tcp_pcb;
    active_tcp_pcb = tpcb;

    return ERR_OK;
  }
  else
  {
    tcp_client_connection_close(tpcb, es);
  }
  return err;
}

/* tcp_recv callback */
static err_t tcp_client_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err)
{
  struct client_struct *es = (struct client_struct *)arg;

  if (p == NULL)
  {
    es->state = ES_CLIENT_CLOSING;
    tcp_client_connection_close(tpcb, es);
    return ERR_OK;
  }

  if (err != ERR_OK)
  {
    if (p != NULL) pbuf_free(p);
    return err;
  }

  if (es->state == ES_CLIENT_CONNECTED)
  {
    handle_tcp_received(tpcb, (uint8_t*)p->payload, p->tot_len);

    tcp_recved(tpcb, p->tot_len);

    pbuf_free(p);
  }
  else
  {
    tcp_recved(tpcb, p->tot_len);
    pbuf_free(p);
  }

  return ERR_OK;
}

/* tcp_err callback */
static void tcp_client_error(void *arg, err_t err)
{
  struct client_struct *es = (struct client_struct *)arg;

  extern struct tcp_pcb *active_tcp_pcb;
  if (active_tcp_pcb == es->pcb) {
      active_tcp_pcb = NULL; //drop connection
  }

  if (es != NULL)
  {
    mem_free(es);
  }

  client_pcb = NULL;
}

/* tcp_poll callback */
static err_t tcp_client_poll(void *arg, struct tcp_pcb *tpcb)
{
  struct client_struct *es = (struct client_struct *)arg;

  if (es == NULL) {
      tcp_abort(tpcb);
      return ERR_ABRT;
  }

  return ERR_OK;
}

/* tcp_sent callback */
static err_t tcp_client_sent(void *arg, struct tcp_pcb *tpcb, u16_t len)
{
  return ERR_OK;
}

/* Close TCP connection */
static void tcp_client_connection_close(struct tcp_pcb *tpcb, struct client_struct *es)
{
  extern struct tcp_pcb *active_tcp_pcb;
  if (active_tcp_pcb == tpcb) {
      active_tcp_pcb = NULL;
  }

  tcp_arg(tpcb, NULL);
  tcp_sent(tpcb, NULL);
  tcp_recv(tpcb, NULL);
  tcp_err(tpcb, NULL);
  tcp_poll(tpcb, NULL, 0);

  if (es != NULL)
  {
    mem_free(es);
  }

  tcp_close(tpcb);
  client_pcb = NULL;
}
