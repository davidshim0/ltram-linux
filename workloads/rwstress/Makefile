CC = gcc
CFLAGS = -O2 -static
TARGET = ltram_rw_stress

all: $(TARGET)

$(TARGET): $(TARGET).c
	$(CC) $(CFLAGS) $(TARGET).c -o $(TARGET)

clean:
	rm -f $(TARGET)
