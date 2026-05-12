/*
 * wav_analyzer.c - WAV audio file analyzer
 *
 * Parses PCM WAV files and reports format metadata, amplitude statistics,
 * silence regions, and an ASCII waveform visualization.
 *
 * Usage: wav_analyzer <file.wav> [options]
 *   -w   show ASCII waveform (default: off)
 *   -s   show silence regions (default: off)
 *
 * Supports 8-bit, 16-bit, and 24-bit mono/stereo PCM WAV files.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

/*---------------------------------------------------------------------------*/
/* Constants                                                                  */
/*---------------------------------------------------------------------------*/
#define WAVEFORM_WIDTH    72
#define WAVEFORM_HEIGHT   12
#define SILENCE_THRESHOLD 0.01   /* fraction of max amplitude */
#define SILENCE_MIN_SEC   0.1    /* minimum silence duration to report */

/*---------------------------------------------------------------------------*/
/* WAV file layout structures (little-endian on disk)                         */
/*---------------------------------------------------------------------------*/
#pragma pack(push, 1)

typedef struct {
    char     chunk_id[4];   /* "RIFF" */
    uint32_t chunk_size;
    char     format[4];     /* "WAVE" */
} RiffHeader;

/* fmt chunk payload only (chunk id + size already consumed by find_chunk) */
typedef struct {
    uint16_t audio_format;  /* 1 = PCM */
    uint16_t num_channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
} FmtChunk;

#pragma pack(pop)

/*---------------------------------------------------------------------------*/
/* Aggregated file info                                                        */
/*---------------------------------------------------------------------------*/
typedef struct {
    char     filename[256];
    uint16_t num_channels;
    uint32_t sample_rate;
    uint16_t bits_per_sample;
    uint32_t num_frames;
    double   duration_sec;
    long     file_size_bytes;
    double   peak_amplitude;   /* 0.0 to 1.0 */
    double   rms_level;        /* 0.0 to 1.0 */
    double   peak_db;
    double   rms_db;
} WavInfo;

/*---------------------------------------------------------------------------*/
/* Helpers                                                                    */
/*---------------------------------------------------------------------------*/
static double linear_to_db(double linear) {
    if (linear <= 0.0) return -96.0;
    return 20.0 * log10(linear);
}

/* Read a 24-bit little-endian sample and sign-extend it to int32 */
static int32_t read_s24le(const uint8_t *p) {
    int32_t v = ((uint32_t)p[0]) | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
    if (v & 0x800000) v |= 0xFF000000;
    return v;
}

/*---------------------------------------------------------------------------*/
/* Core: find a chunk by its 4-byte ID, leave file positioned at chunk data   */
/*---------------------------------------------------------------------------*/
static int find_chunk(FILE *fp, const char *id, uint32_t *out_size) {
    char   found_id[4];
    uint32_t size;

    /* Search from current position; give up after 32 chunks */
    for (int i = 0; i < 32; i++) {
        if (fread(found_id, 1, 4, fp) != 4) return 0;
        if (fread(&size, 4, 1, fp) != 1)    return 0;
        if (memcmp(found_id, id, 4) == 0) {
            if (out_size) *out_size = size;
            return 1;
        }
        /* Skip this chunk */
        fseek(fp, (size + 1) & ~1, SEEK_CUR);
    }
    return 0;
}

