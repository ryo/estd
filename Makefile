OS!=uname -s
.if ${OS} == "NetBSD"
 LIBS=-lutil
.endif
.if ${OS} == "DragonFly"
 LIBS=-lutil -lkinfo
.endif

default:	all

clean:
	rm -f estd
	rm -f *.core
	rm -f *~

estd:	estd.c
	gcc ${CFLAGS} ${LIBS} -o estd estd.c
	
all: estd

install: all
	install -d -o root -g wheel -m 0755 /usr/local/sbin
	install -s -o root -g wheel -m 0755 estd /usr/local/sbin/estd
	install -d -o root -g wheel -m 0755 /usr/local/man/man1
	install -o root -g wheel -m 0644 estd.1 /usr/local/man/man1/estd.1
