# Starting the game's program

How the game's main program is placed in a model of the console and started,
what the model consists of so far, and how far the real program gets.

**The game does not run.** Its start-up code does, in the interpreter: through
the handshake with the console's second processor, the initialisation of
graphics and sound registers and the first interrupts, into the program's
first attempt to read a file. That is as far as it can be taken in the
development environment, where there is program code but no cartridge. The
next step is a run of `bdtool boot` on the owner's ROM.

## The model

In `src/nds`:

| Part | State |
| --- | --- |
| Memory map of the main processor (`arm9_bus`) | Main memory, both fast memories, shared memory, palette, video and sprite memory, in their places and repeating as on the hardware. Video memory is plain memory, not banked. Accesses to addresses with nothing behind them are counted |
| System coprocessor (`system_control`) | Places and enables the fast memories, including the mode in which one takes writes but not reads. Identification registers. Protection and cache registers are stored and have no effect. A request to wait for an interrupt is noted, not acted on |
| Hardware registers (`arm9_io`) | Parts of the hardware implement their registers here. A register nobody implements keeps the last value written, and every access to one is counted by address |
| Interrupt controller (`interrupts`) | The master switch, enable and request registers. When an enabled source has requested an interrupt, the master switch is on and the processor's mask is clear, the processor takes it before its next instruction. **Only interpreted code is interrupted so far** |
| Time (`timeline`) | Console time in system clock cycles. Each interpreted instruction is taken to use one cycle, which is coarse. When the processor sleeps, time jumps to the next event |
| Display timing (`video_timing`) | The line counter, the blanking flags, and the vertical blank, horizontal blank and line-match interrupts, at 263 lines of 2,130 cycles a frame. Nothing is drawn |
| Divider and square root (`math_unit`) | The hardware's division in its three sizes and its square root, with what the hardware returns for division by zero and for the one division that overflows. Results are there at once |
| Timers (`timers`) | All four, with the clock dividers, reload, counting another timer's overflows, and the overflow interrupt |
| Channel to the second processor (`ipc`) | The synchronisation value and the two queues of words, with their status bits, error flag and interrupt requests |
| Second processor (`second_processor`) | Not a processor: a stand-in that answers the main program directly. It performs the start-up handshake and marks its services as ready. Every word the main program sends afterwards is recorded and not answered |
| BIOS calls (`bios`) | Nine of the seventeen the program has wrappers for, done natively: WaitByLoop, Div, CpuSet, CpuFastSet, Sqrt, GetCRC16, IsDebugger, LZ77UnCompWram, RLUnCompWram. Written from what the calls are documented to do; the console's BIOS is not used. A call that is not implemented ends the run, as do two cases whose result on a console is not known here (division by zero, a checksum over an odd length) |
| BIOS code (`bios_rom.S`) | Three more calls, Halt, IntrWait and VBlankIntrWait, and the interrupt entry. See below |
| Sleeping | The system coprocessor's "wait for interrupt" puts the processor to sleep until an enabled source requests an interrupt. A sleep of more than ten seconds of console time ends the run |
| DMA (`dma`) | Transfers that start at once, and transfers fed by the cartridge, in every direction mode, with the fill registers, repeat and the completion interrupt request. A transfer set to start on any other hardware event stays pending and is counted |
| Cartridge (`cartridge`) | The slot as a running game uses it: the "read data" and "read chip identifier" commands, the block sizes, the part of a cartridge that cannot be read, the completion interrupt, and delivery to a DMA channel. A transfer takes no console time. The encrypted exchange before a game starts is not needed and not modelled; the save memory is not modelled. **Tested on generated data only: no real program has used it yet** |
| Program loader (`program`) | From a ROM, or from a directory written by `bdtool extract --code` |
| Start-up state (`console`) | See below |

Not present at all: video, sound, input, the cartridge's save memory.
Interrupts in translated code are not present either: the tool that exists
runs everything in the interpreter.

The handshake and the "services ready" value follow what NooDS does when it
stands in for the second processor; the handshake also matches what the
game's own code waits for, read from its instructions. Neither has been
compared with a real console here.

## The code where the BIOS would be

