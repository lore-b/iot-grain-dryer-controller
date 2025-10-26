// === /res_roof ===
#include "contiki.h"
#include "coap-engine.h"
#include <stdio.h>
#include <string.h>
#include "sys/log.h"

#define LOG_MODULE "RES_ROOF"
#define LOG_LEVEL LOG_LEVEL_INFO

#define MAX_DATA_SIZE 128

static char roof_data[MAX_DATA_SIZE]; // Buffer dati per la risorsa roof
extern int roof_updated; // Flag per indicare se la risorsa è stata aggiornata
static int parse = 0;

extern void try_regression(); // Funzione per avviare la regressione e inviare i dati al server
extern int parse_roof(const char *json_str);

// GET
void res_roof_get_handler(coap_message_t *request, coap_message_t *response,
                          uint8_t *buffer, uint16_t buffer_size, int32_t *offset) {
  int len = snprintf((char *)buffer, buffer_size, "%s", roof_data);
  coap_set_header_content_format(response, APPLICATION_JSON);
  coap_set_payload(response, buffer, len);
}


// PUT
void res_roof_put_handler(coap_message_t *request, coap_message_t *response,
                           uint8_t *buffer, uint16_t buffer_size, int32_t *offset) {
  const uint8_t *payload = NULL;
  size_t len = coap_get_payload(request, &payload);
  if (len > 0 && len < sizeof(roof_data)) {
    memcpy(roof_data, payload, len);
    roof_data[len] = '\0';
    LOG_INFO("Ricevuto PUT su /res_roof: %s\n", roof_data);

    parse = parse_roof(roof_data);

    if(parse==1){
    coap_set_status_code(response, CHANGED_2_04);
    roof_updated = 1; // Ho ricevuto dati validi, aggiorno flag
    try_regression();
    }else{
      coap_set_status_code(response, BAD_REQUEST_4_00);
    }
  } else {
    coap_set_status_code(response, BAD_REQUEST_4_00);
  }
}

RESOURCE(res_roof,
         "title=\"Roof Sensor\";rt=\"application/json\"",
         res_roof_get_handler,
         NULL,
         res_roof_put_handler,
         NULL);
