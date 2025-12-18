That is just a proof of concept.

A NES emulator in pure C make by GPT 5.2 thinkiing high with a codex-cli in 40 minutes.

Currently games are fully playagle with almost 0 glites with games using mapper 0


<img width="789" height="773" alt="Screenshot from 2025-12-18 01-22-59" src="https://github.com/user-attachments/assets/ab480083-a5cc-448c-a0f3-853f7bf7af44" />
<img width="789" height="773" alt="Screenshot from 2025-12-18 01-23-38" src="https://github.com/user-attachments/assets/40e8cded-c547-4fe9-97f4-b8db72efac00" />
<img width="789" height="773" alt="Screenshot from 2025-12-18 01-23-54" src="https://github.com/user-attachments/assets/8a604475-4936-4c50-9624-3a6f2fb0c828" />
<img width="789" height="773" alt="Screenshot from 2025-12-18 01-24-06" src="https://github.com/user-attachments/assets/1a328970-68ce-4b78-823c-bf22aa161b2a" />
<img width="789" height="773" alt="Screenshot from 2025-12-18 01-24-15" src="https://github.com/user-attachments/assets/ebf9b412-9108-4824-939b-e2ffc1b0cf31" />



# Minimal NES Emulator (C + SDL2)

This is a small, working NES emulator focused on running simple iNES **mapper 0 (NROM)** ROMs.

## Build

```bash
make
```

## Run

```bash
./nes path/to/game.nes
```

If a game runs too fast/slow in a VM, the default build throttles to ~60 FPS. To disable throttling:

```bash
./nes --unthrottled path/to/game.nes
```

You can also force controller buttons held down (useful for headless tests):

```bash
./nes --hold-right --hold-b mario.nes
./nes --headless 600 --hold-right --hold-b mario.nes
./nes --headless 6000 --detect-freeze --tap-start 10 --hold-right mario.nes
```

Keys:
- `Z` = B, `X` = A
- `Enter` = Start, `Shift` = Select
- Arrow keys = D-pad
- `R` = reset, `Esc` = quit

## Headless mode (no window)

```bash
./nes --headless 3 path/to/game.nes
```

## Included smoke-test ROM

Generate a tiny homebrew ROM:

```bash
make hello-rom
./nes roms/hello.nes
```

Note: `tools/mk_hello_rom` is the ROM *generator* executable; the ROM it produces is `roms/hello.nes`.

## Limitations

- **Mapper support:** mapper 0 only.
- **APU/audio:** not implemented.
- **PPU accuracy:** simplified (not cycle-accurate).