Most BIOS calls compute something and return, and are done natively. Four
things cannot be: the program's own interrupt handler runs in the middle of
them. They are the entry into an interrupt, which calls that handler, and the
three calls that wait for an interrupt.

For those the console has 304 bytes of code at the address of the real BIOS,
`0xFFFF0000`, and the processor takes the exception for real: it changes mode
and continues at the vector, as on hardware. The code is this project's own
(`src/nds/bios_rom.S`); it is not Nintendo's BIOS and contains none of it. It
does what those parts of a BIOS are known to do:

- An interrupt: save r0-r3, r12 and the return address, call the handler whose
  address the program keeps in the last word of the data memory, restore,
  return.
- Halt: sleep until an interrupt is requested.
- IntrWait and VBlankIntrWait: sleep until the program's handler has recorded
  one of the wanted interrupts in the word before its own address, then clear
  it. As on the console's main processor, asking not to discard interrupts
  already recorded does not return at once: at least one interrupt is waited
  for.

Its numbers are embedded in the program, and a test assembles the source and
fails if they differ.

What this rests on: the source of NooDS, which does the same natively, and
recollection of GBATEK, which could not be reached from the development
environment for this part. One detail is from recollection alone and marked
so in the source: that IntrWait turns the interrupt master switch on.

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

`bdtool boot` runs the start-up code in the interpreter until a number of
instructions have run or a number of frames have passed (`--instructions N`,
`--frames F`; by default a thousand million and 600, which is ten seconds of
console time and can take a minute to run). It reports where it stopped and
why, the time that passed, the processor's registers, the system
coprocessor's state, BIOS calls, interrupts and timers, what was sent to the
second processor, DMA, what was read from the cartridge, every hardware
register used that has no model with its access counts, and memory accessed
where nothing is mapped.

Given a ROM it puts the ROM in the cartridge slot and has the ROM header.
Given a directory of extracted code it has neither, so a program that reads
files cannot get far. Its output contains addresses and counts, no game
content.

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
- After the handshake it makes its first BIOS call, CpuSet. Without a BIOS
  the run ended there, after 589,341 instructions.
- With the BIOS calls and DMA above it goes on to set up the machine: the
  display control registers of both screens, the video memory banks, the
  registers of the 3D engine, the hardware divider and square root, power
  control, a timer. Within 30 million instructions it uses 134 registers that
  have no model, makes two BIOS calls (CpuSet and WaitByLoop, once each) and
  five DMA transfers, all of the kind that starts at once.
- It sends two words to the second processor: one to service 13 and one to
  service 7 (sound). Nothing answers them.
- It turns the interrupt master switch on, enables four sources (vertical
  blank, timer 0, source 13 and "word received") and unmasks interrupts in
  the processor.
- It then comes to rest in its idle loop: a thread that does nothing but ask
  the system coprocessor to wait for an interrupt. Without interrupts the run
  stayed there.
- With interrupts it sleeps once, is woken, and goes on. In 2.4 seconds of
  console time it takes 159 interrupts, from the vertical blank and from
  timer 0, which the program runs as its own clock. It uses the hardware
  divider, makes more DMA transfers, and enters file system code: it reads a
  file in blocks and unpacks what it reads with a routine it keeps in the
  fast instruction memory.
- This run had neither a cartridge nor the ROM header the program reads its
  file tables from, so what it unpacks is not data. After 79.7 million
  instructions and 2.4 seconds of console time it jumps to an address where
  there is no memory, and the run ends there.
- It never sent a command to the cartridge slot on the way. Whether that is
  because the missing header stops the file system before it gets that far,
  or because this program reads files some other way, cannot be told without
  the ROM.

The program has wrappers for seventeen BIOS calls: numbers 0x00, 0x03 to
0x06, 0x09 and 0x0B to 0x15. That bounds the BIOS work.

Next: a run on the real ROM. The cartridge slot is modelled now, and
`bdtool boot` given a ROM puts the ROM in it and has the header, so file
reads can return the game's data. That cannot be tried in the development
environment, because the cartridge's contents exist only on the owner's
machine: the report of `bdtool boot` on the ROM is how each further step
will be found. After that: the second processor's answers, and interrupts in
translated code.

Speed, for orientation only: the interpreter runs about 20 million
instructions per second in a release build on the development machine.
