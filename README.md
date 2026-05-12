# wav_analyzer

A command-line WAV audio file analyzer written in C with no external dependencies.

## Features

- Parses standard PCM WAV files (8-bit, 16-bit, 24-bit; mono or stereo)
- Reports format metadata: channels, sample rate, bit depth, duration
- Computes peak amplitude and RMS level in both linear and dBFS
- Detects and timestamps silence regions
- Renders an ASCII waveform in the terminal

## Build

Requires a C99 compiler and the standard math library.

```bash
gcc -Wall -O2 -o wav_analyzer wav_analyzer.c -lm
```

On Windows (MinGW/MSYS2):
```bash
gcc -Wall -O2 -o wav_analyzer.exe wav_analyzer.c -lm
```

## Usage

```
wav_analyzer <file.wav> [-w] [-s]
```

| Flag | Description |
|------|-------------|
| `-w` | Show ASCII waveform visualization |
| `-s` | Show detected silence regions |

## Example output

```
=== WAV Analyzer ===

File       : recording.wav
File size  : 258.4 KB
Channels   : 1 (mono)
Sample rate: 44100 Hz
Bit depth  : 16-bit PCM
Frames     : 132300
Duration   : 0:03.00 (3.000 sec)
Peak       : 0.5000  (-6.0 dBFS)
RMS level  : 0.2887  (-10.8 dBFS)

Waveform (channel 1):
+1.0 +------------------------------------------------------------------------+
     |            ################################################            |
     |########################################################################|
     |            ################################################            |
-1.0 +------------------------------------------------------------------------+

Silence regions (below 1% of full scale):
    0.000 s ->   0.500 s  (0.500 s)
    2.500 s ->   3.000 s  (0.500 s)
```

## Supported formats

- Audio format: PCM (uncompressed)
- Bit depths: 8-bit (unsigned), 16-bit, 24-bit (signed, little-endian)
- Channels: mono, stereo, or multi-channel (waveform displays channel 1)
- Chunk layout: tolerates non-standard chunk ordering

## License

MIT License — free to use and modify.
