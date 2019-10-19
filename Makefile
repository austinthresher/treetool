BIN=tt
SRC=	readline.c \
	exception.c \
	${BIN}.c

CFLAGS=-lncurses --std=c89 -O0

all: ${BIN}

${BIN}: ${SRC}
	cc ${SRC} -o ${BIN} ${CFLAGS}

clean:
	rm -f ${BIN}
