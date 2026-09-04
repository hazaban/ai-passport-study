# Host build: study_audio_codec.c + speex lib + host test (no ESP-IDF)
SP    = /workspace/components/speex
STUDY = /workspace/main/app_study
STUB  = /workspace/host_test

SPEEX_SRCS = \
	$(SP)/libspeex/bits.c \
	$(SP)/libspeex/cb_search.c \
	$(SP)/libspeex/exc_10_16_table.c \
	$(SP)/libspeex/exc_10_32_table.c \
	$(SP)/libspeex/exc_20_32_table.c \
	$(SP)/libspeex/exc_5_256_table.c \
	$(SP)/libspeex/exc_5_64_table.c \
	$(SP)/libspeex/exc_8_128_table.c \
	$(SP)/libspeex/filters.c \
	$(SP)/libspeex/gain_table.c \
	$(SP)/libspeex/gain_table_lbr.c \
	$(SP)/libspeex/hexc_10_32_table.c \
	$(SP)/libspeex/hexc_table.c \
	$(SP)/libspeex/high_lsp_tables.c \
	$(SP)/libspeex/lpc.c \
	$(SP)/libspeex/lsp.c \
	$(SP)/libspeex/lsp_tables_nb.c \
	$(SP)/libspeex/ltp.c \
	$(SP)/libspeex/modes.c \
	$(SP)/libspeex/nb_celp.c \
	$(SP)/libspeex/quant_lsp.c \
	$(SP)/libspeex/speex.c \
	$(SP)/libspeex/speex_callbacks.c \
	$(SP)/libspeex/stereo.c \
	$(SP)/libspeex/vbr.c \
	$(SP)/libspeex/vq.c \
	$(SP)/libspeex/window.c

CFLAGS = -O2 -w \
	-I$(STUB) -I$(SP)/include -I$(SP)/libspeex -I$(STUDY) \
	-DFIXED_POINT -DOUTSIDE_SPEEX -DUSE_SMALLFT -DDISABLE_FLOAT_API \
	-DEXPORT= -DRANDOM_PREFIX=xiph_spx -DUSE_ALLOCA -DHAVE_ALLOCA_H

OBJS = $(patsubst $(SP)/libspeex/%.c,obj/%.o,$(SPEEX_SRCS)) obj/study_audio_codec.o obj/test_rec.o

all: make_dirs test_rec
	./test_rec

make_dirs:
	mkdir -p obj

obj/%.o: $(SP)/libspeex/%.c
	$(CC) $(CFLAGS) -c $< -o $@

obj/study_audio_codec.o: $(STUDY)/study_audio_codec.c
	$(CC) $(CFLAGS) -c $< -o $@

obj/test_rec.o: test_rec.c
	$(CC) $(CFLAGS) -c $< -o $@

test_rec: $(OBJS)
	$(CC) $(OBJS) -lm -o test_rec

clean:
	rm -rf obj test_rec