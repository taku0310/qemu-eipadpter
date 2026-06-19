# EtherNet/IP adapter & scanner - build & install (Linux/Ubuntu host)
CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -std=c11
LDFLAGS ?=
BIN      = eip_adapter
SCANNER  = eip_scanner

PREFIX  ?= /usr/local
DESTDIR ?=
EDS     ?= Linux_EIP_Adapter.eds
CONFIG  ?= config/adapter.conf

.PHONY: all static clean eds install uninstall

all: $(BIN) $(SCANNER)

$(BIN): src/eip_adapter.c src/eip.h src/device.h
	$(CC) $(CFLAGS) -o $@ src/eip_adapter.c $(LDFLAGS)

$(SCANNER): src/eip_scanner.c src/eip.h
	$(CC) $(CFLAGS) -o $@ src/eip_scanner.c $(LDFLAGS)

# Generate an EDS file from the sample (or $CONFIG) configuration.
eds: $(BIN)
	./$(BIN) --config $(CONFIG) --write-eds $(EDS)

# Optional static build.
static: src/eip_adapter.c src/eip_scanner.c src/eip.h src/device.h
	$(CC) $(CFLAGS) -static -o $(BIN) src/eip_adapter.c $(LDFLAGS)
	$(CC) $(CFLAGS) -static -o $(SCANNER) src/eip_scanner.c $(LDFLAGS)

# Install both binaries, a default config (kept if one already exists) and the
# systemd service. Enable with: sudo systemctl enable --now eip-adapter
install: $(BIN) $(SCANNER)
	install -Dm755 $(BIN) $(DESTDIR)$(PREFIX)/bin/$(BIN)
	install -Dm755 $(SCANNER) $(DESTDIR)$(PREFIX)/bin/$(SCANNER)
	[ -f $(DESTDIR)/etc/eip-adapter/adapter.conf ] || \
		install -Dm644 $(CONFIG) $(DESTDIR)/etc/eip-adapter/adapter.conf
	install -Dm644 packaging/eip-adapter.service \
		$(DESTDIR)/lib/systemd/system/eip-adapter.service

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(BIN) $(DESTDIR)$(PREFIX)/bin/$(SCANNER)
	rm -f $(DESTDIR)/lib/systemd/system/eip-adapter.service
	@echo "note: /etc/eip-adapter/adapter.conf left in place"

clean:
	rm -f $(BIN) $(SCANNER)
