.PHONY: nbdkit valgrind

all: nbd

nbdkit:
	(cd nbdkit; autoreconf -i; ./configure; make -j)

nbd:
	g++ -fPIC -shared -I nbdkit/include -DFTL_DEBUG=1 -g -o0 -o nbdftl.so nbdftl.cpp 

nbdserver:
	nbdkit/nbdkit -fv ./nbdftl.so

nbdtest:
	sudo nbd-client localhost /dev/nbd9
	sudo fio nbd.fio
	sudo nbd-client -d /dev/nbd9

clean:
	rm -f nbdftl.so lba.bin flash.bin

valgrind:
	g++ -g -o0 -o valgrindtest valgrindtest.cpp
	valgrind  --leak-check=full --track-origins=yes --error-limit=no --show-leak-kinds=all --error-exitcode=999 --tool=memcheck ./valgrindtest 666

statictest:
	g++ -g -o0 -o staticwearleveltest staticwearleveltest.cpp
	./staticwearleveltest

# Delta-journal crash-recovery / durability tests. SPIFTL_RAM_STRICT makes the
# RAM flash emulator model real NOR (erase -> 0xFF, program can only clear
# bits) so a "program over a non-erased page" bug is caught.
journaltest:
	g++ -std=c++17 -g -O0 -DSPIFTL_RAM_STRICT -o journaltest journaltest.cpp
	./journaltest
