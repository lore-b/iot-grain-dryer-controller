CONTIKI_PROJECT = coap-furnace

all: $(CONTIKI_PROJECT)

# Do not try to build on Sky because of code size limitation
PLATFORMS_EXCLUDE = sky z1

# Include emlearn
MODULES_REL += /home/iot_ubuntu_intel/.local/lib/python3.10/site-packages/emlearn ./resources

TARGET_LIBFILES += -lm

INC += /home/iot_ubuntu_intel/.local/lib/python3.10/site-packages/emlearn

CONTIKI=/home/iot_ubuntu_intel/contiki-ng

# Include the CoAP implementation
include $(CONTIKI)/Makefile.dir-variables
MODULES += $(CONTIKI_NG_APP_LAYER_DIR)/coap

include $(CONTIKI)/Makefile.include
