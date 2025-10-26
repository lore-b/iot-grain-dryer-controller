#include "contiki.h"
#include "coap-engine.h"
#include "coap-blocking-api.h"
#include "net/netstack.h"
#include "net/routing/routing.h"
#include "sys/log.h"
#include "sys/etimer.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "os/dev/leds.h"
#include "prediction_next_solar.h"
#include "net/ipv6/uiplib.h"

#define LOG_MODULE "RoofNode"
#define LOG_LEVEL LOG_LEVEL_INFO

#define SERVER_EP "coap://[fd00::1]"
#define LOOKUP_PATH "lookup?res=/res_roof"
#define FEATURE_COUNT 5

static coap_endpoint_t server_ep, target_ep;
static coap_message_t request[1];
static struct etimer periodic_timer, wait_timer;
static char target_ip[64] = "";  // ip nodo Edge sarà messo qui
static char json_buf[128];
int attempts = 0;

PROCESS(roof_node_process, "Roof Sensor Node");
AUTOSTART_PROCESSES(&roof_node_process);

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

// Handler per gestire la risposta dal root server contenente IP del nodo con la risorsa richiesta
void handle_lookup_response(coap_message_t *response) {
  const uint8_t *chunk;
  if (response == NULL) {
    LOG_INFO("Timeout dal server\n");
    return;
  }

  // Stampo codice risposta
  LOG_INFO("Codice risposta: %u.%02u\n",
           (response->code >> 5), response->code & 0x1F);

  // Errore client o server
  if (response->code >= 128) {
    LOG_WARN("Errore dal server: codice %u\n", response->code);
    return;
  }
  
  int len = coap_get_payload(response, &chunk);
  LOG_INFO("Risposta: %.*s\n", len, (char *)chunk);

  if (strstr((char *)chunk, "fd00::") != NULL) {
    sscanf((char *)chunk, "{\"ip\" : \"%[^\"]", target_ip); // Estrae l'IP dal JSON
    LOG_INFO("Nodo target: [%s]\n", target_ip);
  } else {
    LOG_WARN("IP non trovato nella risposta\n");
  }
}

// Funzione per avviare la regressione del modello di previsione
int predict_next_solar(int solar, int mese, int ora, int temperature, int humidity) {
  float solarkw = (float)solar / 1000; // Converti da W in kW
  // Il modello si aspetta valori Float
  float inputs[FEATURE_COUNT] = {solarkw, (float)mese, (float)ora, (float)temperature, (float)humidity};
  float result = prediction_next_solar_regress1(inputs, FEATURE_COUNT);
  result = result * 100;
  return result > 0 ? (int)result : 0;
}

