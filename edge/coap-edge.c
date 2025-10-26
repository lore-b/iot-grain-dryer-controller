#include "contiki.h"
#include "coap-engine.h"
#include "contiki-net.h"
#include "coap-blocking-api.h"
#include "sys/log.h"
#include "sys/etimer.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "os/dev/leds.h"
#include "os/dev/button-hal.h"
#include "net/netstack.h"
#include "net/routing/routing.h"
#include "net/ipv6/uiplib.h"
#include "coap-observe-client.h"

#include "prediction_next_power.h"

#define LOG_MODULE "NodeEdge"
#define LOG_LEVEL LOG_LEVEL_INFO

#define SERVER_EP "coap://[fd00::1]"
#define FEATURE_COUNT 5
#define SEARCH_RES 4 // cambia in 5 con soglia

static coap_endpoint_t server_ep, data_ep, pred_ep, furnace_ep, alarm_ep; // 
static coap_endpoint_t *current_lookup_target = NULL; // puntatore alla destinazione corrente per lookup
static coap_message_t request[1];
static struct etimer missing_timer, wait_timer; // Timer per ricerca root iniziale e per gestione arrivo mancato dei dati
static int timer_running = 0; // flag per sapere se il timer attesa dati è attivo
static char target_ip[64] = "";  // ip del target trovato
static char json_buf[180];
static char timestamp[32];
static char server_time[32]; // real time ricevuto dal server, sempre in formato UNIX epoch
const char *res_list[] = {"/res_data", "/res_prediction","/res_furnace", "/res_alarm"}; // risorse da cercare 

coap_endpoint_t *endpoints[] = {&data_ep, &pred_ep, &furnace_ep, &alarm_ep}; // ip dei nodi aventi le risorse cercate (da inizializzare)  

// Variabili per dati
static int solar=0, temperature=0, humidity=0, power=1000;
static int mese = 1, ora = 0;
static int nextSolar = 0;
static int nextPower = 0;
int missing = 0;
int roof_updated = 0, power_updated = 0;   // Flags per aggiornamenti risorse
process_event_t ev_post_update; // Event per inviare i dati al server
process_event_t start_missing_timer; // Event per avviare il timer di attesa dei dati mancanti

// Variabili per attuatori
static int alarm_state = 0;
static int alarm_change = 0;

static int furnace_state = 0;
static int furnace_change = 0;

extern int threshold_on;
extern int threshold_off;
extern int auto_furnace_ctrl; // Flag per controllo automatico della furnace

// Iteratori
int i;
int attempts = 0;

PROCESS(node_edge_process, "Node Edge - ML & Aggregator");
AUTOSTART_PROCESSES(&node_edge_process);

// === Risorse CoAP ===
extern coap_resource_t res_roof;
extern coap_resource_t res_power;
extern coap_resource_t res_threshold;

// === Funzione per generare timestamp ===
void generate_timestamp(char *buffer, size_t len) {
  clock_time_t now = clock_time();
  unsigned long sec = now / CLOCK_SECOND;
  snprintf(buffer, len, "%lu", sec);  // UNIX timestamp
}

// === Funzioni di parsing dei dati ===
int parse_roof(const char *json_str){
    int matched = sscanf(json_str, "{\"solar\": %d, \"mese\": %d, \"ora\": %d, \"temp\": %d, \"humid\": %d, \"nextSolar\": %d}",
        &solar, &mese, &ora, &temperature, &humidity, &nextSolar);
    //LOG_INFO("Ho fatto parsing e matched = %d\n", matched);
    return (matched == 6);  // ritorna 1 se è andato bene
}

int parse_power(const char *json_str){
  int matched = sscanf(json_str, "{\"power\": %d}", &power);
  //LOG_INFO("Ho fatto parsing e matched = %d\n", matched);
  return (matched == 1);  // ritorna 1 se è andato bene
}