/*---------------------------------------------------------------------------*/
/* Parse the file and fill WavInfo; returns 1 on success                      */
/*---------------------------------------------------------------------------*/
static int parse_wav(const char *path, WavInfo *info, float **out_samples,
                     uint32_t *out_num_samples) {
    FILE *fp = fopen(path, "rb");
    if (!fp) { fprintf(stderr, "Error: cannot open '%s'\n", path); return 0; }

    /* --- RIFF header --- */
    RiffHeader riff;
    if (fread(&riff, sizeof(riff), 1, fp) != 1 ||
        memcmp(riff.chunk_id, "RIFF", 4) != 0 ||
        memcmp(riff.format,   "WAVE", 4) != 0) {
        fprintf(stderr, "Error: '%s' is not a valid WAV file\n", path);
        fclose(fp); return 0;
    }

    /* --- fmt chunk --- */
    uint32_t fmt_size;
    if (!find_chunk(fp, "fmt ", &fmt_size)) {
        fprintf(stderr, "Error: no fmt chunk found\n");
        fclose(fp); return 0;
    }
    FmtChunk fmt;
    if (fread(&fmt, sizeof(fmt), 1, fp) != 1) {
        fprintf(stderr, "Error: truncated fmt chunk\n");
        fclose(fp); return 0;
    }
    if (fmt_size > sizeof(FmtChunk))
        fseek(fp, (long)(fmt_size - sizeof(FmtChunk)), SEEK_CUR);

    if (fmt.audio_format != 1) {
        fprintf(stderr, "Error: only PCM (format 1) is supported (got %u)\n",
                fmt.audio_format);
        fclose(fp); return 0;
    }
    if (fmt.bits_per_sample != 8 && fmt.bits_per_sample != 16 &&
        fmt.bits_per_sample != 24) {
        fprintf(stderr, "Error: unsupported bit depth %u\n", fmt.bits_per_sample);
        fclose(fp); return 0;
    }

    /* --- data chunk --- */
    uint32_t data_size;
    if (!find_chunk(fp, "data", &data_size)) {
        fprintf(stderr, "Error: no data chunk found\n");
        fclose(fp); return 0;
    }

    /* --- file size --- */
    long data_start = ftell(fp);
    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, data_start, SEEK_SET);

    /* --- compute frame count --- */
    uint32_t bytes_per_frame = fmt.num_channels * (fmt.bits_per_sample / 8);
    uint32_t num_frames      = data_size / bytes_per_frame;
    uint32_t total_samples   = num_frames * fmt.num_channels;

    /* --- read raw bytes --- */
    uint8_t *raw = malloc(data_size);
    if (!raw) { fprintf(stderr, "Error: out of memory\n"); fclose(fp); return 0; }
    uint32_t bytes_read = (uint32_t)fread(raw, 1, data_size, fp);
    fclose(fp);

    /* --- convert to normalised floats [-1, 1] --- */
    float *samples = malloc(total_samples * sizeof(float));
    if (!samples) { free(raw); fprintf(stderr, "Error: out of memory\n"); return 0; }

    for (uint32_t i = 0; i < total_samples && i * (fmt.bits_per_sample/8) < bytes_read; i++) {
        const uint8_t *p = raw + i * (fmt.bits_per_sample / 8);
        if (fmt.bits_per_sample == 8) {
            samples[i] = ((int)p[0] - 128) / 128.0f;
        } else if (fmt.bits_per_sample == 16) {
            int16_t v = (int16_t)(p[0] | (p[1] << 8));
            samples[i] = v / 32768.0f;
        } else {
            samples[i] = read_s24le(p) / 8388608.0f;
        }
    }
    free(raw);

    /* --- statistics --- */
    double peak = 0.0, rms_sum = 0.0;
    for (uint32_t i = 0; i < total_samples; i++) {
        double v = fabs((double)samples[i]);
        if (v > peak) peak = v;
        rms_sum += (double)samples[i] * samples[i];
    }
    double rms = (total_samples > 0) ? sqrt(rms_sum / total_samples) : 0.0;

    /* --- fill info --- */
    strncpy(info->filename, path, 255);
    info->num_channels    = fmt.num_channels;
    info->sample_rate     = fmt.sample_rate;
    info->bits_per_sample = fmt.bits_per_sample;
    info->num_frames      = num_frames;
    info->duration_sec    = (double)num_frames / fmt.sample_rate;
    info->file_size_bytes = file_size;
    info->peak_amplitude  = peak;
    info->rms_level       = rms;
    info->peak_db         = linear_to_db(peak);
    info->rms_db          = linear_to_db(rms);

    *out_samples     = samples;
    *out_num_samples = total_samples;
    return 1;
}

/*---------------------------------------------------------------------------*/
/* Print a formatted info block                                                */
/*---------------------------------------------------------------------------*/
static void print_info(const WavInfo *info) {
    int    mins = (int)(info->duration_sec / 60);
    double secs = info->duration_sec - mins * 60;

    printf("File       : %s\n",       info->filename);
    printf("File size  : %.1f KB\n",  info->file_size_bytes / 1024.0);
    printf("Channels   : %u (%s)\n",  info->num_channels,
           info->num_channels == 1 ? "mono" :
           info->num_channels == 2 ? "stereo" : "multi");
    printf("Sample rate: %u Hz\n",    info->sample_rate);
    printf("Bit depth  : %u-bit PCM\n", info->bits_per_sample);
    printf("Frames     : %u\n",       info->num_frames);
    printf("Duration   : %d:%05.2f (%.3f sec)\n", mins, secs, info->duration_sec);
    printf("Peak       : %.4f  (%+.1f dBFS)\n",   info->peak_amplitude, info->peak_db);
    printf("RMS level  : %.4f  (%+.1f dBFS)\n",   info->rms_level, info->rms_db);
}

