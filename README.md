# midi2c
# MIDI-to-C Array Converter

A lightweight, robust C tool for converting Standard MIDI Files (.mid) into C arrays for embedded systems.

## Features
- **Monophonic Extraction**: Perfectly suited for PWM buzzers on STM32, Arduino, etc.
- **High Precision**: 64-bit timing calculations to prevent millisecond drift.
- **Robustness**: Handles Running Status, SysEx skips, and Multi-track (Format 1) files.
- **CLI Support**: Easily switch input files via command line.

## Usage

### 1. Compile
Using GCC (MinGW or Linux):
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
    uint8_t note;      // MIDI Note Number (e.g., 60 for C4)
    uint32_t duration; // Duration in milliseconds
} AudioNote_t;
```


Simply paste the generated array into your project's ```main.c``` and use your audio driver to play it.