// === Modello ML ===
int predict_next_power() {

  float powerkw = (float)power/100;
  //LOG_INFO("Previsione power: power=%d, powerkw=%d\n", power, (int)powerkw);

  // Dati per la previsione devono essere in float
  float inputs[FEATURE_COUNT] = {powerkw, (float)mese, (float)ora, (float)temperature, (float)humidity};
  float result = 0.0f;
  
  result = prediction_next_power_regress1(inputs, FEATURE_COUNT);

  return result > 0 ? (int)result : 0;
}

// === Funzione per inviare i dati al server quando ho ricevuto sia da Roof Node che da Power Node ===
void predict_and_send(int m){
  missing = m;
  nextPower = predict_next_power();
  nextPower= nextPower * 2;

  // Ottengo timestamp del dongle e lo sommo al real time per ottenere timestamp UNIX
  generate_timestamp(timestamp, sizeof(timestamp));
  unsigned long final_timestamp = strtoul(server_time, NULL, 10);
  
  unsigned long edge_timestamp = strtoul(timestamp, NULL, 10);
  
  final_timestamp += edge_timestamp; 
  
  snprintf(timestamp, sizeof(timestamp), "%lu", final_timestamp);

  //LOG_INFO("Prediction partita con m=%d\n", m);
  LOG_INFO("Prediction eseguita: nextPower=%d, nextSolar=%d, timestamp=%s\n", nextPower, nextSolar, timestamp);

  int energy_diff = nextPower - nextSolar;

  // # Logica per accendere o spegnere la furnace in base ai threshold configurati
  if(auto_furnace_ctrl){
    if(energy_diff <= threshold_on && furnace_state == 0){
      furnace_state = 1;
      furnace_change = 1;
    }
    else if(energy_diff >= threshold_off && furnace_state == 1){
      furnace_state = 0;
      furnace_change = 1;
    }
  }

  int threshold_cut = threshold_off + (threshold_off * 30) / 100; // soglia di rischio per power cut (30% in piu' di threshold_off)
  LOG_INFO("===STATO CORRENTE===: energy_diff=%d, threshold_on=%d, threshold_off=%d, threshold_cut=%d\n", energy_diff, threshold_on, threshold_off, threshold_cut);

  // # Logica per accendere o spegnere l'allarme
  if(energy_diff <= threshold_on && alarm_state != 0){
    alarm_state = 0; // Can turn on Furnace
    alarm_change = 1;
  }
  else if(energy_diff >= threshold_on && energy_diff <= threshold_off && alarm_state != 1){
    alarm_state = 1; // Average Power consumption
    alarm_change = 1;
  }
  else if(energy_diff >= threshold_off && energy_diff <= threshold_cut && alarm_state != 2){
    alarm_state = 2; // Must Shut Furnace
    alarm_change = 1;
  }
  else if(energy_diff > threshold_cut && alarm_state != 3){
    alarm_state = 3; // Power Cut RISK
    alarm_change = 1;
  }
  

  // Imposto flag per inviare i dati al server nel PORCESS_THREAD
  process_post(&node_edge_process, ev_post_update, NULL);
}

// Funzione per controllare se i dati sono stati aggiornati e avviare la regressione
void try_regression() {
  if (roof_updated && power_updated) {
    predict_and_send(0);
    roof_updated = 0;
    power_updated = 0;
    timer_running = 0;
  } else if (!timer_running) {  // Avvio timer se mi mancano i dati
    process_post(&node_edge_process, start_missing_timer, NULL);
    timer_running = 1;
  }
}

// === Handlers per le risorse osservabili ===
void alarm_handler(coap_observee_t *obs,
                   void *notification,
                   coap_notification_flag_t flag)
{
  if(notification == NULL) {
    LOG_WARN("[ALARM_HANDLER] Errore nella Notify\n");
    return;
  }

  coap_message_t *response = (coap_message_t *)notification;
  const uint8_t *chunk = NULL;
  int alarm_value = -1;
  int len = coap_get_payload(response, &chunk);
 
    if(chunk != NULL && len > 0) {
      LOG_INFO("[ALARM_HANDLER] Notifica ricevuta con valore: %.*s\n", len, (char *)chunk);
      if(sscanf((const char *)chunk, "%d", &alarm_value) == 1) {
        LOG_INFO("[ALARM_HANDLER] Modifico mio valore Alarm: %d\n", alarm_value);
        alarm_state = alarm_value;
      } else {
        LOG_WARN("[ALARM_HANDLER] Payload non valido: %.*s\n", len, (char *)chunk);
      }
    } else {
      LOG_WARN("[ALARM_HANDLER] Nessun payload nella Notifica\n");
    }
}