/*---------------------------------------------------------------------------*/
/* Print ASCII waveform (channel 0 only, downsampled to WAVEFORM_WIDTH)       */
/*---------------------------------------------------------------------------*/
static void print_waveform(const float *samples, uint32_t total_samples,
                           uint16_t num_channels) {
    uint32_t num_frames = total_samples / num_channels;
    char grid[WAVEFORM_HEIGHT][WAVEFORM_WIDTH + 1];

    /* Build empty grid */
    for (int r = 0; r < WAVEFORM_HEIGHT; r++) {
        memset(grid[r], ' ', WAVEFORM_WIDTH);
        grid[r][WAVEFORM_WIDTH] = '\0';
    }

    /* Draw centre line */
    int mid = WAVEFORM_HEIGHT / 2;
    for (int c = 0; c < WAVEFORM_WIDTH; c++) grid[mid][c] = '-';

    /* Plot waveform column by column */
    for (int col = 0; col < WAVEFORM_WIDTH; col++) {
        uint32_t frame_start = (uint32_t)((uint64_t)col * num_frames / WAVEFORM_WIDTH);
        uint32_t frame_end   = (uint32_t)((uint64_t)(col + 1) * num_frames / WAVEFORM_WIDTH);
        if (frame_end <= frame_start) frame_end = frame_start + 1;
        if (frame_end > num_frames)   frame_end = num_frames;

        float mn = 1.0f, mx = -1.0f;
        for (uint32_t f = frame_start; f < frame_end; f++) {
            float v = samples[f * num_channels]; /* channel 0 */
            if (v < mn) mn = v;
            if (v > mx) mx = v;
        }

        /* Map to grid rows (row 0 = top = +1) */
        int row_hi = (int)((1.0f - mx) * 0.5f * (WAVEFORM_HEIGHT - 1) + 0.5f);
        int row_lo = (int)((1.0f - mn) * 0.5f * (WAVEFORM_HEIGHT - 1) + 0.5f);
        if (row_hi < 0) row_hi = 0;
        if (row_lo >= WAVEFORM_HEIGHT) row_lo = WAVEFORM_HEIGHT - 1;

        for (int r = row_hi; r <= row_lo; r++) grid[r][col] = '#';
    }

    printf("\nWaveform (channel 1):\n");
    printf("+1.0 +");
    for (int c = 0; c < WAVEFORM_WIDTH; c++) putchar('-');
    printf("+\n");
    for (int r = 0; r < WAVEFORM_HEIGHT; r++) {
        printf("     |%s|\n", grid[r]);
    }
    printf("-1.0 +");
    for (int c = 0; c < WAVEFORM_WIDTH; c++) putchar('-');
    printf("+\n");
}

/*---------------------------------------------------------------------------*/
/* Print silence regions                                                       */
/*---------------------------------------------------------------------------*/
static void print_silence(const float *samples, uint32_t total_samples,
                          uint16_t num_channels, uint32_t sample_rate) {
    double threshold = SILENCE_THRESHOLD;
    int    in_silence = 0;
    double silence_start = 0.0;
    int    found_any = 0;
    uint32_t num_frames = total_samples / num_channels;

    printf("\nSilence regions (below %.0f%% of full scale):\n",
           threshold * 100.0);

    for (uint32_t f = 0; f < num_frames; f++) {
        /* Average absolute amplitude across channels */
        double amp = 0.0;
        for (int c = 0; c < num_channels; c++)
            amp += fabs((double)samples[f * num_channels + c]);
        amp /= num_channels;

        double t = (double)f / sample_rate;

        if (!in_silence && amp < threshold) {
            in_silence    = 1;
            silence_start = t;
        } else if (in_silence && amp >= threshold) {
            double duration = t - silence_start;
            if (duration >= SILENCE_MIN_SEC) {
                printf("  %7.3f s -> %7.3f s  (%.3f s)\n",
                       silence_start, t, duration);
                found_any = 1;
            }
            in_silence = 0;
        }
    }
    /* Handle trailing silence */
    if (in_silence) {
        double end      = (double)num_frames / sample_rate;
        double duration = end - silence_start;
        if (duration >= SILENCE_MIN_SEC) {
            printf("  %7.3f s -> %7.3f s  (%.3f s)\n",
                   silence_start, end, duration);
            found_any = 1;
        }
    }
    if (!found_any) printf("  (none detected)\n");
}

/*---------------------------------------------------------------------------*/
/* Entry point                                                                 */
/*---------------------------------------------------------------------------*/
int main(int argc, char *argv[]) {
    int show_waveform = 0;
    int show_silence  = 0;
    const char *wav_path = NULL;

    for (int i = 1; i < argc; i++) {
        if      (strcmp(argv[i], "-w") == 0) show_waveform = 1;
        else if (strcmp(argv[i], "-s") == 0) show_silence  = 1;
        else if (argv[i][0] != '-')          wav_path      = argv[i];
        else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            fprintf(stderr, "Usage: wav_analyzer <file.wav> [-w] [-s]\n");
            return 1;
        }
    }

    if (!wav_path) {
        fprintf(stderr, "Usage: wav_analyzer <file.wav> [-w] [-s]\n");
        fprintf(stderr, "  -w  show ASCII waveform\n");
        fprintf(stderr, "  -s  show silence regions\n");
        return 1;
    }

    WavInfo  info;
    float   *samples     = NULL;
    uint32_t num_samples = 0;

    if (!parse_wav(wav_path, &info, &samples, &num_samples)) return 1;

    printf("=== WAV Analyzer ===\n\n");
    print_info(&info);
    if (show_waveform) print_waveform(samples, num_samples, info.num_channels);
    if (show_silence)  print_silence(samples, num_samples, info.num_channels,
                                     info.sample_rate);
    printf("\n");
    free(samples);
    return 0;
}
