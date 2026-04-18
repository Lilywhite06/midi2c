/**
 * @file      midi2c.c
 * @brief     Standard MIDI File (.mid) to C Array Converter for Embedded Systems
 * @author    Assistant (Refined for Industrial Standards)
 * @details   Extracts note events from a MIDI file and generates a monophonic C array.
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

// Structure to hold tempo changes across the global absolute timeline
typedef struct {
    uint32_t absolute_tick;
    uint32_t tempo; // Microseconds per quarter note
} TempoEvent_t;

// Global pointers for Dynamic Tempo Map
TempoEvent_t *tempo_map = NULL;
int tempo_count = 0;
size_t tempo_capacity = 100; // Initial capacity, dynamically scales

// MIDI note number (0-127) to Frequency (Hz) mapping table
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

/**
 * @brief  Helper functions for Big-Endian file reading & VLQ parsing
 */
uint32_t read_uint32_be(FILE *file) {
    uint8_t buffer[4];
    if (fread(buffer, 1, 4, file) != 4) return 0;
    return (buffer[0] << 24) | (buffer[1] << 16) | (buffer[2] << 8) | buffer[3];
}

uint16_t read_uint16_be(FILE *file) {
    uint8_t buffer[2];
    if (fread(buffer, 1, 2, file) != 2) return 0;
    return (buffer[0] << 8) | buffer[1];
}

uint32_t read_vlq(FILE *file) {
    uint32_t value = 0;
    uint8_t byte;
    do {
        if (fread(&byte, 1, 1, file) != 1) break;
        value = (value << 7) | (byte & 0x7F);
    } while (byte & 0x80);
    return value;
}

/**
 * @brief  Scans the key state array to find the highest currently pressed note
 */
uint8_t get_active_highest_note(const uint8_t *key_state) {
    for (int i = 127; i >= 0; i--) {
        if (key_state[i]) return i;
    }
    return 0; // 0 indicates no note is currently pressed (Rest)
}

/**
 * @brief  Comparator for sorting the tempo map chronologically
 */
int compare_tempo(const void *a, const void *b) {
    TempoEvent_t *t1 = (TempoEvent_t *)a;
    TempoEvent_t *t2 = (TempoEvent_t *)b;
    if (t1->absolute_tick < t2->absolute_tick) return -1;
    if (t1->absolute_tick > t2->absolute_tick) return 1;
    return 0;
}

/**
 * @brief  Converts an absolute tick value into absolute milliseconds using the global Tempo Map
 */
uint32_t tick_to_ms(uint32_t target_tick, uint16_t division) {
    if (division == 0 || tempo_map == NULL) return 0;
    
    uint64_t total_time_accum = 0; 
    uint32_t current_tick = 0;
    uint32_t current_tempo = 500000; // Default: 120 BPM

    for (int i = 0; i < tempo_count; i++) {
        if (tempo_map[i].absolute_tick >= target_tick) {
            break; 
        }
        uint32_t segment_ticks = tempo_map[i].absolute_tick - current_tick;
        total_time_accum += (uint64_t)segment_ticks * current_tempo;
        
        current_tick = tempo_map[i].absolute_tick;
        current_tempo = tempo_map[i].tempo;
    }

    uint32_t remaining_ticks = target_tick - current_tick;
    total_time_accum += (uint64_t)remaining_ticks * current_tempo;

    uint64_t dividend = total_time_accum + ((uint64_t)division * 500); 
    return (uint32_t)(dividend / ((uint64_t)division * 1000));
}

