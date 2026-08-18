# Define the compiler and the flags you want to use
CC = gcc
CFLAGS = -O3 -static

# Define the name of your final program
TARGET = cow_to_dram

# The default action when you just type 'make'
all: $(TARGET)

# How to build the target (Notice the added -lnuma at the end)
$(TARGET): $(TARGET).c
	$(CC) $(CFLAGS) $(TARGET).c -o $(TARGET) -lnuma

# A cleanup command to easily delete the compiled binary
clean:
	rm -f $(TARGET)
