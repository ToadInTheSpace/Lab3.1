CC      = gcc
CFLAGS  = -std=c11 -Wall -Wextra -Werror -pedantic \
          -Wno-deprecated-declarations
LDFLAGS = -pthread

TARGET  = main
SRC     = main.c

.PHONY: all clean run

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(SRC) -o $(TARGET) $(LDFLAGS)

run: $(TARGET)
	./$(TARGET) $(ARGS)

clean:
	rm -f $(TARGET)
