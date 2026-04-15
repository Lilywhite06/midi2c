/**
 * @file      midi2c.c
 * @brief     Standard MIDI File (.mid) to C Array Converter for STM32
 * @author    Assistant (Refined for Industrial Standards)
 * @details   Extracts note events from a MIDI file and generates a monophonic C array.
 * Implements a Two-Pass architecture (Tempo Map extraction -> Absolute Time resolution)
 * to flawlessly support Multi-Track (Format 1) files with complex tempo changes.
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#define MAX_TEMPO_EVENTS 1000

// Structure to hold tempo changes across the global absolute timeline
typedef struct {
    uint32_t absolute_tick;
    uint32_t tempo; // Microseconds per quarter note
} TempoEvent_t;

TempoEvent_t tempo_map[MAX_TEMPO_EVENTS];
int tempo_count = 0;

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
    if (division == 0) return 0;
    
    uint64_t total_time_accum = 0; 
    uint32_t current_tick = 0;
    uint32_t current_tempo = 500000; // Default: 120 BPM

    for (int i = 0; i < tempo_count; i++) {
        if (tempo_map[i].absolute_tick >= target_tick) {
            break; // Target tick falls within the current tempo segment
        }
        uint32_t segment_ticks = tempo_map[i].absolute_tick - current_tick;
        total_time_accum += (uint64_t)segment_ticks * current_tempo;
        
        current_tick = tempo_map[i].absolute_tick;
        current_tempo = tempo_map[i].tempo;
    }

    // Add the remaining ticks in the final tempo segment
    uint32_t remaining_ticks = target_tick - current_tick;
    total_time_accum += (uint64_t)remaining_ticks * current_tempo;

    // Perform a single division at the very end to eliminate floating-point drift
    uint64_t dividend = total_time_accum + ((uint64_t)division * 500); // Add half divisor for rounding
    return (uint32_t)(dividend / ((uint64_t)division * 1000));
}

int main(int argc, char *argv[]) {
    const char *filename = (argc > 1) ? argv[1] : "song.mid";
    
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
    
    if (division & 0x8000) {
        printf("/* WARNING: Unsupported SMPTE time format detected. Timing may be inaccurate. */\n");
        division &= 0x7FFF; 
    }

    // Save the file pointer position right after the MThd chunk
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
                    
                    // Capture Tempo Event
                    if (meta_type == 0x51 && meta_len == 3) {
                        uint8_t b[3]; 
                        if (fread(b, 1, 3, file) == 3) {
                            if (tempo_count < MAX_TEMPO_EVENTS) {
                                tempo_map[tempo_count].absolute_tick = absolute_tick;
                                tempo_map[tempo_count].tempo = (b[0] << 16) | (b[1] << 8) | b[2];
                                tempo_count++;
                            }
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
            }
        }
        fseek(file, track_end_pos, SEEK_SET); 
    }

    // Sort the Tempo Map chronologically (essential for Multi-Track Format 1 files)
    if (tempo_count > 0) {
        qsort(tempo_map, tempo_count, sizeof(TempoEvent_t), compare_tempo);
    }

    /* ====================================================================
     * PASS 2: Parse Notes and resolve Exact Milliseconds using Tempo Map
     * ==================================================================== */
    fseek(file, first_chunk_pos, SEEK_SET); // Rewind back to the first MTrk
    
    printf("/* Auto-generated Frequency Array (Hz, ms) for STM32 (Source: %s) */\n", filename);
    if (format == 1) printf("/* Format 1 (Multi-Track) detected: Global Tempo Map synchronized. */\n");
    printf("const AudioNote_t my_new_melody[] = {\n");

    int track_count = 0;

    while (fread(chunk_type, 1, 4, file) == 4) {
        uint32_t chunk_len = read_uint32_be(file);
        long track_end_pos = ftell(file) + chunk_len;
        
        if (strncmp(chunk_type, "MTrk", 4) == 0) {
            track_count++;
            uint8_t last_status = 0;
            
            uint32_t absolute_tick = 0;
            uint32_t last_absolute_ms = 0;
            uint32_t time_accum_ms = 0;
            uint32_t current_note = 0;
            
            printf("\n    // --- Track %d Start ---\n", track_count);
            
            while (ftell(file) < track_end_pos) {
                uint32_t delta_ticks = read_vlq(file);
                absolute_tick += delta_ticks;
                
                // Convert current absolute tick to absolute ms, and find the delta ms difference
                if (division > 0) {
                    uint32_t current_absolute_ms = tick_to_ms(absolute_tick, division);
                    uint32_t delta_ms = current_absolute_ms - last_absolute_ms;
                    time_accum_ms += delta_ms;
                    last_absolute_ms = current_absolute_ms;
                }

                uint8_t status;
                if (fread(&status, 1, 1, file) != 1) break;

                // Handle Running Status
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
                    // Pass 2 ignores tempo changes since they are already mapped
                    fseek(file, meta_len, SEEK_CUR);
                }
                // Note On
                else if ((status & 0xF0) == 0x90) { 
                    uint8_t note, vel; 
                    if (fread(&note, 1, 1, file) != 1) break;
                    if (fread(&vel, 1, 1, file) != 1) break;
                    
                    if (vel > 0) {
                        if (current_note != 0 && time_accum_ms > 0) {
                            printf("    {%u, %u},\n", MIDI_FREQ_TABLE[current_note & 0x7F], time_accum_ms);
                        } else if (current_note == 0 && time_accum_ms > 0) {
                            printf("    {0, %u}, // Rest\n", time_accum_ms); 
                        }
                        current_note = note;
                        time_accum_ms = 0;
                    } else { 
                        // Velocity 0 acts as Note Off
                        if (current_note == note && time_accum_ms > 0) {
                            printf("    {%u, %u},\n", MIDI_FREQ_TABLE[current_note & 0x7F], time_accum_ms);
                            current_note = 0;
                            time_accum_ms = 0;
                        }
                    }
                }
                // Note Off
                else if ((status & 0xF0) == 0x80) { 
                    uint8_t note, vel; 
                    if (fread(&note, 1, 1, file) != 1) break;
                    if (fread(&vel, 1, 1, file) != 1) break;
                    
                    if (current_note == note && time_accum_ms > 0) {
                        printf("    {%u, %u},\n", MIDI_FREQ_TABLE[current_note & 0x7F], time_accum_ms);
                        current_note = 0;
                        time_accum_ms = 0;
                    }
                }
                // Skip other events
                else if ((status & 0xF0) == 0xC0 || (status & 0xF0) == 0xD0) {
                    fseek(file, 1, SEEK_CUR); 
                }
                else if (status >= 0x80 && status < 0xF0) {
                    fseek(file, 2, SEEK_CUR); 
                }
            }
            
            if (current_note != 0 && time_accum_ms > 0) {
                printf("    {%u, %u},\n", MIDI_FREQ_TABLE[current_note & 0x7F], time_accum_ms);
            }
            printf("    // --- Track %d End ---\n", track_count);
        }
        
        fseek(file, track_end_pos, SEEK_SET); 
    }
    
    printf("};\n");
    fclose(file);
    return EXIT_SUCCESS;
}