int main(int argc, char *argv[]) {
    const char *filename = "song.mid";
    int target_track = -1; // -1 means extract all tracks

    // Parse CLI arguments
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
            target_track = atoi(argv[i + 1]);
            i++; 
        } else if (argv[i][0] != '-') {
            filename = argv[i];
        }
    }
    
    FILE *file = fopen(filename, "rb");
    if (!file) {
        fprintf(stderr, "Error: Cannot open input file '%s'\n", filename);
        return EXIT_FAILURE;
    }

    char chunk_type[4];
    if (fread(chunk_type, 1, 4, file) != 4 || strncmp(chunk_type, "MThd", 4) != 0) {
        fprintf(stderr, "Error: Invalid MIDI format (MThd not found).\n");
        fclose(file);
        return EXIT_FAILURE;
    }

    read_uint32_be(file);           // Skip header length
    uint16_t format = read_uint16_be(file); 
    read_uint16_be(file);           // Skip track count
    uint16_t division = read_uint16_be(file); 
    
    // [Safety Fix]: Intercept unsupported SMPTE time formats immediately
    if (division & 0x8000) {
        fprintf(stderr, "\n[FATAL ERROR] SMPTE time format is not supported.\n");
        fprintf(stderr, "Please export your MIDI file using TPQN (Ticks Per Quarter Note) format.\n\n");
        fclose(file);
        return EXIT_FAILURE;
    }

    // Allocate dynamic memory for Tempo Map
    tempo_map = (TempoEvent_t *)malloc(tempo_capacity * sizeof(TempoEvent_t));
    if (!tempo_map) {
        fprintf(stderr, "Memory allocation failed.\n");
        fclose(file);
        return EXIT_FAILURE;
    }

    long first_chunk_pos = ftell(file);

    /* ====================================================================
     * PASS 1: Scan for all Tempo Events to build the Global Tempo Map
     * ==================================================================== */
    while (fread(chunk_type, 1, 4, file) == 4) {
        uint32_t chunk_len = read_uint32_be(file);
        long track_end_pos = ftell(file) + chunk_len;
        
        if (strncmp(chunk_type, "MTrk", 4) == 0) {
            uint32_t absolute_tick = 0;
            uint8_t last_status = 0;
            
            while (ftell(file) < track_end_pos) {
                uint32_t delta_ticks = read_vlq(file);
                absolute_tick += delta_ticks;

                uint8_t status;
                if (fread(&status, 1, 1, file) != 1) break;

                if (status < 0x80) {
                    status = last_status;
                    fseek(file, -1, SEEK_CUR);
                } else {
                    last_status = status;
                }

                if (status == 0xF0 || status == 0xF7) {
                    uint32_t sysex_len = read_vlq(file);
                    fseek(file, sysex_len, SEEK_CUR);
                }
                else if (status == 0xFF) { 
                    uint8_t meta_type; 
                    if (fread(&meta_type, 1, 1, file) != 1) break;
                    uint32_t meta_len = read_vlq(file);
                    
                    if (meta_type == 0x51 && meta_len == 3) {
                        uint8_t b[3]; 
                        if (fread(b, 1, 3, file) == 3) {
                            // Dynamically resize Tempo Map if capacity is reached
                            if (tempo_count >= tempo_capacity) {
                                tempo_capacity *= 2;
                                tempo_map = (TempoEvent_t *)realloc(tempo_map, tempo_capacity * sizeof(TempoEvent_t));
                            }
                            tempo_map[tempo_count].absolute_tick = absolute_tick;
                            tempo_map[tempo_count].tempo = (b[0] << 16) | (b[1] << 8) | b[2];
                            tempo_count++;
                        }
                    } else {
                        fseek(file, meta_len, SEEK_CUR);
                    }
                }
                else if ((status & 0xF0) == 0xC0 || (status & 0xF0) == 0xD0) {
                    fseek(file, 1, SEEK_CUR); 
                }
                else if (status >= 0x80 && status < 0xF0) {
                    fseek(file, 2, SEEK_CUR); 
                }
                else {
                    break; // Fallback: Break out of corrupted track chunk
                }
            }
        }
        fseek(file, track_end_pos, SEEK_SET); 
    }

    if (tempo_count > 0) {
        qsort(tempo_map, tempo_count, sizeof(TempoEvent_t), compare_tempo);
    }

    /* ====================================================================
     * PASS 2: Parse Notes with High-Note Priority & Absolute Time
     * ==================================================================== */
    fseek(file, first_chunk_pos, SEEK_SET); 
    
    printf("/* Auto-generated Frequency Array (Hz, ms) for STM32 */\n");
    printf("/* Source: %s | Track: %s */\n", filename, (target_track == -1) ? "All" : "Specified");
    if (format == 1) printf("/* Format 1 detected: Global Tempo Map active. */\n");
    printf("const AudioNote_t my_new_melody[] = {\n");

    int track_count = 0;

    while (fread(chunk_type, 1, 4, file) == 4) {
        uint32_t chunk_len = read_uint32_be(file);
        long track_end_pos = ftell(file) + chunk_len;
        
        if (strncmp(chunk_type, "MTrk", 4) == 0) {
            track_count++;
            
            // Skip tracks if a specific track is targeted via CLI
            if (target_track != -1 && track_count != target_track) {
                fseek(file, track_end_pos, SEEK_SET); 
                continue;
            }

            uint8_t last_status = 0;
            uint32_t absolute_tick = 0;
            uint32_t last_absolute_ms = 0;
            uint32_t time_accum_ms = 0;
            
            uint8_t key_state[128] = {0}; // Note Stack for High-Note priority
            uint8_t current_note = 0;     // The currently sounding note
            int track_header_printed = 0;
            
            while (ftell(file) < track_end_pos) {
                uint32_t delta_ticks = read_vlq(file);
                absolute_tick += delta_ticks;
                
                if (division > 0) {
                    uint32_t current_absolute_ms = tick_to_ms(absolute_tick, division);
                    uint32_t delta_ms = current_absolute_ms - last_absolute_ms;
                    time_accum_ms += delta_ms;
                    last_absolute_ms = current_absolute_ms;
                }

                uint8_t status;
                if (fread(&status, 1, 1, file) != 1) break;

                if (status < 0x80) {
                    status = last_status;
                    fseek(file, -1, SEEK_CUR);
                } else {
                    last_status = status;
                }

                if (status == 0xF0 || status == 0xF7) {
                    uint32_t sysex_len = read_vlq(file);
                    fseek(file, sysex_len, SEEK_CUR);
                }
                else if (status == 0xFF) { 
                    uint8_t meta_type; 
                    if (fread(&meta_type, 1, 1, file) != 1) break;
                    uint32_t meta_len = read_vlq(file);
                    fseek(file, meta_len, SEEK_CUR);
                }
                // Handle Note On & Note Off uniformly using Key State Array
                else if ((status & 0xF0) == 0x90 || (status & 0xF0) == 0x80) { 
                    uint8_t note, vel; 
                    if (fread(&note, 1, 1, file) != 1) break;
                    if (fread(&vel, 1, 1, file) != 1) break;
                    
                    // Note Off (0x80) OR Note On with 0 velocity
                    if ((status & 0xF0) == 0x80 || vel == 0) {
                        key_state[note & 0x7F] = 0;
                    } else {
                        key_state[note & 0x7F] = 1;
                    }

                    // Evaluate if the highest currently pressed note has changed
                    uint8_t new_highest_note = get_active_highest_note(key_state);
                    
                    if (new_highest_note != current_note) {
                        if (time_accum_ms > 0) {
                            if (!track_header_printed) { printf("\n    // --- Track %d Start ---\n", track_count); track_header_printed = 1; }
                            
                            if (current_note > 0) {
                                printf("    {%u, %u},\n", MIDI_FREQ_TABLE[current_note & 0x7F], time_accum_ms);
                            } else {
                                printf("    {0, %u}, // Rest\n", time_accum_ms); 
                            }
                        }
                        current_note = new_highest_note;
                        time_accum_ms = 0;
                    }
                }
                else if ((status & 0xF0) == 0xC0 || (status & 0xF0) == 0xD0) {
                    fseek(file, 1, SEEK_CUR); 
                }
                else if (status >= 0x80 && status < 0xF0) {
                    fseek(file, 2, SEEK_CUR); 
                }
                else {
                    // [Safety Fix]: Prevent infinite loops on corrupted data
                    break; 
                }
            }
            
            if (current_note != 0 && time_accum_ms > 0) {
                if (!track_header_printed) { printf("\n    // --- Track %d Start ---\n", track_count); track_header_printed = 1; }
                printf("    {%u, %u},\n", MIDI_FREQ_TABLE[current_note & 0x7F], time_accum_ms);
            }
            
            if (track_header_printed) {
                printf("    // --- Track %d End ---\n", track_count);
            }
        }
        
        fseek(file, track_end_pos, SEEK_SET); 
    }
    
    // [Safety Fix]: EOF Sentinel guarantees C array compilation and safe embedded termination
    printf("\n    {0, 0} // [End of File Sentinel]\n");
    printf("};\n");
    
    free(tempo_map);
    fclose(file);
    return EXIT_SUCCESS;
}
