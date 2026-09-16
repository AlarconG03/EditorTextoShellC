CC = gcc
CFLAGS = -Wall -Wextra -std=gnu99 -g -D_GNU_SOURCE
LDLIBS_TEST = -lutil

TARGET = sys_shell
EDITOR_BIN = cat_editor
TEST_BIN = tests/test_cat_editor

SRCS = main.c cat_datos.c cat_memoria.c cat_monitoreo.c cat_util.c cat_editor_launch.c
OBJS = $(SRCS:.c=.o)

# cat_editor es un binario independiente. El shell lo inicia con
# fork()+execv() desde cat_editor_launch.c mediante el comando e_open.
EDITOR_SRC = cat_editor.c

all: $(TARGET) $(EDITOR_BIN)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS)

$(EDITOR_BIN): $(EDITOR_SRC)
	$(CC) $(CFLAGS) -o $@ $(EDITOR_SRC)

%.o: %.c shell.h
	$(CC) $(CFLAGS) -c $< -o $@

# Suite de pruebas auxiliar POSIX escrita en C. forkpty() permite probar
# tambien el editor de pantalla completa y sus atajos Alt+S/Q/E/G.
$(TEST_BIN): tests/test_cat_editor.c $(EDITOR_BIN) $(TARGET)
	$(CC) $(CFLAGS) -I. -o $@ tests/test_cat_editor.c $(LDLIBS_TEST)

test: all $(TEST_BIN)
	./$(TEST_BIN)

clean:
	rm -f $(TARGET) $(OBJS) $(EDITOR_BIN) $(TEST_BIN)

.PHONY: all test clean
