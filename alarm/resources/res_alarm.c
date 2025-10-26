// === /res_alarm ===
#include "contiki.h"
#include "coap-engine.h"
#include <stdio.h>
#include <string.h>
#include "sys/log.h"


#define LOG_MODULE "RES_ALARM"
#define LOG_LEVEL LOG_LEVEL_INFO

int alarm_state;
extern void set_alarm(int new_state);
static int parse = 0; // flag per controllare il parsing
extern coap_resource_t res_alarm;

static int parse_alarm(const char *json_str, int *new_state){
  int matched = sscanf(json_str, "{\"alarm_state\": %d}", new_state); // Prova a fare il parsing del JSON ricevuto cercando il campo "alarm_state"
  LOG_INFO("Ho fatto parsing, matched = %d\n", matched);
  return (matched == 1);  // ritorna 1 se è andato bene
}

// GET
static void res_alarm_get_handler(coap_message_t *request, coap_message_t *response,
                      uint8_t *buffer, uint16_t preferred_size, int32_t *offset) {

  int len = snprintf((char *)buffer, preferred_size, "%d", alarm_state);
  coap_set_header_content_format(response, TEXT_PLAIN);
  coap_set_payload(response, buffer, len);
}

// PUT
static void res_alarm_put_handler(coap_message_t *request, coap_message_t *response,
                      uint8_t *buffer, uint16_t preferred_size, int32_t *offset) {
  size_t len = coap_get_payload(request, (const uint8_t **)&buffer);

    if(len > 0) {
        int new_state;
        parse = parse_alarm((const char *)buffer, &new_state); // Fa parsing
        if(parse && new_state >= 0 && new_state <= 3){
          LOG_INFO("[RES_ALARM] Ricevuto PUT su /res_alarm: %d\n", new_state);

          // Solo se lo stato è cambiato cambia led e notifica observers
          if(new_state != alarm_state){
              LOG_INFO("[RES_ALARM] Cambio stato: %d → %d\n", alarm_state, new_state); 
              set_alarm(new_state);
              alarm_state = new_state;
              LOG_INFO("Notifico!\n");
              coap_notify_observers(&res_alarm);
          }
        
        coap_set_status_code(response, CHANGED_2_04);
        } else {
        LOG_WARN("[RES_ALARM] Payload non valido: %s\n", buffer);
        coap_set_status_code(response, BAD_REQUEST_4_00);
        }
    } else {
        LOG_WARN("[RES_ALARM] Payload mancante\n");
        coap_set_status_code(response, BAD_REQUEST_4_00);
    }
}


RESOURCE(res_alarm,
         "title=\"Alarm State\";obs;rt=\"int\"",
         res_alarm_get_handler,
         NULL,
         res_alarm_put_handler,
         NULL);
