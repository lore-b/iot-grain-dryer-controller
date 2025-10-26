// === /res_furnace ===
#include "contiki.h"
#include "coap-engine.h"
#include <stdio.h>
#include <string.h>
#include "sys/log.h"


#define LOG_MODULE "RES_FURNACE"
#define LOG_LEVEL LOG_LEVEL_INFO

int furnace_state; // 1 = ON, 0 = OFF
extern void set_furnace(int new_state);
static int parse = 0;
coap_resource_t res_furnace;

// Funzione per fare il parsing del JSON ricevuto cercando il campo "furnace_state"
static int parse_furnace(const char *json_str, int *new_state){
  int matched = sscanf(json_str, "{\"furnace_state\": %d}", new_state);
  LOG_INFO("Ho fatto parsing, matched = %d\n", matched);
  return (matched == 1);  // ritorna 1 se è andato bene
}

// GET
static void res_furnace_get_handler(coap_message_t *request, coap_message_t *response,
                      uint8_t *buffer, uint16_t preferred_size, int32_t *offset) {

  LOG_INFO("[RES_FURNACE] Ricevuto GET su /res_furnace: %d\n", furnace_state);                      
  int len = snprintf((char *)buffer, preferred_size, "{\"furnace_state\":%d}", furnace_state);
  coap_set_header_content_format(response, APPLICATION_JSON);
  coap_set_payload(response, buffer, len);
}

// PUT
static void res_furnace_put_handler(coap_message_t *request, coap_message_t *response,
                      uint8_t *buffer, uint16_t preferred_size, int32_t *offset) {
  size_t len = coap_get_payload(request, (const uint8_t **)&buffer);

    if(len > 0) {
        int new_state;
        parse = parse_furnace((const char *)buffer, &new_state);
        if(parse && (new_state == 0 || new_state == 1)){
          LOG_INFO("[RES_FURNACE] Ricevuto PUT su /res_furnace: %d\n", new_state);

          // Solo se lo stato è cambiato cambia led e notifica observers
          if(new_state != furnace_state){
              LOG_INFO("[RES_FURNACE] Cambio stato: %d → %d\n", furnace_state, new_state);
              set_furnace(new_state);
              furnace_state = new_state;
              LOG_INFO("Notifico!\n");
              coap_notify_observers(&res_furnace);
          }
        
        coap_set_status_code(response, CHANGED_2_04);
        } else {
        LOG_WARN("[RES_FURNACE] Payload non valido: %s\n", buffer);
        coap_set_status_code(response, BAD_REQUEST_4_00);
        }
    } else {
        LOG_WARN("[RES_FURNACE] Payload mancante\n");
        coap_set_status_code(response, BAD_REQUEST_4_00);
    }
}


RESOURCE(res_furnace,
         "title=\"Furnace State\";obs;rt=\"int\"",
         res_furnace_get_handler,
         NULL,
         res_furnace_put_handler,
         NULL);