void furnace_handler(coap_observee_t *obs,
                   void *notification,
                   coap_notification_flag_t flag)
{
  if(notification == NULL) {
    LOG_WARN("[FURNACE_HANDLER] Errore nella Notify\n");
    return;
  }

  coap_message_t *response = (coap_message_t *)notification;
  const uint8_t *chunk = NULL;
  int furnace_value = -1;
  int len = coap_get_payload(response, &chunk);
 
    if(chunk != NULL && len > 0) {
      LOG_INFO("[FURNACE_HANDLER] Notifica ricevuta con: %.*s\n", len, (char *)chunk);
      if(sscanf((const char *)chunk, "{\"furnace_state\":%d}", &furnace_value) == 1) {
        LOG_INFO("[FURNACE_HANDLER] Modifico mio valore Furnace: %d\n", furnace_value);
        furnace_state = furnace_value;
      } else {
        LOG_WARN("[FURNACE_HANDLER] Payload non valido: %.*s\n", len, (char *)chunk);
      }
    } else {
      LOG_WARN("[FURNACE_HANDLER] Nessun payload nella Notifica\n");
    }
}

// Handler che gestisce la risposta della richiesta di timestamp
void timestamp_handler(coap_message_t *response) {
  const uint8_t *chunk;

  if (response == NULL) {
    LOG_WARN("Timeout durante la registrazione\n");
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
  //LOG_INFO("Risposta: %.*s\n", len, (char *)chunk);

  if (len > 0) {
    sscanf((char *)chunk, "{\"timestamp\": \"%[^\"]", server_time); 
    LOG_INFO("Timestamp ricevuto dal server: %s\n", server_time);
  } else {
    LOG_WARN("Payload inatteso nella register: %.*s\n", len, (char *)chunk);
  }
}

// Handler che gestisce la risposta della lookup e aggiorna l'endpoint del target
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
  //LOG_INFO("Risposta: %.*s\n", len, (char *)chunk);

  if (len >0 && strstr((char *)chunk, "fd00::") != NULL) {
    sscanf((char *)chunk, "{\"ip\" : \"%[^\"]", target_ip);
    LOG_INFO("Nodo target trovato: [%s]\n", target_ip);
  } else {
    //LOG_WARN("IP non trovato nella risposta\n");
  }

}

// Funzione di callback per gestire la risposta generica dal server
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

// Funzione per accendere o spegnere il led relativo al controllo automatico della furnace
void set_auto_ctrl(){
  if(auto_furnace_ctrl){
    leds_single_on(LEDS_YELLOW); // Auto control ON (Green)
  } else {
    leds_single_off(LEDS_YELLOW); // Auto control OFF
  }
}


// =========== PROCESS THREAD =============

