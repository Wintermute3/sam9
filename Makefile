all: sam9boot

sam9boot: sam9boot.c
	g++ $@.c -o $@

#
