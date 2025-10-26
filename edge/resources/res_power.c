// === /res_power ===
#include "contiki.h"
#include "coap-engine.h"
#include <stdio.h>
#include <string.h>
#include "sys/log.h"

#define LOG_MODULE "RES_POWER"
#define LOG_LEVEL LOG_LEVEL_INFO

#define MAX_DATA_SIZE 64

static char power_data[MAX_DATA_SIZE]; // Buffer dati per la risorsa power
extern int power_updated; // Flag per indicare se la risorsa è stata aggiornata
static int parse = 0;

extern void try_regression();  // Funzione per avviare la regressione e inviare i dati al server
extern int parse_power(const char *json_str);

// GET
void res_power_get_handler(coap_message_t *request, coap_message_t *response,
                           uint8_t *buffer, uint16_t buffer_size, int32_t *offset) {
  int len = snprintf((char *)buffer, buffer_size, "%s", power_data);
  coap_set_header_content_format(response, APPLICATION_JSON);
  coap_set_payload(response, buffer, len);
}

// PUT
void res_power_put_handler(coap_message_t *request, coap_message_t *response,
                            uint8_t *buffer, uint16_t buffer_size, int32_t *offset) {
  size_t len = coap_get_payload(request, (const uint8_t **)&buffer);
  if (len > 0 && len < MAX_DATA_SIZE) {
    memcpy(power_data, buffer, len);
    power_data[len] = '\0';
    LOG_INFO("Ricevuto PUT su /res_power: %s\n", power_data);

    parse = parse_power(power_data); // Fa parsing

    if(parse==1){
    coap_set_status_code(response, CHANGED_2_04);
    power_updated = 1; // Ho ricevuto dati validi, aggiorno flag
    try_regression();
    }else{
      coap_set_status_code(response, BAD_REQUEST_4_00);
    }

  } else {
    coap_set_status_code(response, BAD_REQUEST_4_00);
  }
}

RESOURCE(res_power,
         "title=\"Power Sensor\";rt=\"application/json\"",
         res_power_get_handler,
         NULL,
         res_power_put_handler,
         NULL);