PROCESS_THREAD(node_edge_process, ev, data)
{
  static uip_ipaddr_t dest_ipaddr;
  char ipstr[64]; // temp ip root e server
  char endpoint[128]; // indirizzo server root
  button_hal_button_t *btn;

  PROCESS_BEGIN();

  coap_engine_init();

  start_missing_timer = process_alloc_event();
  ev_post_update = process_alloc_event();

  printf("%p\n", eml_net_activation_function_strs); // This is needed to avoid compiler error (warnings == errors)
  printf("%p\n", eml_error_str); // This is needed to avoid compiler error (warnings == errors)

  leds_single_on(LEDS_YELLOW);

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

  // Inizializzo risorse del nodo
  coap_activate_resource(&res_power, "res_power");
  coap_activate_resource(&res_roof, "res_roof");

  // Risorsa osservabile
  res_threshold.flags |= IS_OBSERVABLE;
  coap_activate_resource(&res_threshold, "res_threshold");
  

   // === 1. REGISTRAZIONE + REGISTRAZIONE RISORSE ===
  snprintf(json_buf, sizeof(json_buf),
           "{\"id\":\"nodoEdge\", \"resources\":[\"/res_power\",\"/res_roof\",\"/res_threshold\"]}");

  coap_init_message(request, COAP_TYPE_CON, COAP_POST, coap_get_mid());
  coap_set_header_uri_path(request, "register");
  coap_set_payload(request, (uint8_t *)json_buf, strlen(json_buf));

  uiplib_ipaddr_snprint(ipstr, sizeof(ipstr), &server_ep.ipaddr);
  LOG_INFO("-> Invio a CoAP server IP: [%s], porta: %u\n", ipstr, uip_ntohs(server_ep.port));

  COAP_BLOCKING_REQUEST(&server_ep, request, response_handler);
  LOG_INFO("Registrazione completata\n");

  leds_on(LEDS_GREEN);

  // Richiesta timestamp alla Register
  coap_init_message(request, COAP_TYPE_CON, COAP_GET, coap_get_mid());
  coap_set_header_uri_path(request, "register");
      
  LOG_INFO("Chiedo il timestamp attuale\n");
  COAP_BLOCKING_REQUEST(&server_ep, request, timestamp_handler);

  // === 2. LOOKUP per risorse ===
  char endpoint_uri[128];
  int found = 0;

  for (i = 0; i < SEARCH_RES; i++) {
    found = 0;
    attempts = 0;

    while (!found) {
      memset(target_ip, 0, sizeof(target_ip));
      current_lookup_target = endpoints[i];     // imposta destinazione della lookup corrente

      coap_init_message(request, COAP_TYPE_CON, COAP_GET, coap_get_mid());
      coap_set_header_uri_path(request, "lookup");
      snprintf(json_buf, sizeof(json_buf), "res=%s", res_list[i]);
      
      LOG_INFO("Cerco Risorsa %s\n", json_buf);
      coap_set_header_uri_query(request, json_buf);
      //LOG_INFO("Prima della COAP e %s, con indice %d\n", res_list[i], i);

      COAP_BLOCKING_REQUEST(&server_ep, request, handle_lookup_response);
      //LOG_INFO("Dopo della COAP e %s\n", res_list[i]);

      if (strlen(target_ip) > 0) {
        // tolte perchè lo faccio nell' handle_lookup_response
        snprintf(endpoint_uri, sizeof(endpoint_uri), "coap://[%s]", target_ip);
        coap_endpoint_parse(endpoint_uri, strlen(endpoint_uri), endpoints[i]);
        found = 1;
      } else {
        LOG_WARN("Lookup fallito per %s, tentativo %d\n", res_list[i], attempts + 1);
        clock_wait(CLOCK_SECOND / 2); // 500ms
        attempts++;
      }
    }

    if (!found) {
      LOG_INFO("Errore: lookup fallito per %s\n", res_list[i]);
    }
  }

  // === 3. OBSERVE RESOURCE ===
  LOG_INFO("Iscrizione alla risorsa res_alarm...\n");
  coap_obs_request_registration(&alarm_ep, "res_alarm", alarm_handler, NULL);

  LOG_INFO("Iscrizione alla risorsa res_furnace...\n");
  coap_obs_request_registration(&furnace_ep, "res_furnace", furnace_handler, NULL);
  
  // === 4. CICLO INFINITO ===
  while(1) {
    PROCESS_WAIT_EVENT();
    //LOG_INFO("Evento ricevuto: %u\n", ev);

    // Gestione evento dati mancanti, avvio timer
    if(ev == start_missing_timer){
      etimer_set(&missing_timer, CLOCK_SECOND * 15);  // timer di 14 secondi 
      LOG_INFO("Timer partito: in attesa dell'altra risorsa...\n");
    }

    // Controllo se ci sono dati da inviare
    if (ev == ev_post_update) {
      leds_off(LEDS_GREEN);
      leds_on(LEDS_BLUE); // inizia a inviare

      /* === POST DATA === */
      LOG_INFO("Invio DATA e PREDICTION al server\n");
      snprintf(json_buf, sizeof(json_buf),
               "{\"ts\":\"%s\",\"sol\":%d,\"mese\":%d,\"ora\":%d,\"temp\":%d,\"hum\":%d,\"pow\":%d}", // gestire float
               timestamp, solar, mese, ora, temperature, humidity, power);
      coap_init_message(request, COAP_TYPE_CON, COAP_POST, coap_get_mid());
      coap_set_header_uri_path(request, "res_data");
      coap_set_payload(request, (uint8_t *)json_buf, strlen(json_buf));
      COAP_BLOCKING_REQUEST(&data_ep, request, response_handler);

      /* === POST PREDICTION === */
      //LOG_INFO("Invio PREDICTION\n");
      snprintf(json_buf, sizeof(json_buf),
               "{\"ts\":\"%s\",\"nPow\":%d,\"nSol\":%d, \"miss\":%d}", // gestire float
               timestamp, nextPower, nextSolar, missing);
      coap_init_message(request, COAP_TYPE_CON, COAP_POST, coap_get_mid());
      coap_set_header_uri_path(request, "res_prediction");
      coap_set_payload(request, (uint8_t *)json_buf, strlen(json_buf));
      COAP_BLOCKING_REQUEST(&pred_ep, request, response_handler);

      /* === POST ALARM e FURNACE === */ //
      if(alarm_change){
        LOG_INFO("MANDO a alarm: %d\n", alarm_state);
        snprintf(json_buf, sizeof(json_buf),
                "{\"alarm_state\":%d}", alarm_state);
        coap_init_message(request, COAP_TYPE_CON, COAP_PUT, coap_get_mid());
        coap_set_header_uri_path(request, "res_alarm");
        coap_set_payload(request, (uint8_t *)json_buf, strlen(json_buf));
        COAP_BLOCKING_REQUEST(&alarm_ep, request, response_handler);
        alarm_change = 0;
      }
      if(furnace_change){
        LOG_INFO("MANDO a furnace: %d\n", furnace_state);
        snprintf(json_buf, sizeof(json_buf),
                "{\"furnace_state\":%d}", furnace_state);
        coap_init_message(request, COAP_TYPE_CON, COAP_PUT, coap_get_mid());
        coap_set_header_uri_path(request, "res_furnace");
        coap_set_payload(request, (uint8_t *)json_buf, strlen(json_buf));
        COAP_BLOCKING_REQUEST(&furnace_ep, request, response_handler);
        furnace_change = 0;
      }
      leds_off(LEDS_BLUE); // Spegnimento LED dopo invio
      leds_on(LEDS_GREEN);
    }

    // Gestione evento dati non ricevuti in tempo
    if (ev == PROCESS_EVENT_TIMER && data == &missing_timer && timer_running) {
      // Timer scaduto, forza regressione
      LOG_INFO("Timeout raggiunto: regressione con dati precedenti\n");
      predict_and_send(1); 

      // reset flags
      roof_updated = 0;
      power_updated = 0;
      timer_running = 0;
    }

    // Bottone premuto per attivare/disattivare controllo automatico della furnace
    if(ev == button_hal_release_event) {
      btn = (button_hal_button_t *)data;

      if(btn != NULL && btn->press_duration_seconds >= 3) {
      //LOG_INFO("Bottone premuto per 3 secondi, auto control  OFF\n");
        if(auto_furnace_ctrl != 0){
          LOG_INFO("AutoControl spento, invio notifica\n");
          auto_furnace_ctrl = 0;
          coap_notify_observers(&res_threshold); 
          set_auto_ctrl();
        }
      } else{
      //LOG_INFO("Bottone premuto, auto control ON\n");
        if(auto_furnace_ctrl != 1){
          LOG_INFO("AutoControl acceso, invio notifica\n");
          auto_furnace_ctrl = 1;
          coap_notify_observers(&res_threshold); 
          set_auto_ctrl();
        }
      }
    }
  }

  PROCESS_END();
}
