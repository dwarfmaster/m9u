CFLAGS=-g -Wall -I/usr/local/include
OBJ=main.o fs.o event.o play.o list.o
LIBS=-L/usr/local/lib -lixp
prefix=/usr/local

m9u: $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) $(OBJ) $(LIBS) -o $@

%.o: %.c m9u.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

install: m9u
	mkdir -p $(prefix)/bin
	install m9u $(prefix)/bin/m9u
	install m9play $(prefix)/bin
	install m9title $(prefix)/bin

clean:
	rm -f $(OBJ) m9u
