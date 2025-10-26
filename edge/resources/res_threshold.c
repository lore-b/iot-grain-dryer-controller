#include "contiki.h"
#include "coap-engine.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "sys/log.h"

#define LOG_MODULE "RES_THRESHOLD"
#define LOG_LEVEL LOG_LEVEL_INFO

int threshold_on = 1000; // Soglie di default
int threshold_off = 3000;
int auto_furnace_ctrl = 1; // Controllo automatico della furnace abilitato di default

extern void set_auto_ctrl(); // Funzione che cambia stato di Edge e il led associato

// GET
static void res_get_handler(coap_message_t *request, coap_message_t *response, uint8_t *buffer,
              uint16_t preferred_size, int32_t *offset) {
  int len = snprintf((char *)buffer, preferred_size, "{\"auto_furnace_ctrl\":%d,"
                                                      "\"on_threshold\":%d,"
                                                      "\"off_threshold\":%d}",
                                                      auto_furnace_ctrl, threshold_on, threshold_off);
  coap_set_header_content_format(response, APPLICATION_JSON);
  coap_set_payload(response, buffer, len);
}

// PUT
static void res_put_handler(coap_message_t *request, coap_message_t *response, uint8_t *buffer,
                            uint16_t preferred_size, int32_t *offset) {
  const uint8_t *payload = NULL;
  int len = coap_get_payload(request, &payload);

  if (len > 0 && payload) {
    char json[128];
    memset(json, 0, sizeof(json));
    strncpy(json, (const char *)payload, sizeof(json) - 1);

    // Parsing del JSON cercando  threshold_on, threshold_off e auto_furnace_ctrl se presenti
    if (strstr(json, "threshold_on") != NULL) {
      int new_val;
      if (sscanf(json, "{\"threshold_on\":%d}", &new_val) == 1) {
        threshold_on = new_val;
        LOG_INFO("Updated threshold_on to %d\n", threshold_on);
        coap_set_status_code(response, CHANGED_2_04);
        return;
      }
    }

    if (strstr(json, "threshold_off") != NULL) {
      int new_val;
      if (sscanf(json, "{\"threshold_off\":%d}", &new_val) == 1) {
        threshold_off = new_val;
        LOG_INFO("Updated threshold_off to %d\n", threshold_off);
        coap_set_status_code(response, CHANGED_2_04);
        return;
      }
    }

    if(strstr(json, "auto_furnace_ctrl") != NULL) {
      int new_val;
      if (sscanf(json, "{\"auto_furnace_ctrl\":%d}", &new_val) == 1 && (new_val == 0 || new_val == 1)) {
        auto_furnace_ctrl = new_val;
        set_auto_ctrl(); // Cambia stato della edge e led
        LOG_INFO("Updated auto_furnace_ctrl to %d\n", auto_furnace_ctrl);
        coap_set_status_code(response, CHANGED_2_04);
        return;
      }
    }
    LOG_WARN("[RES_THRESHOLD] Payload non valido: %s\n", json);
    coap_set_status_code(response, BAD_REQUEST_4_00);
  } else {
    LOG_WARN("[RES_THRESHOLD] Payload mancante\n");
    coap_set_status_code(response, BAD_REQUEST_4_00);
  }
}

RESOURCE(res_threshold,
     "title=\"Threshold controller\";obs;rt=\"Control\"",
     res_get_handler, 
     NULL,            
     res_put_handler,
     NULL);           