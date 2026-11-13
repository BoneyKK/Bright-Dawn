# Starting the game's program

How the game's main program is placed in a model of the console and started,
what the model consists of so far, and how far the real program gets.

**The game does not run.** Its start-up code does, in the interpreter, through
the handshake with the console's second processor and up to its first BIOS
call.

## The model

In `src/nds`:

| Part | State |
| --- | --- |
| Memory map of the main processor (`arm9_bus`) | Main memory, both fast memories, shared memory, palette, video and sprite memory, in their places and repeating as on the hardware. Video memory is plain memory, not banked. Accesses to addresses with nothing behind them are counted |
| System coprocessor (`system_control`) | Places and enables the fast memories, including the mode in which one takes writes but not reads. Identification registers. Protection and cache registers are stored and have no effect. A request to wait for an interrupt is noted, not acted on |
| Hardware registers (`arm9_io`) | Parts of the hardware implement their registers here. A register nobody implements keeps the last value written, and every access to one is counted by address |
| Interrupt controller (`interrupts`) | The master switch, enable and request registers. Sources can request an interrupt; **the processor never takes one yet** |
| Channel to the second processor (`ipc`) | The synchronisation value and the two queues of words, with their status bits, error flag and interrupt requests |
| Second processor (`second_processor`) | Not a processor: a stand-in that answers the main program directly. It performs the start-up handshake and marks its services as ready. Every word the main program sends afterwards is recorded and not answered |
| BIOS calls | None. A software interrupt ends the run |
| Program loader (`program`) | From a ROM, or from a directory written by `bdtool extract --code` |
| Start-up state (`console`) | See below |

Not present at all: taking interrupts, timing, DMA, the cartridge, video,
sound, input.

The handshake and the "services ready" value follow what NooDS does when it
stands in for the second processor; the handshake also matches what the
game's own code waits for, read from its instructions. Neither has been
compared with a real console here.

## The state a game starts in

A real console runs its firmware first, which loads the game and leaves
registers and memory in a known state. The model sets that state directly:

- System coprocessor: control `0x0005707D`, data memory 16 KiB at
  `0x03000000`, instruction memory over the first 32 MiB.
- Processor in System mode with interrupts masked; stack pointers
  `0x03002F7C` (System), `0x03003F80` (IRQ), `0x03003FC0` (Supervisor).
- The first `0x170` bytes of the ROM header copied to `0x027FFE00`, when the
  header is available (it is not when starting from an extracted directory).
- A few values at the top of main memory (cartridge chip identifier, a
  checksum of the second processor's BIOS, start-up flags), and three
  hardware registers.
- The decompressed main program at its load address, with the field of its
  start-up parameters that says where the compressed data ends set to zero.
  Without that, the program's own start-up code would run its decompressor
  over the already decompressed program.

Where the values come from: they are what emulators set when they skip the
firmware, checked against the source of NooDS, and what GBATEK documents. They
have not been compared with a real console here.

Known to be missing from that state: the user settings the firmware copies
from its storage (language, names, calibration) at `0x027FFC80`.

## The trace tool

```bash
bdtool boot path/to/your-rom.nds
```

`bdtool boot` runs the start-up code in the interpreter for a number of
instructions (`--instructions N`, default ten million) and reports where it
stopped, the processor's registers, the system coprocessor's state, every
software interrupt, every hardware register used that has no model with its
access counts, and memory accessed where nothing is mapped. It accepts a ROM
or an extracted code directory. Its output contains addresses and counts, no
game content.

It is a development diagnostic: the list of unmodelled registers, ordered by
use, is the work queue for the hardware model.

## What the real program does

Run in the development session on the program files the owner supplied
(European release; the files are kept outside the repository). Interpreter
only: no translated game code has been executed.

- The start-up code completes. It masks interrupts, waits for the vertical
  counter to read zero, configures the system coprocessor, gives the
  Supervisor, IRQ and System modes their stacks, clears memory, copies its
  two extra blocks into the fast memories, and calls into the main program.
  Afterwards the data memory is at `0x027E0000` and the System stack pointer
  inside it, as the program's own layout expects.
- No software interrupt is made on the way, no memory is accessed where
  nothing is mapped, and no coprocessor register is used that the model does
  not know.
- It then sets the queue control register to `0xC408`, enables the "word
  received" interrupt (and only that one), and performs the handshake with
  the second processor on register `0x04000180`. With nothing answering, it
  stayed in that loop: after 20 million instructions, 2.4 million reads and
  2,422 writes. With the stand-in answering, it goes on.
- After the handshake it writes to the registers of DMA channel 0, to the
  memory control register `0x04000204` and to the shared-memory control
  register `0x04000247`, and after 589,341 instructions makes its first BIOS
  call: number `0x0B`, which copies or fills memory. The run ends there,
  because there is no BIOS.
- Up to that point it has sent nothing through the queue to the second
  processor.

Next: BIOS calls, DMA, and taking interrupts. The plan for the second
processor, from ASSESSMENT.md, is not to run its program but to answer the
main program's requests natively.

Speed, for orientation only: the interpreter runs about 20 million
instructions per second in a release build on the development machine.
