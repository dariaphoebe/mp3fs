#include <stdio.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <chromaprint.h>
#ifdef _WIN32
#include <windows.h>
#endif

#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))

#define BUFFER_SIZE (AVCODEC_MAX_AUDIO_FRAME_SIZE * 2)

int decode_audio_file(ChromaprintContext *chromaprint_ctx, int16_t *buffer, const char *file_name, int max_length, int *duration)
{
	int i, ok = 0, remaining, length, consumed, buffer_size, codec_ctx_opened = 0;
	AVFormatContext *format_ctx = NULL;
	AVCodecContext *codec_ctx = NULL;
	AVCodec *codec = NULL;
	AVStream *stream = NULL;
	AVPacket packet, packet_temp;

	if (av_open_input_file(&format_ctx, file_name, NULL, 0, NULL) != 0) {
		fprintf(stderr, "ERROR: couldn't open the file\n");
		goto done;
	}

	if (av_find_stream_info(format_ctx) < 0) {
		fprintf(stderr, "ERROR: couldn't find stream information in the file\n");
		goto done;
	}

	for (i = 0; i < format_ctx->nb_streams; i++) {
		codec_ctx = format_ctx->streams[i]->codec;
#if LIBAVCODEC_VERSION_INT <= AV_VERSION_INT(52, 20, 0)
		if (codec_ctx && codec_ctx->codec_type == CODEC_TYPE_AUDIO) {
#else
		if (codec_ctx && codec_ctx->codec_type == AVMEDIA_TYPE_AUDIO) {
#endif
			stream = format_ctx->streams[i];
			break;
		}
	}
	if (!stream) {
		fprintf(stderr, "ERROR: couldn't find any audio stream in the file\n");
		goto done;
	}

	codec = avcodec_find_decoder(codec_ctx->codec_id);
	if (!codec) {
		fprintf(stderr, "ERROR: unknown codec\n");
		goto done;
	}

	if (avcodec_open(codec_ctx, codec) < 0) {
		fprintf(stderr, "ERROR: couldn't open the codec\n");
		goto done;
	}
	codec_ctx_opened = 1;

	if (codec_ctx->sample_fmt != SAMPLE_FMT_S16) {
		fprintf(stderr, "ERROR: unsupported sample format\n");
		goto done;
	}

	if (codec_ctx->channels <= 0) {
		fprintf(stderr, "ERROR: no channels found in the audio stream\n");
		goto done;
	}

	*duration = stream->time_base.num * stream->duration / stream->time_base.den;

	av_init_packet(&packet);
	av_init_packet(&packet_temp);

	remaining = max_length * codec_ctx->channels * codec_ctx->sample_rate;
	chromaprint_start(chromaprint_ctx, codec_ctx->sample_rate, codec_ctx->channels);

	while (1) {
		if (av_read_frame(format_ctx, &packet) < 0) {
			break;
		}

		packet_temp.data = packet.data;
		packet_temp.size = packet.size;

		while (packet_temp.size > 0) {
			buffer_size = BUFFER_SIZE;
#if LIBAVCODEC_VERSION_INT <= AV_VERSION_INT(52, 20, 0)
			consumed = avcodec_decode_audio2(codec_ctx,
				buffer, &buffer_size, packet_temp.data, packet_temp.size);
#else
			consumed = avcodec_decode_audio3(codec_ctx,
				buffer, &buffer_size, &packet_temp);
#endif

			if (consumed < 0) {
				break;
			}

			packet_temp.data += consumed;
			packet_temp.size -= consumed;

			if (buffer_size <= 0) {
				if (buffer_size < 0) {
					fprintf(stderr, "WARNING: size returned from avcodec_decode_audioX is too small\n");
				}
				continue;
			}
			if (buffer_size > BUFFER_SIZE) {
				fprintf(stderr, "WARNING: size returned from avcodec_decode_audioX is too large\n");
				continue;
			}

			length = MIN(remaining, buffer_size / 2);
			if (!chromaprint_feed(chromaprint_ctx, buffer, length)) {
				fprintf(stderr, "ERROR: fingerprint calculation failed\n");
				goto done;
			}

			if (max_length) {
				remaining -= length;
				if (remaining <= 0) {
					goto finish;
				}
			}
		}

		if (packet.data) {
			av_free_packet(&packet);
		}
	}

finish:
	if (!chromaprint_finish(chromaprint_ctx)) {
		fprintf(stderr, "ERROR: fingerprint calculation failed\n");
		goto done;
	}

	ok = 1;

done:
	if (codec_ctx_opened) {
		avcodec_close(codec_ctx);
	}
	if (format_ctx) {
		av_close_input_file(format_ctx);
	}
	return ok;
}

int fpcalc_main(int argc, char **argv)
{
	int i, j, max_length = 60, num_file_names = 0, raw = 0, raw_fingerprint_size, duration;
	int16_t *buffer;
	int32_t *raw_fingerprints[2];
	char *file_name, *fingerprint, **file_names;
	ChromaprintContext *chromaprint_ctx;
    int32_t *all_raw_fingerprints;
    int setbits = 0;
    int32_t thisdiff;

	file_names = malloc(argc * sizeof(char *));
	for (i = 1; i < argc; i++) {
        file_names[num_file_names++] = argv[i];
	}

	if (num_file_names != 2) {
		printf("usage: %s [OPTIONS] FILE1 FILE2\n\n", argv[0]);
		return 2;
	}

	av_register_all();
	av_log_set_level(AV_LOG_ERROR);

	buffer = av_malloc(BUFFER_SIZE + 16);
	chromaprint_ctx = chromaprint_new(CHROMAPRINT_ALGORITHM_DEFAULT);

	for (i = 0; i < num_file_names; i++) {
		file_name = file_names[i];
		if (!decode_audio_file(chromaprint_ctx, buffer, file_name, max_length, &duration)) {
			fprintf(stderr, "ERROR: unable to calculate fingerprint for file %s, skipping\n", file_name);
			continue;
		}
        if (!chromaprint_get_raw_fingerprint(chromaprint_ctx, (void **)&raw_fingerprints[i], &raw_fingerprint_size)) {
            fprintf(stderr, "ERROR: unable to calculate fingerprint for file %s, skipping\n", file_name);
            continue;
        }
	}
//    printf("FINGERPRINTDELTA=");
    for (j = 0; j < raw_fingerprint_size; j++) {
        thisdiff = raw_fingerprints[0][j]^raw_fingerprints[1][j];
//        printf("0x%08x%s", thisdiff, j + 1 < raw_fingerprint_size ? "," : "\n");
        setbits += __builtin_popcount(thisdiff);
    }
    printf("%f\n", setbits/(raw_fingerprint_size*sizeof(int32_t)*8.0));
    chromaprint_dealloc(raw_fingerprints[0]);
    chromaprint_dealloc(raw_fingerprints[1]);

	chromaprint_free(chromaprint_ctx);
	av_free(buffer);
	free(file_names);

	return 0;
}

#ifdef _WIN32
int main(int win32_argc, char **win32_argv)
{
	int i, argc = 0, buffsize = 0, offset = 0;
	char **utf8_argv, *utf8_argv_ptr;
	wchar_t **argv;

	argv = CommandLineToArgvW(GetCommandLineW(), &argc);

	buffsize = 0;
	for (i = 0; i < argc; i++) {
		buffsize += WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, NULL, 0, NULL, NULL);
	}

	utf8_argv = av_mallocz(sizeof(char *) * (argc + 1) + buffsize);
	utf8_argv_ptr = (char *)utf8_argv + sizeof(char *) * (argc + 1);

	for (i = 0; i < argc; i++) {
		utf8_argv[i] = &utf8_argv_ptr[offset];
		offset += WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, &utf8_argv_ptr[offset], buffsize - offset, NULL, NULL);
	}

	LocalFree(argv);

	return fpcalc_main(argc, utf8_argv);
}
#else
int main(int argc, char **argv)
{
	return fpcalc_main(argc, argv);
}
#endif

