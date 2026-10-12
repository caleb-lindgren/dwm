# Regression tests and a random-operation fuzz for the patches in dwm.c,
# built with AddressSanitizer and UBSan and run on a private Xvfb display
# with two Xinerama screens. Needs Xvfb; `make -C test SEEDS=300` fuzzes
# longer. -no-pie works around GCC 12's ASan crashing on kernels with
# high mmap randomisation.

include ../config.mk

SEEDS = 20
DISP = :97
TCFLAGS = -std=c99 -Wall -Wno-deprecated-declarations -Wno-unused-function \
	-g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -no-pie \
	-I.. ${INCS} ${CPPFLAGS}

test: harness
	Xvfb ${DISP} +xinerama -screen 0 1280x800x24 -screen 1 1024x768x24 \
		>/dev/null 2>&1 & xvfb=$$!; sleep 1; \
	DISPLAY=${DISP} UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
		LSAN_OPTIONS=suppressions=lsan.supp:print_suppressions=0 \
		./harness ${SEEDS}; status=$$?; \
	kill $$xvfb; exit $$status

harness: harness.c ../dwm.c ../config.h ../drw.c ../util.c
	${CC} ${TCFLAGS} -o $@ harness.c ../drw.c ../util.c ${LIBS}

clean:
	rm -f harness

.PHONY: test clean
