# midi2c
# MIDI-to-C Array Converter

A C tool created by Gemini for converting Standard MIDI Files (.mid) into C arrays for embedded systems.

## Features
- **Monophonic Extraction**: Perfectly suited for PWM buzzers on STM32, Arduino, etc.
- **CLI Support**: Easily switch input files via command line.

## Usage

### 1. Compile
Using GCC (MinGW, Linux or other):
```bash
gcc midi2c.c -o midi2c
```

### 2. Run
Process a MIDI file and display the array:
```bash
./midi2c your_song.mid
```
Or save directly to a header file:
```bash
./midi2c your_song.mid > song_data.h
```

### STM32 Integration
The output is compatible with the following structure:
```c
typedef struct {
    uint16_t frequency; // Frequency in Hz (e.g., 987 for B5). 0 indicates a rest.
    uint32_t duration;  // Duration in milliseconds
} AudioNote_t;
```

Simply paste the generated array into your project's `main.c` and use the `frequency` value directly to set your timer's ARR (Auto-Reload Register) for PWM output.

### Example
song.mid --> song.txt




