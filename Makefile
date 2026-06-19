# EtherNet/IP adapter - build & install (Linux/Ubuntu host)
CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -std=c11
LDFLAGS ?=
SRC      = src/eip_adapter.c
BIN      = eip_adapter

PREFIX  ?= /usr/local
DESTDIR ?=
EDS     ?= Linux_EIP_Adapter.eds
CONFIG  ?= config/adapter.conf

.PHONY: all static clean eds install uninstall

all: $(BIN)

$(BIN): $(SRC) src/eip.h src/device.h
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDFLAGS)

# Generate an EDS file from the sample (or $CONFIG) configuration.
eds: $(BIN)
	./$(BIN) --config $(CONFIG) --write-eds $(EDS)

# Optional static build.
static: $(SRC) src/eip.h src/device.h
	$(CC) $(CFLAGS) -static -o $(BIN) $(SRC) $(LDFLAGS)

# Install the binary, a default config (kept if one already exists) and the
# systemd service. Enable with: sudo systemctl enable --now eip-adapter
install: $(BIN)
	install -Dm755 $(BIN) $(DESTDIR)$(PREFIX)/bin/$(BIN)
	[ -f $(DESTDIR)/etc/eip-adapter/adapter.conf ] || \
		install -Dm644 $(CONFIG) $(DESTDIR)/etc/eip-adapter/adapter.conf
	install -Dm644 packaging/eip-adapter.service \
		$(DESTDIR)/lib/systemd/system/eip-adapter.service

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(BIN)
	rm -f $(DESTDIR)/lib/systemd/system/eip-adapter.service
	@echo "note: /etc/eip-adapter/adapter.conf left in place"

clean:
	rm -f $(BIN)
