# EtherNet/IP adapter - build
CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -std=c11
LDFLAGS ?=
SRC      = src/eip_adapter.c
BIN      = eip_adapter

EDS     ?= QEMU_EIP_Adapter.eds
CONFIG  ?= config/adapter.conf

.PHONY: all static clean eds

all: $(BIN)

$(BIN): $(SRC) src/eip.h src/device.h
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDFLAGS)

# Generate an EDS file from the sample (or $CONFIG) configuration.
eds: $(BIN)
	./$(BIN) --config $(CONFIG) --write-eds $(EDS)

# Static binary, convenient for dropping into a QEMU initramfs / guest rootfs.
static: $(SRC) src/eip.h src/device.h
	$(CC) $(CFLAGS) -static -o $(BIN) $(SRC) $(LDFLAGS)

clean:
	rm -f $(BIN)
