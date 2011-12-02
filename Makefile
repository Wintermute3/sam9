all: sam9boot

sam9boot: sam9boot.c Makefile
	g++ $@.c -o $@
	cp sam9boot ~

clean:
	@rm -f sam9boot

