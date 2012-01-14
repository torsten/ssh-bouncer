TARGET = ssh-bouncer

CFLAGS   = -std=c99 -pipe -Wall -g -O2

SRC_FILES = $(wildcard *.c)
O_FILES   = $(SRC_FILES:%.c=%.o)


.PHONY: all clean

all: $(TARGET)

clean:
	rm -f $(O_FILES) $(TARGET)

$(TARGET): $(O_FILES)
	$(CC) -o $@ $(O_FILES) $(LIBS)
