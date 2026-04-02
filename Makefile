CC      = gcc
CFLAGS  = -std=c11 -Wall -Wextra -Wno-unused-parameter -O2 -g \
           $(shell pkg-config --cflags libxml-2.0 libcurl ncurses json-c)
LDFLAGS = $(shell pkg-config --libs   libxml-2.0 libcurl ncurses json-c) \
           -lpthread -lm

TARGET  = foxterm
SRCS    = browser.c fetch.c marionette.c render.c ui.c input.c util.c
OBJS    = $(SRCS:.c=.o)

.PHONY: all clean install

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
	@echo ""
	@echo "  ✓ Built $(TARGET) successfully!"
	@echo "  Run: ./$(TARGET) [URL]"
	@echo ""

%.o: %.c foxterm.h
	$(CC) $(CFLAGS) -c -o $@ $<

install: $(TARGET)
	install -m755 $(TARGET) /usr/local/bin/

clean:
	rm -f $(OBJS) $(TARGET)