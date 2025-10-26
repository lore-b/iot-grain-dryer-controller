#include "contiki.h"
#include "coap-engine.h"
#include "coap-blocking-api.h"
#include "sys/log.h"
#include "sys/etimer.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "os/dev/leds.h"
#include "net/netstack.h"
#include "net/routing/routing.h"
#include "net/ipv6/uiplib.h"
#include "lib/random.h"

#define LOG_MODULE "NodeAlarm"
#define LOG_LEVEL LOG_LEVEL_INFO

#define SERVER_EP "coap://[fd00::1]:5683"

static coap_endpoint_t server_ep;
static coap_message_t request[1];
static struct etimer wait_timer; // Timer per ricerca root iniziale
extern int alarm_state; // Valore della risorsa res_alarm

static char json_buf[64];

PROCESS(node_alarm_process, "Alarm Actuator Node");
AUTOSTART_PROCESSES(&node_alarm_process);

// === Risorse CoAP ===
extern coap_resource_t res_alarm;

// Cambia colore dei led in base allo stato inviato all'allarme luminoso
void set_alarm(int new_state) {
    leds_off(LEDS_ALL);
    leds_single_on(LEDS_YELLOW);
    if(new_state==0){
      leds_on(LEDS_BLUE); // Best moment to turn furnace ON
    }
    else if(new_state==1){
      leds_on(LEDS_GREEN); // Average power consumption
    }
    else if(new_state==2){ 
      leds_on(LEDS_RED); // Must shut furnace
      
    }
    else if(new_state==3){
      leds_on(LEDS_RED); // Power Cut RISK
      leds_on(LEDS_BLUE); 
    }
}

// Funzione di callback per gestire la risposta dal server
void response_handler(coap_message_t *response){

   if (response == NULL) {
    LOG_INFO("Timeout dal server\n");
    return;
  }

  // Stampa e controlla codice CoAP (es. 2.05, 4.04, ecc.)
  uint8_t class = response->code >> 5;
  uint8_t detail = response->code & 0x1F;
  LOG_INFO("Codice risposta: %u.%02u\n", class, detail);
}

PROCESS_THREAD(node_alarm_process, ev, data)
{
  static uip_ipaddr_t dest_ipaddr;
  char ipstr[64]; // temp ip root e server
  char endpoint[128]; // indirizzo server root

  PROCESS_BEGIN();

  coap_engine_init();

  leds_single_on(LEDS_YELLOW);

  // Inizializzo risorse del nodo
  res_alarm.flags |= IS_OBSERVABLE;
  coap_activate_resource(&res_alarm, "res_alarm");

  // Ricerca del nodo root
  etimer_set(&wait_timer, CLOCK_SECOND);
  while(!NETSTACK_ROUTING.node_is_reachable() ||
      !NETSTACK_ROUTING.get_root_ipaddr(&dest_ipaddr)) {
  LOG_INFO("In attesa del root...\n");
  leds_toggle(LEDS_RED);
  PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&wait_timer));
  etimer_reset(&wait_timer);
  }
  
  leds_off(LEDS_RED);
  
  uiplib_ipaddr_snprint(ipstr, sizeof(ipstr), &dest_ipaddr);
  snprintf(endpoint, sizeof(endpoint), "coap://[%s]:5683", ipstr);
  LOG_INFO("Root trovato: %s\n", endpoint);

  coap_endpoint_parse(SERVER_EP, strlen(SERVER_EP), &server_ep);


   // === 1. REGISTRAZIONE RISORSE ===
  snprintf(json_buf, sizeof(json_buf),
           "{\"id\":\"nodoAlarm\", \"resources\":[\"/res_alarm\"]}");
  
  coap_init_message(request, COAP_TYPE_CON, COAP_POST, coap_get_mid());
  coap_set_header_uri_path(request, "register");
  coap_set_payload(request, (uint8_t *)json_buf, strlen(json_buf));

  uiplib_ipaddr_snprint(ipstr, sizeof(ipstr), &server_ep.ipaddr);
  LOG_INFO("-> Invio a CoaP server IP: [%s], porta: %u\n", ipstr, uip_ntohs(server_ep.port));

  COAP_BLOCKING_REQUEST(&server_ep, request, response_handler);
  LOG_INFO("Registrazione completata\n");

  leds_on(LEDS_BLUE);

  // === 2. CICLO INFINITO ===
  while(1) {
    PROCESS_WAIT_EVENT();
    LOG_INFO("Evento ricevuto: %u\n", ev);
  }

  PROCESS_END();
}