PROCESS_THREAD(roof_node_process, ev, data)
{
  static int solar = 0, temperature = 18, humidity = 60;
  static int mese = 5, ora = 16, oraPM = 4;

  static uip_ipaddr_t dest_ipaddr;
  char ipstr[64]; // temp per ip root e server
  char endpoint[128]; // indirizzo server root

  PROCESS_BEGIN();

  printf("%p\n", eml_net_activation_function_strs); // This is needed to avoid compiler error (warnings == errors)
  printf("%p\n", eml_error_str); // This is needed to avoid compiler error (warnings == errors)

  leds_single_on(LEDS_YELLOW);

  // Ricerca nodo ROOT
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

  // === 1. REGISTRAZIONE ===
  snprintf(json_buf, sizeof(json_buf),
           "{\"id\":\"nodoRoof\", \"resources\":[\"\"]}");
  
  coap_init_message(request, COAP_TYPE_CON, COAP_POST, coap_get_mid());
  coap_set_header_uri_path(request, "register");
  coap_set_payload(request, (uint8_t *)json_buf, strlen(json_buf));

  uiplib_ipaddr_snprint(ipstr, sizeof(ipstr), &server_ep.ipaddr);
  LOG_INFO("-> Invio a CoAP server IP: [%s], porta: %u\n", ipstr, uip_ntohs(server_ep.port));

  COAP_BLOCKING_REQUEST(&server_ep, request, response_handler);

  LOG_INFO("Registrazione completata\n");

  leds_on(LEDS_GREEN);

  // === 2. LOOKUP ===
  attempts = 1;
  memset(target_ip, 0, sizeof(target_ip));  // reset del buffer

  while (strlen(target_ip) == 0) {
    coap_init_message(request, COAP_TYPE_CON, COAP_GET, coap_get_mid());
    coap_set_header_uri_path(request, "lookup");
    coap_set_header_uri_query(request, "res=/res_roof");
    LOG_INFO("Tentativo %d: richiesta IP nodo con /res_roof...\n", attempts);

    COAP_BLOCKING_REQUEST(&server_ep, request, handle_lookup_response);

    if (strlen(target_ip) == 0) {
      clock_wait(CLOCK_SECOND / 2);  // attesa 500 ms prima di ritentare
      attempts++;
    }
  }

  if (strlen(target_ip) > 0) {
    LOG_INFO("Lookup riuscito: %s\n", target_ip);
  } else {
    LOG_WARN("Lookup fallito\n");
    PROCESS_EXIT();
  }

  // Inizializzo target_ep (edge)
  char endpoint_uri[128];
  snprintf(endpoint_uri, sizeof(endpoint_uri), "coap://[%s]", target_ip);
  coap_endpoint_parse(endpoint_uri, strlen(endpoint_uri), &target_ep);

  uiplib_ipaddr_snprint(ipstr, sizeof(ipstr), &target_ep.ipaddr);
  LOG_INFO("-> Inizializzato target a NodoEdge IP: [%s], porta: %u\n", ipstr, uip_ntohs(target_ep.port));

  // === 3. SEND PERIODICAMENTE DATI ===
  etimer_set(&periodic_timer, CLOCK_SECOND * 15); // Genero dati ogni 15 secondi

  while (1) {
    PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&periodic_timer));

      // Generazione dati da mandare in maniera simulata
      ora++; if(ora > 23) {
          ora = 0;
        }

      oraPM = (ora + 12)%24;
      if (oraPM == 0) {
        mese++; 
        if(mese > 12) mese = 1;
      }
        // Simulazione semplice produzione solare in modo casuale
      if (oraPM >= 8 && oraPM <= 14) {
        solar += (rand() % 1301) - 300;
        if (solar < 0) solar = 0; // Niente valori negativi
      } else {
        solar += -((rand() % 901) + 300); 
        if (solar < 0) solar = 0;
      }
      
      // Simulazione temperatura e umidità casuale
      temperature += (rand() % 5) - 2;
      if (temperature > 28) temperature = 28;
      if (temperature < 10) temperature = 10;
      humidity += (rand() % 21) - 10;
      if (humidity > 90) humidity = 90;
      if (humidity < 30) humidity = 30;
      
      
      
      // Chiamo la funzione di previsione
      int next_solar = predict_next_solar(solar, mese, ora, temperature, humidity);
      //next_solar = next_solar*100;
      LOG_INFO("NextSolar previsto è: %d\n", next_solar);

      // Preparo il JSON da inviare e invio messaggio al nodo Edge
      snprintf(json_buf, sizeof(json_buf),
               "{\"solar\": %d, \"mese\": %d, \"ora\": %d, \"temp\": %d, \"humid\": %d, \"nextSolar\": %d}",
               solar, mese, oraPM, temperature, humidity, next_solar);

      coap_init_message(request, COAP_TYPE_CON, COAP_PUT, coap_get_mid());
      coap_set_header_uri_path(request, "res_roof");
      coap_set_payload(request, (uint8_t *)json_buf, strlen(json_buf));
      LOG_INFO("Invio PUT a /res_roof su Edge: %s\n", json_buf);

      leds_off(LEDS_GREEN);
      leds_on(LEDS_BLUE); // LED BLUE acceso durante l'invio
      COAP_BLOCKING_REQUEST(&target_ep, request, response_handler);
      clock_wait(CLOCK_SECOND);
      leds_off(LEDS_BLUE);  // Spegnimento LED dopo invio
      leds_on(LEDS_GREEN);
    etimer_reset(&periodic_timer);
  }

  PROCESS_END();
}