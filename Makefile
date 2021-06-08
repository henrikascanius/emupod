# jtload makefile   2004-02-27 HJA

TARGET = emupod

CC     = gcc
CC_OPTS= -O2

ROOT   = .
SRCDIR = $(ROOT)

# Source files:
SRC_C  = $(foreach dir, $(SRCDIR), $(wildcard $(dir)/*.c))
SRC_H  = $(foreach dir, $(SRCDIR), $(wildcard $(dir)/*.h))
SRC    = $(SRC_C) $(SRC_H)

# Object files:
OBJ_C = $(notdir $(patsubst %.c, %.o, $(SRC_C)))
OBJ    = $(OBJ_C)


# Targets:
all:     $(TARGET)

$(TARGET) : $(SRC) $(MAKEFILE)
	$(CC) $(CC_OPTS) -o $(TARGET) $(SRC_C)

$(OBJ_C) : %.o : %.c
	$(CC) $(CC_OPTS) -o $@ $<

CLEANFILES = ./*.o

clean:
	rm -f $(CLEANFILES)





