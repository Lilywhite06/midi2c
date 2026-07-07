/**
 * @file      midi2c.c
 * @brief     Standard MIDI File (.mid) to C Array Converter for Embedded Systems
 * @details   Extracts note events from a MIDI file and generates a monophonic C array.
 *            Supports two operation modes (--mode):
 *              legato : Last-Note Priority with fallback (default)
 *                       When the latest note is released, the previously held note resumes.
 *                       This preserves musical legato/trill phrasing.
 *              grab   : New note steals, release of latest silences immediately
 *                       Suited for simple single-voice buzzers where only the most
 *                       recent note can be heard, and note-off ends all sound.
 *
 * @usage     midi2c [options] <input.mid>
 *   Options:
 *     -t <track>   : extract only the specified track number (1-based)
 *                    -t 0 or omitted = all tracks
 *     --mode <m>   : select operation mode: legato (default) or grab
 *
 *   Example: midi2c song.mid > melody.h
 *            midi2c --mode grab -t 1 melody.mid > melody_grab.h
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>

// --- MIDI Constants ---
#define MIDI_CMD_NOTE_OFF   0x80
#define MIDI_CMD_NOTE_ON    0x90
#define MIDI_CMD_CONTROL    0xB0
#define MIDI_CMD_PROGRAM    0xC0
#define MIDI_CMD_PITCH_BEND 0xE0
#define MIDI_CMD_SYSEX      0xF0
#define MIDI_CMD_META       0xFF
#define META_TEMPO          0x51
#define REST_NOTE           255

// --- Tempo Map Structures ---
typedef struct {
    uint32_t absolute_tick;
    uint32_t tempo;         // Microseconds per quarter note
    uint64_t accum_time;    // Pre-calculated absolute microseconds
} TempoEvent_t;

TempoEvent_t *tempo_map = NULL;
int tempo_count = 0;
size_t tempo_capacity = 100;

// --- MIDI note to Frequency (Hz) Table ---
const uint16_t MIDI_FREQ_TABLE[128] = {
    8, 9, 9, 10, 10, 11, 12, 12, 13, 14, 15, 15, 
    16, 17, 18, 19, 20, 21, 23, 24, 25, 27, 29, 30, 
    32, 34, 36, 38, 41, 43, 46, 48, 51, 55, 58, 61, 
    65, 69, 73, 77, 82, 87, 92, 97, 103, 110, 116, 123, 
    130, 138, 146, 155, 164, 174, 184, 195, 207, 220, 233, 246, 
    261, 277, 293, 311, 329, 349, 369, 391, 415, 440, 466, 493, 
    523, 554, 587, 622, 659, 698, 739, 783, 830, 880, 932, 987, 
    1046, 1108, 1174, 1244, 1318, 1396, 1479, 1567, 1661, 1760, 1864, 1975, 
    2093, 2217, 2349, 2489, 2637, 2793, 2959, 3135, 3322, 3520, 3729, 3951, 
    4186, 4434, 4698, 4978, 5274, 5587, 5919, 6271, 6644, 7040, 7458, 7902, 
    8372, 8869, 9397, 9956, 10548, 11175, 11839, 12543
};

bool safe_read_u32(FILE *file, uint32_t *out_val) {
    uint8_t buf[4];
    if (fread(buf, 1, 4, file) != 4) return false;
    *out_val = (buf[0] << 24) | (buf[1] << 16) | (buf[2] << 8) | buf[3];
    return true;
}

bool safe_read_u16(FILE *file, uint16_t *out_val) {
    uint8_t buf[2];
    if (fread(buf, 1, 2, file) != 2) return false;
    *out_val = (buf[0] << 8) | buf[1];
    return true;
}

bool safe_read_vlq(FILE *file, uint32_t *out_val, long max_bytes_left) {
    uint32_t value = 0;
    uint8_t byte;
    long bytes_read = 0;
    do {
        if (bytes_read >= max_bytes_left) return false;
        if (fread(&byte, 1, 1, file) != 1) return false;
        bytes_read++;
        value = (value << 7) | (byte & 0x7F);
    } while (byte & 0x80);
    *out_val = value;
    return true;
}

// O(log N) Comparator
int compare_tempo(const void *a, const void *b) {
    TempoEvent_t *t1 = (TempoEvent_t *)a;
    TempoEvent_t *t2 = (TempoEvent_t *)b;
    if (t1->absolute_tick < t2->absolute_tick) return -1;
    if (t1->absolute_tick > t2->absolute_tick) return 1;
    return 0;
}

uint32_t tick_to_ms(uint32_t target_tick, uint16_t division) {
    if (division == 0) return 0;

    if (tempo_map == NULL || tempo_count == 0) {
        uint64_t total_time_accum = (uint64_t)target_tick * 500000;
        uint64_t dividend = total_time_accum + ((uint64_t)division * 500);
        return (uint32_t)(dividend / ((uint64_t)division * 1000));
    }

    int low = 0, high = tempo_count - 1;
    int best_idx = 0;
    while (low <= high) {
        int mid = low + (high - low) / 2;
        if (tempo_map[mid].absolute_tick <= target_tick) {
            best_idx = mid;
            low = mid + 1;
        } else {
            high = mid - 1;
        }
    }

    uint64_t base_time = tempo_map[best_idx].accum_time;
    uint32_t remaining_ticks = target_tick - tempo_map[best_idx].absolute_tick;
    uint32_t current_tempo = tempo_map[best_idx].tempo;

    uint64_t total_time_accum = base_time + ((uint64_t)remaining_ticks * current_tempo);
    uint64_t dividend = total_time_accum + ((uint64_t)division * 500); 
    
    return (uint32_t)(dividend / ((uint64_t)division * 1000));
}

uint8_t get_active_latest_note(const uint32_t *key_state_ts) {
    uint32_t max_ts = 0;
    uint8_t latest_note = REST_NOTE;
    for (int i = 0; i < 128; i++) {
        if (key_state_ts[i] > max_ts) {
            max_ts = key_state_ts[i];
            latest_note = i;
        }
    }
    return latest_note;
}

void flush_note(uint8_t note, uint32_t elapsed_ms, int track_num, int *header_printed) {
    if (elapsed_ms == 0) return; 
    
    if (!(*header_printed)) {
        printf("\n    // --- Track %d Start ---\n", track_num);
        *header_printed = 1;
    }
    
    if (note != REST_NOTE && note < 128) {
        printf("    {%u, %u},\n", MIDI_FREQ_TABLE[note], elapsed_ms);
    } else {
        printf("    {0, %u}, // Rest\n", elapsed_ms);
    }
}

void print_help(const char* prog_name) {
    printf("Usage: %s [options] <input.mid>\n", prog_name);
    printf("Options:\n");
    printf("  -t <track>   : Extract only specified track number (1-based).\n");
    printf("  --mode <m>   : legato (default) OR grab\n");
    printf("  -h, --help   : Display this help.\n");
}

int main(int argc, char *argv[]) {
    const char *filename = NULL;
    int target_track = -1;
    int mode = 0; 

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_help(argv[0]);
            return EXIT_SUCCESS;
        } else if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
            target_track = atoi(argv[i + 1]);
            i++;
        } else if (strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
            if (strcmp(argv[i + 1], "legato") == 0) mode = 0;
            else if (strcmp(argv[i + 1], "grab") == 0) mode = 1;
            else fprintf(stderr, "[WARNING] Unknown mode '%s', using default.\n", argv[i+1]);
            i++;
        } else if (argv[i][0] != '-') {
            filename = argv[i];
        }
    }

    if (!filename) { print_help(argv[0]); return EXIT_FAILURE; }

    FILE *file = fopen(filename, "rb");
    if (!file) { fprintf(stderr, "[ERROR] Cannot open file '%s'\n", filename); return EXIT_FAILURE; }

    char chunk_type[4];
    if (fread(chunk_type, 1, 4, file) != 4 || strncmp(chunk_type, "MThd", 4) != 0) {
        fprintf(stderr, "[ERROR] Invalid MIDI format (MThd missing).\n");
        fclose(file); return EXIT_FAILURE;
    }

    uint32_t header_len;
    uint16_t format, track_count_header, division;
    if (!safe_read_u32(file, &header_len) || 
        !safe_read_u16(file, &format) || 
        !safe_read_u16(file, &track_count_header) || 
        !safe_read_u16(file, &division)) {
        fprintf(stderr, "[ERROR] Corrupted MThd header.\n");
        fclose(file); return EXIT_FAILURE;
    }

    if (division & 0x8000) {
        fprintf(stderr, "\n[FATAL] SMPTE time format not supported.\n");
        fclose(file); return EXIT_FAILURE;
    }

    tempo_map = (TempoEvent_t *)malloc(tempo_capacity * sizeof(TempoEvent_t));
    long first_chunk_pos = ftell(file);

    /* ====================================================================
     * PASS 1: Build Global Tempo Map
     * ==================================================================== */
    while (fread(chunk_type, 1, 4, file) == 4) {
        uint32_t chunk_len;
        if (!safe_read_u32(file, &chunk_len)) break;
        long track_start_pos = ftell(file);
        long track_end_pos = track_start_pos + chunk_len;

        if (strncmp(chunk_type, "MTrk", 4) == 0) {
            uint32_t absolute_tick = 0;
            uint8_t last_status = 0;

            while (ftell(file) < track_end_pos) {
                uint32_t delta;
                long left = track_end_pos - ftell(file);
                if (!safe_read_vlq(file, &delta, left)) break;
                absolute_tick += delta;

                uint8_t status;
                if (fread(&status, 1, 1, file) != 1) break;

                if (status < 0x80) { 
                    status = last_status; 
                    if (fseek(file, -1, SEEK_CUR) != 0) break;
                } else { 
                    last_status = status; 
                }

                left = track_end_pos - ftell(file);

                if (status == MIDI_CMD_SYSEX || status == 0xF7) {
                    uint32_t sysex_len;
                    if (!safe_read_vlq(file, &sysex_len, left)) break;

                    last_status = 0; 
                    left = track_end_pos - ftell(file);
                    if (sysex_len > (uint32_t)left) break; 
                    if (fseek(file, sysex_len, SEEK_CUR) != 0) break;

                } else if (status == MIDI_CMD_META) {
                    uint8_t meta_type;
                    if (fread(&meta_type, 1, 1, file) != 1) break;
                    left = track_end_pos - ftell(file);
                    
                    uint32_t meta_len;
                    if (!safe_read_vlq(file, &meta_len, left)) break;

                    last_status = 0;

                    left = track_end_pos - ftell(file);
                    if (meta_len > (uint32_t)left) break;

                    if (meta_type == META_TEMPO && meta_len == 3) {
                        uint8_t b[3];
                        if (fread(b, 1, 3, file) == 3) {
                            if (tempo_count >= tempo_capacity) {
    				size_t new_cap = tempo_capacity * 2;
    				TempoEvent_t *tmp = (TempoEvent_t *)realloc(tempo_map, new_cap * sizeof(TempoEvent_t));
    				if (!tmp) {
        				fprintf(stderr, "[FATAL] Out of memory expanding Tempo Map.\n");
        				free(tempo_map);
        				fclose(file);
        				return EXIT_FAILURE;
    				}
    				tempo_map = tmp;
    				tempo_capacity = new_cap;
			    }
                            tempo_map[tempo_count].absolute_tick = absolute_tick;
                            tempo_map[tempo_count].tempo = (b[0] << 16) | (b[1] << 8) | b[2];
                            tempo_count++;
                        }
                    } else {
                        if (fseek(file, meta_len, SEEK_CUR) != 0) break;
                    }
                } else if ((status & 0xF0) >= 0x80 && (status & 0xF0) <= MIDI_CMD_PITCH_BEND) {
                    int skip = ((status & 0xF0) == MIDI_CMD_PROGRAM || (status & 0xF0) == 0xD0) ? 1 : 2;
                    left = track_end_pos - ftell(file);
                    if (skip > left) break;
                    if (fseek(file, skip, SEEK_CUR) != 0) break;
                } else {
                    break;
                }
            }
        }
        if (fseek(file, track_end_pos, SEEK_SET) != 0) break;
    }

    if (tempo_count > 0) {
        qsort(tempo_map, tempo_count, sizeof(TempoEvent_t), compare_tempo);
        uint32_t last_tick = 0, current_tempo = 500000;
        uint64_t current_accum = 0;
        for (int i = 0; i < tempo_count; i++) {
            current_accum += (uint64_t)(tempo_map[i].absolute_tick - last_tick) * current_tempo;
            tempo_map[i].accum_time = current_accum;
            last_tick = tempo_map[i].absolute_tick;
            current_tempo = tempo_map[i].tempo;
        }
    }

    /* ====================================================================
     * PASS 2: Array Generation
     * ==================================================================== */
    if (fseek(file, first_chunk_pos, SEEK_SET) != 0) {
        fprintf(stderr, "[ERROR] Fatal I/O error seeking PASS 2.\n");
        free(tempo_map); fclose(file); return EXIT_FAILURE;
    }

    printf("/* Auto-generated Audio Header */\n");
    printf("typedef struct {\n    uint16_t frequency;\n    uint32_t duration;\n} AudioNote_t;\n\n");
    printf("/* Source: %s | Track: %s | Mode: %s */\n", 
           filename, (target_track == -1) ? "Auto" : "Specified", mode == 0 ? "Legato" : "Grab");
    printf("const AudioNote_t my_new_melody[] = {\n");

    int track_count = 0, tracks_extracted = 0;

    while (fread(chunk_type, 1, 4, file) == 4) {
        uint32_t chunk_len;
        if (!safe_read_u32(file, &chunk_len)) break;
        long track_end_pos = ftell(file) + chunk_len;

        if (strncmp(chunk_type, "MTrk", 4) == 0) {
            track_count++;

            if (target_track == -1) {
                if (track_count == 1 && format == 1) { fseek(file, track_end_pos, SEEK_SET); continue; }
                if (tracks_extracted > 0) {
                    fprintf(stderr, "\n[WARNING] Multi-track ignored. Extracting Track %d.\n", track_count - 1);
                    fseek(file, track_end_pos, SEEK_SET); continue;
                }
            } else if (track_count != target_track) {
                fseek(file, track_end_pos, SEEK_SET); continue;
            }

            tracks_extracted++;
            uint8_t last_status = 0;
            uint32_t absolute_tick = 0, last_absolute_ms = 0, time_accum_ms = 0;

            uint32_t key_state_ts[128] = {0};   
            uint32_t event_counter = 1;
            uint8_t current_note = REST_NOTE;           
            uint8_t latest_note_on = REST_NOTE;         
            uint8_t sounding_note = REST_NOTE;          
            int track_header_printed = 0;

            while (ftell(file) < track_end_pos) {
                uint32_t delta;
                long left = track_end_pos - ftell(file);
                if (!safe_read_vlq(file, &delta, left)) break;
                absolute_tick += delta;

                if (division > 0) {
                    uint32_t current_absolute_ms = tick_to_ms(absolute_tick, division);
                    time_accum_ms += current_absolute_ms - last_absolute_ms;
                    last_absolute_ms = current_absolute_ms;
                }

                uint8_t status;
                if (fread(&status, 1, 1, file) != 1) break;
                if (status < 0x80) { 
                    status = last_status; 
                    if (fseek(file, -1, SEEK_CUR) != 0) break; 
                } else { 
                    last_status = status; 
                }

                uint8_t cmd = status & 0xF0;
                left = track_end_pos - ftell(file);

                if (status == MIDI_CMD_SYSEX || status == 0xF7) {
                    uint32_t sysex_len;
                    if (!safe_read_vlq(file, &sysex_len, left)) break;
                    
                    last_status = 0;
                    left = track_end_pos - ftell(file);
                    if (sysex_len > (uint32_t)left || fseek(file, sysex_len, SEEK_CUR) != 0) break;

                } else if (status == MIDI_CMD_META) {
                    uint8_t meta_type;
                    if (fread(&meta_type, 1, 1, file) != 1) break;
                    left = track_end_pos - ftell(file);
                    
                    uint32_t meta_len;
                    if (!safe_read_vlq(file, &meta_len, left)) break;
                    
                    last_status = 0;
                    left = track_end_pos - ftell(file);
                    if (meta_len > (uint32_t)left || fseek(file, meta_len, SEEK_CUR) != 0) break;

                } else if (cmd == MIDI_CMD_NOTE_ON || cmd == MIDI_CMD_NOTE_OFF) {
                    uint8_t note, vel;
                    if (fread(&note, 1, 1, file) != 1 || fread(&vel, 1, 1, file) != 1) break;
                    note &= 0x7F;

                    if (mode == 0) { 
                        if (cmd == MIDI_CMD_NOTE_OFF || vel == 0) key_state_ts[note] = 0;
                        else key_state_ts[note] = event_counter++;
                        
                        uint8_t new_latest_note = get_active_latest_note(key_state_ts);
                        if (new_latest_note != current_note) {
                            flush_note(current_note, time_accum_ms, track_count, &track_header_printed);
                            time_accum_ms = 0;
                            current_note = new_latest_note;
                        }
                    } else {         
                        if (cmd == MIDI_CMD_NOTE_OFF || vel == 0) {
                            if (note == latest_note_on && sounding_note != REST_NOTE) {
                                flush_note(sounding_note, time_accum_ms, track_count, &track_header_printed);
                                time_accum_ms = 0;
                                sounding_note = REST_NOTE;
                            }
                        } else {
                            latest_note_on = note;
                            if (sounding_note != latest_note_on) {
                                flush_note(sounding_note, time_accum_ms, track_count, &track_header_printed);
                                time_accum_ms = 0;
                                sounding_note = latest_note_on;
                            }
                        }
                    }
                } else if (cmd >= 0x80 && cmd <= MIDI_CMD_PITCH_BEND) {
                    int skip = (cmd == MIDI_CMD_PROGRAM || cmd == 0xD0) ? 1 : 2;
                    if (skip > left || fseek(file, skip, SEEK_CUR) != 0) break;
                } else { break; }
            }

            if (mode == 0 && current_note != REST_NOTE) flush_note(current_note, time_accum_ms, track_count, &track_header_printed);
            else if (mode == 1 && sounding_note != REST_NOTE) flush_note(sounding_note, time_accum_ms, track_count, &track_header_printed);
            if (track_header_printed) printf("    // --- Track %d End ---\n", track_count);
        }
        if (fseek(file, track_end_pos, SEEK_SET) != 0) break;
    }

    printf("\n    {0, 0} // [End of File Sentinel]\n};\n");
    free(tempo_map); fclose(file);
    return EXIT_SUCCESS;
}
