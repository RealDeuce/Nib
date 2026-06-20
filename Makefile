CC = cc
BISON = bison
FLEX = flex
CFLAGS = -Wall -g

all: nib nibasm nibdis nibbind nibbuild

nib.tab.c nib.tab.h: nib.y
	$(BISON) -d -v nib.y

lex.yy.c: nib.l nib.tab.h
	$(FLEX) nib.l

nib: nib.tab.c lex.yy.c ast.h ast.c compile.c compile.h table.h
	$(CC) $(CFLAGS) -o nib nib.tab.c lex.yy.c ast.c compile.c

nibasm: asm.c table.h
	$(CC) $(CFLAGS) -o nibasm asm.c

nibbind: bind.c table.h v20_timing.c v20_timing.h
	$(CC) $(CFLAGS) -o nibbind bind.c v20_timing.c

nibbuild: build.c table.h
	$(CC) $(CFLAGS) -o nibbuild build.c

nibdis: dis.cpp
	c++ $(CFLAGS) -o nibdis dis.cpp

check: nib.y
	$(BISON) -d -v nib.y 2>&1 | tee bison_report.txt
	@echo "---"
	@echo "Conflict summary from nib.output:"
	@head -5 nib.output
	@echo "---"
	@grep -c "conflict" nib.output || echo "No conflicts found"

test: all
	@sh tests/run_tests.sh

clean:
	rm -f nib nibasm nibdis nibbind nibbuild nib.tab.c nib.tab.h lex.yy.c nib.output bison_report.txt

.PHONY: all check test clean
