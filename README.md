# 🎵 midi2c: MIDI-to-C Array Converter

This is a C tool created by Gemini for embedded developers (STM32, Arduino, ESP32, etc.). Its core purpose is to seamlessly convert Standard MIDI Files (`.mid`) into C-language arrays containing precise "frequency (Hz) + duration (ms)" pairs, ready to be digested by your microcontroller's PWM drivers.

The playback example below uses STM32 HAL. If you are using Arduino, simply pass the `frequency` and `duration` values into the standard `tone(pin, frequency, duration)` function.

## 🛠️ Phase 1: Converting MIDI on Your PC

### 1. Prerequisites
* **Environment:** We need a C compiler installed on your computer (e.g., `gcc` via MinGW on Windows, or any other C compilers).
* **Audio Source:** A **monophonic (single-track)** MIDI file. Since a standard passive buzzer can only play one note at a time, ensure the MIDI file does not contain chords or overlapping notes. (*A test file `song.mid` is provided in this repository*).

### 2. Compile the Converter
Open our terminal, navigate to the directory containing `midi2c.c`, and run the following command to compile the tool:

```bash
gcc midi2c.c -o midi2c
```

### 3. Generate the C Array
Once compiled, process the MIDI file. To make things easier, use the `>` operator to redirect the terminal output directly into a header (`.h`) or text (`.txt`) file:

```bash
# Syntax: ./midi2c <input_file.mid> > <output_file>
./midi2c song.mid > my_melody.h
```

Open the newly generated `my_melody.h`. We will see a clean, formatted C array. Each row represents `{frequency_in_Hz, duration_in_ms}`, where a frequency of `0` represents a rest (silence):

```c
const AudioNote_t my_new_melody[] = {
    {987, 217},
    {1108, 217},
    {0, 100}, // Rest
    // ...
};
```

---

## 🚀 Phase 2: Playing the Music on STM32

Now that we have the array, we need to configure our STM32 project to read it and drive the PWM timer.

### 1. Define the Data Structure
In our audio header file (e.g., `audio.h`), define the structure that matches the generated array:

```c
// Defines a single musical note
typedef struct {
    uint16_t frequency; // Frequency in Hz (e.g., 440 for A4)
    uint32_t duration;  // Duration in milliseconds
} AudioNote_t;
```

### 2. Import Your Melody
Copy the `my_new_melody` array generated in Phase 1 and paste it into the `main.c` (or our dedicated audio data file). 
Then, define a macro to calculate the length of the array, which is useful for loops:

```c
#define MELODY_LENGTH (sizeof(my_new_melody) / sizeof(AudioNote_t))
```

### 3. Write the PWM Driver Logic
This is the core step. A passive buzzer changes its pitch based on the PWM frequency. Assuming your Timer clock is configured to **1 MHz** (1,000,000 Hz), we can write a playback function like this using the STM32 HAL library:

```c
#include "stm32f1xx_hal.h"
extern TIM_HandleTypeDef htim2; // Assuming your buzzer is on TIM2_CH1

void Play_Single_Note(uint16_t freq) {
    if (freq == 0) {
        // A frequency of 0 indicates a rest. Set duty cycle to 0 to mute the buzzer.
        __HAL_TIM_SET_COMPARE(&htim2, TIM_CHANNEL_1, 0); 
    } else {
        // Calculate the Auto-Reload Register (ARR) value
        // Formula: ARR = (Timer_Clock_Frequency / Target_Frequency) - 1
        uint32_t arr_value = 1000000 / freq - 1;
        
        // 1. Set the pitch (Modify ARR)
        __HAL_TIM_SET_AUTORELOAD(&htim2, arr_value);
        
        // 2. Set the volume (Modify CCR for a 50% duty cycle, which is maximum volume)
        __HAL_TIM_SET_COMPARE(&htim2, TIM_CHANNEL_1, arr_value / 2); 
    }
}
```

### 4. Play it in the Main Loop
Finally, iterate through our array in the main program task to play the melody:

```c
void Play_Music(void) {
    for (int i = 0; i < MELODY_LENGTH; i++) {
        // Fetch the frequency and update the PWM timer
        Play_Single_Note(my_new_melody[i].frequency);
        
        // Hold the note for the specified duration
        HAL_Delay(my_new_melody[i].duration);
    }
    
    // Mute the buzzer when the song is finished
    Play_Single_Note(0); 
}
```

🎉 **That's it! Enjoy the music you extracted yourself!**

---

### 💡 Pro Tips
* **Tempo too fast or too slow?** If the generated melody feels off-beat, open the `midi2c.c` source code, locate the line `uint32_t tempo = 500000;`, and slightly increase or decrease this base value. Recompile the tool to adjust the global speed.
* **Notes blending together?** If two consecutive identical notes sound like one long continuous beep, you can add a tiny gap between them. Simply insert `Play_Single_Note(0); HAL_Delay(5);` at the end of your `for` loop to create a brief, crisp separation (staccato effect).




