# STM32H753 CAN-bus Intrusion Detection – on-device GRU benchmark

## What it is

A single NUCLEO-H753ZI board both generates and detects CAN attacks. It uses FDCAN1 in
internal loopback mode, so no transceiver or second board is needed.

Detection runs the team's `traffic_gru` model through X-CUBE-AI. It classifies 4 classes:
**Normal, DoS, Fuzzy, Spoofing**. Bus-off attacks are out of scope.

## Model

- 2×GRU-64 → FC32 (ReLU) → FC4
- Input: 20 frames × 7 features (float32)
- 771,524 MACC
- Weights: about 165 KB in flash
- Activations: about 12 KB, placed in DTCM

## Features per frame

`delta_time_us`, `can_id_norm` (ID / 0x7FF), `dlc`, `payload_mean`, `payload_std`,
`is_rtr`, `id_freq_in_window`.

Windows are 20 frames with a stride of 10. Values are scaled with a StandardScaler.

## Hardware config

- System clock: 480 MHz from an 8 MHz HSE bypass
- FDCAN1: classic CAN, internal loopback, 500 kbit/s, FDCAN clock = HSE 8 MHz
- USART3: PD8/PD9 at 115200 8N1, through the ST-LINK virtual COM port
- I-cache and D-cache enabled

## Key source files (`Core/Src`)

| File | What it does |
|------|--------------|
| `ids_features.c` | Turns raw CAN frames into the 7 features per frame and builds the 20-frame windows. Copies NumPy's summation order so results are bit-identical to Python. |
| `ids_scaler.c` | StandardScaler mean and scale values (auto-generated from the Python scaler). |
| `ids_model.c` | Wrapper around the X-CUBE-AI (ST Edge AI) network: init, run inference, return the 4 logits. Puts activations in DTCM. |
| `ids_bench.c` | The benchmark: model check, feature check, timing, and loopback replay. Prints a report over USART3. |
| `ids_test_data.c` | Test data (auto-generated): replay CAN frames, PC (ONNX) reference logits, and golden scaled windows. |
| `ids_demo.c` | Live FreeRTOS attack demo. **Parked / work in progress.** |

## How to build

- Use **STM32CubeIDE 2.2.0**. It has no built-in CubeMX, so if you change the `.ioc`,
  regenerate the code with standalone **STM32CubeMX**.
- Build flags: `-O2` (not `-Ofast`).
- Linker library: `:NetworkRuntime1201_CM7_GCC.a`, with no leading spaces.
- The linker script (`STM32H753ZITX_FLASH.ld`) needs a `.dtcm_bss (NOLOAD)` section.
- `main.c` calls `ids_bench_run(&hfdcan1, &huart3);` in `USER CODE 2`.

## How to run

```sh
minicom -D /dev/ttyACM0 -b 115200
```

Turn hardware flow control off. Your user must be in the `dialout` group.

## Results so far

- **Model check:** 20/20 windows match ONNX, worst logit difference 1.9e-6.
- **Feature check:** 20/20 windows bit-identical to Python.
- **Inference:** 13.0 ms average (about 6.25 M cycles at 480 MHz).
- **Test split:** 1,048 windows (250 Normal / 250 DoS / 298 Fuzzy / 250 Spoofing).
  - PC accuracy: 99.33%
  - Board Spoofing score: 0.9858
  - Full board confusion matrix: TBD

## Known limitations

- The `delta_time` scaling makes the model largely ignore timing.
- Throughput is about 770 frames/s, which is below real bus rates. int8 quantization is the planned fix.
- Only loopback has been tested so far, not a physical bus.
- The live FreeRTOS demo is not working yet.

## Next steps

- Capture the full results
- int8 quantization
- Fix the `delta_time` scaling
- Test on a physical CAN bus
