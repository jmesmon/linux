include ../scripts/Makefile.include

all: $(OUTPUT)gfp-decode

CC = $(CROSS_COMPILE)gcc
ALL_CFLAGS = $(CFLAGS) -Wall -Wextra -I../../include

$(OUTPUT)gfp-decode: gfp-decode.c
	$(QUIET_CC)$(CC) -o $@ $(ALL_CFLAGS) $(LDFLAGS) $<
clean:
	$(RM) gfp-decode
