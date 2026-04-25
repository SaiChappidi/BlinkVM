CC ?= gcc
CFLAGS ?= -O2 -g -Wall -Wextra -Wpedantic -std=c11
LDFLAGS ?=

INCLUDES := -Iinclude
SRCS := src/main.c src/kvm_vm.c src/log.c
OBJS := $(SRCS:.c=.o)
BIN := microvm

.PHONY: all clean run

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) $(OBJS) -o $@ $(LDFLAGS)

src/%.o: src/%.c
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

run: $(BIN)
	./$(BIN) run-demo

run-demo: $(BIN)
	./$(BIN) run-demo

run-linux: $(BIN)
	@if [ -z "$(KERNEL)" ]; then \
		echo "Usage: make run-linux KERNEL=/path/to/bzImage [INITRD=/path/to/initrd] [MEM_MIB=256] [CMDLINE='console=ttyS0 ...']"; \
		exit 1; \
	fi
	./$(BIN) run-linux --kernel "$(KERNEL)" $(if $(INITRD),--initrd "$(INITRD)",) --mem-mib $(or $(MEM_MIB),256) --cmdline "$(or $(CMDLINE),console=ttyS0 reboot=k panic=1 pci=off)"

clean:
	rm -f $(OBJS) $(BIN)
