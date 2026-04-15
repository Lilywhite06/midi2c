/**
 * @file      midi2c.c
 * @brief     Standard MIDI File (.mid) to C Array Converter for STM32
 * @author    Assistant (Refined for Industrial Standards)
 * @details   Extracts note events from a MIDI file and generates a monophonic C array 
 * containing absolute frequencies (Hz) and durations (ms).
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

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
 * @brief  Reads a 32-bit unsigned integer (Big-Endian) from the file.
 */
uint32_t read_uint32_be(FILE *file) {
    uint8_t buffer[4];
    if (fread(buffer, 1, 4, file) != 4) return 0;
    return (buffer[0] << 24) | (buffer[1] << 16) | (buffer[2] << 8) | buffer[3];
}

/**
 * @brief  Reads a 16-bit unsigned integer (Big-Endian) from the file.
 */
uint16_t read_uint16_be(FILE *file) {
    uint8_t buffer[2];
    if (fread(buffer, 1, 2, file) != 2) return 0;
    return (buffer[0] << 8) | buffer[1];
}

/**
 * @brief  Reads a Variable Length Quantity (VLQ) from the file.
 */
uint32_t read_vlq(FILE *file) {
    uint32_t value = 0;
    uint8_t byte;
    do {
        if (fread(&byte, 1, 1, file) != 1) break;
        value = (value << 7) | (byte & 0x7F);
    } while (byte & 0x80);
    return value;
}

int main(int argc, char *argv[]) {
    // Check command line arguments, default to "song.mid"
    const char *filename = (argc > 1) ? argv[1] : "song.mid";
    
    FILE *file = fopen(filename, "rb");
    if (!file) {
        fprintf(stderr, "Error: Cannot open input file '%s'\n", filename);
        return EXIT_FAILURE;
    }

    // Verify MIDI Header Chunk (MThd)
    char chunk_type[4];
    if (fread(chunk_type, 1, 4, file) != 4 || strncmp(chunk_type, "MThd", 4) != 0) {
        fprintf(stderr, "Error: Invalid MIDI format (MThd not found).\n");
        fclose(file);
        return EXIT_FAILURE;
    }

    read_uint32_be(file);           // Skip header length
    uint16_t format = read_uint16_be(file); 
    
    // Warn user about Format 1 potential tempo map synchronization issues
    if (format == 1) {
        fprintf(stderr, "\n[WARNING] Format 1 (Multi-Track) MIDI detected.\n");
        fprintf(stderr, "If this song contains tempo changes, timing might become inaccurate across tracks.\n");
        fprintf(stderr, "For the most precise results, please export your MIDI as Format 0 (Single Track).\n\n");
    }
    
    read_uint16_be(file);           // Skip track count
    uint16_t division = read_uint16_be(file); 
    
    if (division & 0x8000) {
        printf("/* WARNING: Unsupported SMPTE time format detected. */\n");
        division &= 0x7FFF; 
    }

    uint32_t tempo = 500000;        // Default: 120 BPM
    uint8_t last_status = 0;        
    int track_count = 0;

    printf("/* Auto-generated Frequency Array (Hz, ms) for STM32 (Source: %s) */\n", filename);
    printf("const AudioNote_t my_new_melody[] = {\n");

    // Iterate through all chunks
    while (fread(chunk_type, 1, 4, file) == 4) {
        uint32_t chunk_len = read_uint32_be(file);
        long track_end_pos = ftell(file) + chunk_len;
        
        if (strncmp(chunk_type, "MTrk", 4) == 0) {
            track_count++;
            last_status = 0; // Reset running status at the beginning of each new track to prevent bleed-over
            uint32_t current_note = 0;
            uint32_t time_accum_ms = 0;
            
            printf("\n    // --- Track %d Start ---\n", track_count);
            
            while (ftell(file) < track_end_pos) {
                uint32_t delta_ticks = read_vlq(file);
                
                // Tick to ms conversion with 64-bit overflow protection
                if (division > 0) {
                    uint64_t dividend = ((uint64_t)delta_ticks * tempo) + ((uint64_t)division * 500); // Add half divisor for rounding
                    uint64_t divisor = (uint64_t)division * 1000;
                    time_accum_ms += (uint32_t)(dividend / divisor);
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

                // Skip SysEx messages
                if (status == 0xF0 || status == 0xF7) {
                    uint32_t sysex_len = read_vlq(file);
                    fseek(file, sysex_len, SEEK_CUR);
                }
                // Handle Meta Events
                else if (status == 0xFF) { 
                    uint8_t meta_type; 
                    if (fread(&meta_type, 1, 1, file) != 1) break;
                    uint32_t meta_len = read_vlq(file);
                    if (meta_type == 0x51 && meta_len == 3) {
                        uint8_t b[3]; 
                        if (fread(b, 1, 3, file) == 3) {
                            tempo = (b[0] << 16) | (b[1] << 8) | b[2];
                        }
                    } else {
                        fseek(file, meta_len, SEEK_CUR);
                    }
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
                        // Note on with 0 velocity acts as Note Off
                        if (current_note == note && time_accum_ms > 0) {
                            printf("    {%u, %u},\n", MIDI_FREQ_TABLE[current_note & 0x7F], time_accum_ms);
                            current_note = 0;
                            time_accum_ms = 0;
                        }
                    }
                }
                // Note Off (Fixed Variable 'file')
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
                // Skip 1-byte events
                else if ((status & 0xF0) == 0xC0 || (status & 0xF0) == 0xD0) {
                    fseek(file, 1, SEEK_CUR); 
                }
                // Skip 2-byte events
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
