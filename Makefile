o=8
CFLAGS=-g -Wall -I/usr/local/include
OBJ=main.$o fs.$o event.$o play.$o list.$o
LIBS=-L/usr/local/lib -lixp
prefix=$(HOME)

$o.m9u: $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) $(OBJ) $(LIBS) -o $@

%.$o: %.c m9u.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

install: $o.m9u
	mkdir -p $(prefix)/bin
	install $o.m9u $(prefix)/bin/m9u
	install m9uplay $(prefix)/bin
	install m9utitle $(prefix)/bin

clean:
	rm -f $(OBJ) $o.m9u
