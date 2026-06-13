CC = cc
BISON = bison
FLEX = flex
CFLAGS = -Wall -g

all: nib nibasm nibdis nibbind

nib.tab.c nib.tab.h: nib.y
	$(BISON) -d -v -Wcounterexamples nib.y

lex.yy.c: nib.l nib.tab.h
	$(FLEX) nib.l

nib: nib.tab.c lex.yy.c ast.h ast.c compile.c compile.h
	$(CC) $(CFLAGS) -o nib nib.tab.c lex.yy.c ast.c compile.c

nibasm: asm.c
	$(CC) $(CFLAGS) -o nibasm asm.c

nibbind: bind.c
	$(CC) $(CFLAGS) -o nibbind bind.c

nibdis: dis.cpp
	c++ $(CFLAGS) -o nibdis dis.cpp

check: nib.y
	$(BISON) -d -v -Wcounterexamples nib.y 2>&1 | tee bison_report.txt
	@echo "---"
	@echo "Conflict summary from nib.output:"
	@head -5 nib.output
	@echo "---"
	@grep -c "conflict" nib.output || echo "No conflicts found"

test: nib
	@for f in tests/*.nib; do \
		printf "%-30s " "$$f:"; \
		./nib "$$f" 2>&1; \
	done

clean:
	rm -f nib nibasm nibdis nibbind nib.tab.c nib.tab.h lex.yy.c nib.output bison_report.txt

.PHONY: all check test clean
