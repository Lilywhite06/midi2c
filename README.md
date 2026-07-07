# 🎵 midi2c: MIDI-to-C Array Converter

This is a C tool created by Gemini for embedded developers (STM32, Arduino, ESP32, etc.). Its core purpose is to seamlessly convert Standard MIDI Files (`.mid`) into C-language arrays containing precise "frequency (Hz) + duration (ms)" pairs, ready to be digested by your microcontroller's PWM drivers.

The playback example below uses STM32 HAL. If you are using Arduino, simply pass the `frequency` and `duration` values into the standard `tone(pin, frequency, duration)` function.

---

## 🛠️ Phase 1: Converting MIDI on Your PC

### 1. Prerequisites
* **Environment:** A C compiler installed on your computer (e.g., `gcc` via MinGW on Windows, or any other C compiler).
* **Audio Source:** A **monophonic (single-track)** MIDI file is recommended, since a standard passive buzzer can only play one note at a time. The converter supports two monophonic strategies; see [Mode Selection](#mode-selection) below for details. (*A test file `song.mid` is provided in this repository*).

### 2. Compile the Converter
Open your terminal, navigate to the directory containing `midi2c.c`, and run:

```bash
gcc midi2c.c -o midi2c
```

### 3. Generate the C Array
The basic usage outputs the array directly to the terminal. Use `>` to redirect it into a header file.

```bash
# Basic usage (legato mode by default)
./midi2c song.mid > my_melody.h

# Specify a track (1‑based)
./midi2c -t 1 song.mid > melody_track1.h

# Use "grab" mode for simple single‑voice buzzers
./midi2c --mode grab song.mid > my_melody.h
```

Open the generated `my_melody.h`. You will see a clean, formatted C array. Each row is `{frequency_in_Hz, duration_in_ms}`; a frequency of `0` represents a rest (silence):

```c
const AudioNote_t my_new_melody[] = {
    {987, 217},
    {1108, 217},
    {0, 100}, // Rest
    // ...
};
```

#### Mode Selection

The converter offers two monophonic conversion modes. **Choose the one that matches your hardware and musical intent.**

| Mode (`--mode`) | Behavior | When to use |
|-----------------|----------|-------------|
| `legato` *(default)* | **Last‑note priority with fallback.** When you release the latest note, the previously held note resumes playing. | Preserves musical legato/trill phrasing from MIDI files with overlapping notes. Ideal for PWM synthesizers that can smoothly transition between notes. |
| `grab` | **New note steals, release silences.** Every new note immediately cuts off the previous one. When the currently sounding note is released, the buzzer goes silent – even if other notes are still held. | Perfect for simple single‑voice buzzers where only the most recent key press can be heard (e.g., a doorbell‑style beeper). |

**Example:**  
If your melody sounds “messy” or contains unexpected ghost notes, try switching to `--mode grab`.

---

## 🚀 Phase 2: Playing the Music on STM32

Now that we have the array, we need to configure our STM32 project to read it and drive the PWM timer.

### 1. Define the Data Structure
In your audio header file (e.g., `audio.h`), define the structure that matches the generated array:

```c
// Defines a single musical note
typedef struct {
    uint16_t frequency; // Frequency in Hz (e.g., 440 for A4)
    uint32_t duration;  // Duration in milliseconds
} AudioNote_t;
```

### 2. Import Your Melody
Copy the `my_new_melody` array generated in Phase 1 and paste it into `main.c` (or a dedicated audio data file).  
Then, define a macro to calculate the array length:

```c
#define MELODY_LENGTH (sizeof(my_new_melody) / sizeof(AudioNote_t))
```

### 3. Write the PWM Driver Logic
Assuming your Timer clock is configured to **1 MHz** (1,000,000 Hz), use this function (STM32 HAL):

```c
#include "stm32f1xx_hal.h"
extern TIM_HandleTypeDef htim2; // Assuming your buzzer is on TIM2_CH1

void Play_Single_Note(uint16_t freq) {
    if (freq == 0) {
        // Rest: set duty cycle to 0
        __HAL_TIM_SET_COMPARE(&htim2, TIM_CHANNEL_1, 0); 
    } else {
        // Calculate Auto-Reload Register (ARR)
        // Formula: ARR = (Timer_Clock_Frequency / Target_Frequency) - 1
        uint32_t arr_value = 1000000 / freq - 1;
        
        // Set pitch (ARR)
        __HAL_TIM_SET_AUTORELOAD(&htim2, arr_value);
        
        // Set volume (50% duty cycle for maximum loudness)
        __HAL_TIM_SET_COMPARE(&htim2, TIM_CHANNEL_1, arr_value / 2); 
    }
}
```

### 4. Play in the Main Loop
Iterate through the array in your main task:

```c
void Play_Music(void) {
    for (int i = 0; i < MELODY_LENGTH; i++) {
        Play_Single_Note(my_new_melody[i].frequency);
        HAL_Delay(my_new_melody[i].duration);
    }
    
    // Mute the buzzer when the song finishes
    Play_Single_Note(0); 
}
```

---

## 💡 Pro Tips

* **Tempo too fast or too slow?**  
  Open `midi2c.c`, locate the line `uint32_t current_tempo = 500000;` (default 120 BPM), and adjust this base value. Recompile the tool to change the global playback speed.

* **Notes blending together?**  
  If two consecutive identical notes sound like one continuous beep, insert a tiny gap between them:  
  ```c
  Play_Single_Note(0);
  HAL_Delay(5); // 5 ms of silence
  ```

* **Choosing the right mode:**  
  If your MIDI file contains intentional overlaps (e.g., a held bass note with a quick melody), `legato` mode will preserve that musical phrasing. For the simplest single‑buzzer setups where you only want the most recently pressed key to sound, use `grab` mode.

---

🎉 **That’s it! Enjoy the music you extracted yourself!**



