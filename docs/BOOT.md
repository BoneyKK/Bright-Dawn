# Starting the game's program

How the game's main program is placed in a model of the console and started,
what the model consists of so far, and how far the real program gets.

**The game does not run.** Its start-up code does, in the interpreter, on the
owner's ROM: through the handshake with the console's second processor, the
initialisation of graphics and sound registers, the first interrupts, reading
its file tables and overlays from the cartridge, and past the check that stops
the game on a machine that does not look like a console, and through its
requests to the second processor for its save memory and the touch screen.
It reaches its frame loop and stays in it: every frame it reads the buttons
and samples the touch screen. It sets up its sound: its commands reach a
model of the sound hardware, and the echo it builds from that hardware's
capture units runs. Nothing is drawn, nothing presses a button, and no sound
comes out, because the part of the second processor's program that plays
music is not there yet.

## The model

In `src/nds`:

| Part | State |
| --- | --- |
| Memory map of the main processor (`arm9_bus`) | Main memory, both fast memories, shared memory, palette, video and sprite memory, in their places and repeating as on the hardware. Accesses to addresses with nothing behind them are counted |
| Video memory (`video_memory`) | The nine banks, each placed by its control register: as backgrounds or sprites of either screen, or as plain memory, with the places where small banks repeat and what happens where banks overlap. Uses that give a bank no address (textures, palettes, the second processor's) take it out of reach and keep its contents. Banks handed to the 3D engine make up its texture memory (four slots of 128 KiB) and texture palette memory (six slots of 16 KiB), which can be read from here. After GBATEK's table, checked against NooDS. Nothing reads the banks for display yet |
| System coprocessor (`system_control`) | Places and enables the fast memories, including the mode in which one takes writes but not reads. Identification registers. Protection and cache registers are stored and have no effect. A request to wait for an interrupt is noted, not acted on |
| Hardware registers (`arm9_io`) | Parts of the hardware implement their registers here. A register nobody implements keeps the last value written, and every access to one is counted by address |
| Interrupt controller (`interrupts`) | The master switch, enable and request registers. When an enabled source has requested an interrupt, the master switch is on and the processor's mask is clear, the processor takes it before its next instruction. A source can also be a level, whose request stands for as long as its condition does. **Only interpreted code is interrupted so far** |
| 3D engine, front half (`geometry_engine`) | The queue of commands, both ways into it, the status register and its interrupt; the four matrices with their stacks and every matrix command; the position, vector and box tests; the registers that read the matrices back. "Swap buffers" stops the engine until the vertical blank, what is sent meanwhile queues, and the processor waits when the queue is full. Vertices are made into polygons: through the matrices, by the kind of vertex list (triangles, four-cornered polygons, strips of either), dropped when they face a way that is not drawn, cut to the view volume, placed on the screen by the viewport; each vertex with its colour and texture coordinates, each polygon with its attribute, texture and palette words. A vertex colour is sent as such or worked out from a normal, up to four lights and the material's colours. A frame holds 6144 vertices and 2048 polygons, the count register reads how many there are, and "swap buffers" hands the frame over. Commands take no time. After GBATEK; the arithmetic of lighting, facing, cutting and placing, and which vertices a strip keeps, after NooDS, except that vertices that made no polygon are not kept |
| 3D engine, back half (`polygon_renderer`) | Drawing a frame's polygons, 256 by 192, a line at a time: the clear plane; each polygon's span between its edges, with the hardware's rules for which edge pixels belong to it; depth by z or by w; colours and texture coordinates taken between the corners with perspective; all seven texture formats, read from the banks handed to the 3D engine, with repeating and mirroring; modulation, decal, toon and highlight; translucent polygons blended after the opaque ones; shadows with their masks; outlines; the alpha test; and when a line is done, edge marking, fog and anti-aliasing. Its registers, of which the control register can be read. After GBATEK's registers and NooDS's renderer, step by step. **Not here:** sorting of translucent polygons, the rear plane as a picture, the hiding of far one-dot polygons, any timing (a frame is drawn whole when asked), and resolutions other than the hardware's. **Nothing shows the drawn frame yet**: there is no 2D display hardware to put it on a screen; `bdtool boot --picture` writes it to a file |
| 2D display engine (`display_engine`) | One of the two engines that make a screen's picture, a line at a time: backgrounds of tiles (16 or 256 colours, scrolled, mirrored tiles, extended palettes), tiles or bitmaps seen through a matrix, one large bitmap, the 3D picture as background 0; 128 sprites of tiles or bitmaps, in every shape, mirrored or through a matrix, translucent or as a window; the two topmost layers by priority; two windows and the sprites' window; alpha blending, brightening and darkening, with a 3D pixel blended by its own alpha; display off, the picture, or a bank shown directly; the master brightness. After GBATEK's registers and NooDS's 2D engine. **Not here:** mosaic except down the screen on tiles and sprites, the alpha of bitmap sprites, the display fed from main memory. |
| Display (`display`) | The two engines driven by the display's timing: each visible line drawn when its dots are done, with the registers as they are then; the 3D picture drawn when a frame begins, from the polygons handed over at the vertical blank before; the display capture (the main engine's picture, the 3D picture alone, a bank, or two blended) into a bank of video memory; which engine is on which screen. Registers after GBATEK, capture after NooDS. **Not here:** the 3D picture is drawn anew only after a swap (the hardware draws it every frame), a capture source fed from main memory, switching the screens off. `bdtool boot --screens` writes both screens as last shown |
| Time (`timeline`) | Console time in system clock cycles. Each interpreted instruction is taken to use one cycle, which is coarse. When the processor sleeps, time jumps to the next event |
| Display timing (`video_timing`) | The line counter, the blanking flags, and the vertical blank, horizontal blank and line-match interrupts, at 263 lines of 2,130 cycles a frame. It tells the display when each line begins and when its dots are done |
| Divider and square root (`math_unit`) | The hardware's division in its three sizes and its square root, with what the hardware returns for division by zero and for the one division that overflows. Results are there at once |
| Timers (`timers`) | All four, with the clock dividers, reload, counting another timer's overflows, and the overflow interrupt |
| Buttons and lid (`keypad`) | The main program's button register, and the X and Y buttons and the lid sensor as the second processor passes them on. Nothing presses a button yet. The register that raises an interrupt on a button combination is not modelled |
| Channel to the second processor (`ipc`) | The synchronisation value and the two queues of words, with their status bits, error flag and interrupt requests |
| Second processor (`second_processor`) | Not a processor: a stand-in that answers the main program directly. It leaves the console's settings in memory (below), performs the start-up handshake, marks its services as ready, and receives words the way the game's second-processor program does. Of the services, four are there: requests to the cartridge (service 11), including those on the save memory, one-off samples of the touch screen (service 6), sound (service 7, next row), and the check of the second cartridge slot (service 13), which is empty. Every word for another service is recorded and not answered. See below |
| Sound program (`sound_driver`) | A stand-in for the part of the second processor's program that looks after sound: it takes the main program's lists of commands, carries out the ones that drive the hardware directly, runs the alarms, and keeps the block of memory the two programs share up to date. **It does not play sequences**, which is how the game plays its music: those commands are counted and not carried out. See below |
| Sound hardware (`sound_hardware`) | The sixteen channels (sampled sound in three formats, square waves, noise), the mixer, the two capture units and the output stage, in steps of one output sample. Registers after GBATEK, mixing arithmetic after NooDS. Its output is counted, not played: there is no audio device yet. Limits are listed in its header |
| Save memory (`save_memory`) | The cartridge's 256 KiB serial flash chip, by what its commands do: read, write, program, erase a sector. Blank at the start; nothing keeps it in a file between runs yet. Every operation completes at once |
| Console settings (`user_settings`) | Language, the owner's name, message, birthday and favourite colour, the console's network address and its wireless channels. See below |
| BIOS calls (`bios`) | Nine of the seventeen the program has wrappers for, done natively: WaitByLoop, Div, CpuSet, CpuFastSet, Sqrt, GetCRC16, IsDebugger, LZ77UnCompWram, RLUnCompWram. Written from what the calls are documented to do; the console's BIOS is not used. A call that is not implemented ends the run, as do two cases whose result on a console is not known here (division by zero, a checksum over an odd length) |
| BIOS code (`bios_rom.S`) | Three more calls, Halt, IntrWait and VBlankIntrWait, and the interrupt entry. See below |
| Sleeping | The system coprocessor's "wait for interrupt" puts the processor to sleep until an enabled source requests an interrupt. A sleep of more than ten seconds of console time ends the run |
| DMA (`dma`) | Transfers that start at once, transfers fed by the cartridge, transfers that start with the vertical blank or with the horizontal blank of each visible line, and transfers that feed the 3D engine's queue, in every direction mode, with the fill registers, repeat and the completion interrupt request. A transfer set to start on any other hardware event stays pending and is counted |
| Cartridge (`cartridge`) | The slot as a running game uses it: the "read data" and "read chip identifier" commands, the block sizes, the part of a cartridge that cannot be read, the completion interrupt, and delivery to a DMA channel. A transfer takes no console time. The encrypted exchange before a game starts is not needed and not modelled. The save memory is not reached through the slot's registers by the main program but through the second processor (below). The real program reads its file tables and overlays through it, and an overlay read this way passes the game's own check of its code |
| Program loader (`program`) | From a ROM, or from a directory written by `bdtool extract --code` |
| Start-up state (`console`) | See below |

Not present at all: video, input, and a device for the sound to come out
of.

The handshake follows what NooDS does when it stands in for the second
processor, and matches what the game's own code waits for, read from its
instructions. It has not been compared with a real console here.

## Talking to the second processor

The two programs exchange 32-bit words: a service number in the low five
bits, a flag in the next, and 26 bits of data. Everything the stand-in does
with them was read from the game's own second-processor program, which is
not run but is the reference for what the main program expects:

- Which services exist. That program registers a handler for services 4 to
  14, 16 and 17, and marks each as ready in a word in main memory, which the
  stand-in sets to `0x00037FF0` once the handshake is done. (All at once: on
  a console each bit appears as the program gets to it.)
- What happens to a word nobody handles: it is sent back with its flag set,
  or dropped if the flag was set already. Words for service 0 are dropped.
- Service 11, requests to the cartridge. The first request, "initialise",
  is two words: the request number, then the address of a block of memory
  the two programs share from then on. Each later request is one word. The
  second processor carries the request out, writes a result into the block
  and sends one word back. A request whose bit is not set in the block's
  list of allowed requests is refused, with result 3; so are requests 4, 5
  and 10, always. "Read the chip identifier" (request 3) leaves the
  identifier in the block.
- The save memory. The block says what kind the game has: for this game a
  serial flash chip of 256 KiB, with 256-byte pages, 64 KiB sectors and
  three-byte addresses. The requests are carried out as the program's
  routines for them drive the chip: prepare (2: set the chip's status
  register if the block asks for a value, which this game does not), read
  (6), write (7: replaces bytes, a page at a time), program (8: can only
  clear bits, a page at a time), verify (9: result 1 if the chip differs
  from memory), erase sectors (11: result 2 unless address and length are
  whole sectors) and read status (13). Erase everything (12), write status
  (14) and erase a smaller unit (15) are not allowed by this game's block
  and therefore refused. The chip is never busy, so the waiting and the
  time limits in those routines never come into play. A block that
  describes any other kind of save memory, or points outside main memory,
  is not acted on: the request is recorded and not answered.

- Service 6, the touch screen, shares a handler with the firmware storage,
  microphone and power services (4, 9, 8). A request to these is put
  together from 16-bit pieces, one per word, with marks on the first and
  last; its number is the high byte of the first piece. The answer carries
  the request's number and a result, and goes out on the service the
  request's number belongs to. "Sample once" (request 0) leaves a sample at
  `0x027FFFAA`: the two readings, whether the screen is touched, and
  whether each reading is usable. The second-processor program reads a
  touched screen five times in each direction and settles on a value from
  readings that agree; the stand-in has no noise, so it reports the reading
  for the touched pixel directly, with the low three bits cleared as the
  program clears them, and for an untouched screen what the program
  reports: no readings, neither usable. The touch state comes from
  `set_touch`; nothing sets it yet, so the screen is never touched. Setting
  the tolerance (request 3) is accepted. Sampling at regular intervals
  (request 1) is not implemented; stopping it (request 2) is answered as
  the program answers when it is not running.

- Service 7, sound. The main program never touches the sound hardware. It
  builds lists of commands in main memory, each command six words (the
  address of the next, a number, four arguments), and sends the address of
  a list as a word. The second processor's sound program wakes every 2,728
  timer ticks (5.2 ms), works through the lists that have arrived, and adds
  one to a count in a block of memory the main program has told it about
  (command `0x1D`), which is how the main program knows a list is done. A
  word with the data 0 means "do it now". In the same block the sound
  program reports which channels and capture units are active. There are 34
  commands. The stand-in carries out the 20 that work on the hardware, the
  alarms and the shared block, by the same arithmetic as the program's
  routines: set up, start and stop channels and capture units; volume,
  place between left and right, timer; a "surround" effect that makes
  sound at the sides quieter; master volume; what goes to the outputs;
  reserving channels; variables kept in the shared block. Of the other 14,
  13 concern sequences: the sound program is also a music player, which
  reads sequence and instrument data the main program points it to and
  plays notes on channels it picks itself. That player is not there. The
  last one copies the player's internal state out, and is not there either. A list is
  counted as finished all the same, since the main program otherwise waits
  for ever; each command not carried out is reported by `bdtool boot`.
- Alarms. Eight timers the main program can set up through commands. When
  one fires, the second processor sends a word on service 7 carrying the
  alarm's number and an identifier, once or at regular intervals. If the
  queue to the main program is full, the word is kept back and sent with
  the next firing, as the program does it.

- Service 13, the console's second cartridge slot, the one for Game Boy
  Advance cartridges. The second-processor program waits for one word from
  the main program, then compares what the slot reads back with the logo
  every such cartridge carries, notes the outcome in a flag at
  `0x027FFC35` and sends the word's number back. Nothing is in that slot
  here, so the outcome is "no such cartridge". (An empty slot is taken to
  read as all ones, as in NooDS; the comparison masks bits off its last two
  values, so it cannot succeed on all ones.) This game's main program
  ignores the answer word.

- The buttons wired to the second processor. Its program reads its own
  button register from a timer, every four milliseconds, and leaves X, Y and
  the lid sensor in a word at `0x027FFFA8` in the form the main program's
  library expects. The stand-in writes that word when the program starts and
  whenever a button or the lid changes.

The stand-in answers the moment a request arrives. On a console the second
processor needs time, during which the main program's thread sleeps. In the
run below the main program took the immediate answer and went on; whether
every request tolerates one has not been checked. The second
processor takes a lock on the cartridge while it works on a request; the
stand-in does not, and would not notice if the main program held it.

The stand-in reads and writes main memory as the second processor sees it,
without the main processor's fast memories in front.

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

## The console's settings

A console keeps its owner's settings and its wireless network address in its
firmware. A game's main program never reads the firmware: the game's
second-processor program does, as it starts, and leaves what it read near the
top of main memory. Read from that program's start-up routine in this game:

- the settings block, `0x74` bytes, at `0x027FFC80`: language, favourite
  colour, birthday, name, message, touch screen calibration, then an update
  count and a checksum;
- the network address, six bytes, at `0x027FFCF4`;
- the wireless channels it picked from the ones the firmware allows, 16 bits,
  at `0x027FFCFA`.

This project has no firmware image, so the stand-in writes all three from
values of its own (`UserSettings`): English, a player called "Player" born on
1 January, an invented network address, channels 1 to 13. The language will
become a setting; for now it can be changed in code only. Values the firmware
would not hold are refused, not corrected.

How this was checked: the block passes the two tests the second-processor
program applies before it uses one (checksum and update count), by reading
that code. The choice of channels was compared with the game's own routine,
run in the interpreter, for all 65,536 possible inputs: no difference. The
touch screen calibration is a guess that emulators also make (raw reading =
pixel times 16) and has to be looked at again when the touch screen is
modelled.

**Why it matters.** The game looks at these early. An overlay it loads in its
first frames, whose code is stored encrypted and decrypts itself as it runs,
checks its own code, fetches the network address and the owner's details, and
when the network address is all zeros erases part of its stack and its own
code and ends the program: interrupts masked, waiting for ever. No console
has that address; a model with empty memory there does. With the settings in
place the game goes on.

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

With `--wireframe FILE` it also writes the outlines of the polygons of the
last 3D frame the program sent, as an SVG picture of the screen. That is not
a rendering: nothing is filled, textured or hidden behind anything. It shows
whether the geometry arrives. With `--picture FILE` it draws that frame's
polygons and writes the 3D layer alone as a BMP picture: what the 3D hardware
would hand to the display, without backgrounds, sprites or the second
screen. Both pictures are made from your ROM's content; keep them to
yourself.

Given a ROM it puts the ROM in the cartridge slot and has the ROM header.
Given a directory of extracted code it has neither, so a program that reads
files cannot get far. Its report contains addresses and counts, no game
content.

It is a development diagnostic: the list of unmodelled registers, ordered by
use, is the work queue for the hardware model.

## What the real program does

Run in the development session on the owner's own files (European release;
kept outside the repository): first on the extracted program code alone, then
on the ROM. Interpreter only: no translated game code has been executed.

### From extracted code, without a cartridge

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

### From the ROM

- Everything above up to the idle loop happens as before. With the header and
  a cartridge, the file system code then reads from the slot: file tables,
  and overlays, which it unpacks.
- Without console settings the run ended after 1.47 million instructions,
  about a twentieth of a second of console time: the check described under "The console's settings"
  found a network address of zeros and ended the program. Seen from outside,
  that is a program asleep with interrupts masked; `bdtool boot` reported
  millions of sleeps and one interrupt taken.
- With the settings it passes the check and goes on. In ten seconds of
  console time (600 frames) it reads 120,832 bytes from the cartridge in 236
  commands, between `0x00008000` and `0x0D6723FF`, asks for the chip
  identifier 103 times, makes 19 DMA transfers, takes 676 interrupts, and
  touches no memory where nothing is mapped.
- It reads register `0x04004000` six times. That register exists only on the
  DSi; here it reads as zero.
- It sends words to the second processor: one to service 13, one to service
  7 (sound), which nothing answers, and then to service 11, the cartridge.
- Without an answer from service 11 the program's threads waited there, and
  only the idle loop and the interrupt handlers ran.
- With the cartridge service answering, the program prepares its save
  memory (request 2) and reads it: 17 read requests, 513 bytes in all. The
  chip is blank, as in a cartridge that has never been saved to. Nothing is
  written.
- It then asks the touch screen for a sample and waits for it in a loop
  that does nothing else. Without an answer the run stayed there, 336
  million instructions in ten seconds.
- With the sample answered it reaches its frame loop. Each frame it reads
  the button register, asks for a touch sample and sleeps until the next
  vertical blank. It writes to the registers of the 3D engine, starts DMA
  transfers fed by the cartridge, which run, and others timed by the
  horizontal blank, which do not yet, and reads on from the cartridge.
- It writes to its save memory: 16 times a write followed by a verify, 512
  bytes in all, with reads in between. A blank chip is being given its first
  contents.
- Before there was a keypad, the run ended after 165 frames: the program
  sent a word to service 12 with the value `0x1000`, masked every interrupt
  but "word received" and waited. That is a request to restart the game,
  and the cause was on this side: the button register read as zero, which
  means every button held down, the game's soft-reset combination among
  them.
- With the keypad, and nothing held, the frame loop runs for the whole ten
  seconds: 600 frames, 555 touch samples, 56 million instructions, the
  processor asleep for most of each frame, 2,784 interrupts taken. Besides
  the vertical blank, timer 0 and "word received" it has enabled the
  interrupt of the 3D engine's command queue.

- The three words it sends to the sound service are lists of commands.
  The first names the block of memory to share. The second sets up an
  echo: channels 1 and 3 are reserved and made to play, for ever, two
  buffers of 1,360 bytes; the two capture units record the mixer's left and
  right output into those same buffers; the outputs are taken from channels
  1 and 3 instead of the mixer; and an alarm is to fire every 5,440 ticks
  (10.4 ms), which is each time half a buffer has gone by. So everything
  the game plays passes through main memory, where the main program can
  change it, on its way to the speakers. The third list prepares a sequence
  on player 0, sets two of the player's values and starts it: the game's
  first music.
- With the sound program's stand-in, the first two lists are carried out in
  full. In ten seconds the alarm fires 950 times and the main program is
  told each time; the capture units write 1.3 million bytes, all of them
  silence, because nothing is playing: the third list's four sequence
  commands are not carried out.
- The alarm changes how the program spends its time. A thread of its own,
  which the alarm wakes, then keeps running whenever nothing more urgent
  does, so the processor no longer sleeps between frames: 335 million
  instructions in the ten seconds instead of 56 million, 4 sleeps instead
  of 626. The frame loop goes on as before, with as many touch samples and
  cartridge reads. From the code, the thread goes round a loop that calls
  one routine and sleeps only when its own priority is 14; whether a
  console spends its idle time the same way has not been checked.

- Its one word to service 13 asks whether a cartridge is in the console's
  second slot. It is told there is none, and no word the program has sent
  is left unanswered.

- The DMA transfers it starts at the horizontal blank, 430 of them in the
  ten seconds, each set to repeat, now run: 82,444 transfers of about 110
  units each, one per visible line. They write to memory, not to any of the
  registers that have no model. The program's own course is unchanged by
  them, to the instruction.

- With video memory in banks, every one of the program's accesses to it
  lands in a bank the program has placed there; none misses. At the end of
  the ten seconds banks A, B and C are with the 3D engine as textures and G
  as texture palettes, E and F are the main screen's backgrounds and
  sprites, H and I the second screen's, and D is plain memory. The controls
  of A, B, C and G are set dozens of times on the way.

- With the 3D engine's command queue, what the program sends can be read.
  In the ten seconds it ends 556 frames with "swap buffers", and sends
  947,897 vertex lists with 5.9 million vertices, 5.4 million texture
  coordinates and 3.9 million colours between them: about 1,700 lists and
  10,000 vertices a frame. It is drawing a 3D scene almost from the start.
  Its matrix commands run: 107,000 of them, with no stack error and no
  number that is not a command. It does not use the three tests. At most
  27 entries wait in the queue, behind a swap, and the processor never has
  to wait for room.
- The vertices make polygons. Of 2.2 million polygons sent in the ten
  seconds, 1.56 million are out of view and 439,000 face away; 194,000 go
  into frames, 26,000 of them cut at the edge of the view: about 350 a
  frame. The fullest frame has 3,853 vertices and 1,304 polygons, inside
  the hardware's 6144 and 2048, and nothing is lost for lack of room. No
  polygon crosses the far side with cutting there turned off. The outlines
  of the last frame (`--wireframe`) show a scene in perspective: a
  landscape with box-shaped buildings on it and a many-cornered disc above.
- It sends no normals in the ten seconds, so the hardware's lighting is not
  used here: the vertices get their colours directly. It does set up
  lights and materials. The lighting is modelled and tested, but the
  program has not yet been seen to use it.
- The frame draws. The last 3D frame of the ten seconds, drawn by the
  model, is the title screen: a textured landscape under a low sun, with
  the prompt to touch the screen and the copyright line, which the program
  draws as 3D polygons too. It was looked at, not compared with a
  console's output pixel by pixel. The program has textures, alpha
  blending, anti-aliasing and fog switched on (drawing control `0x049B`),
  clears to depth `0x7FFF`, and sets the fog and edge colour tables.
- Both screens show. After ten seconds the top screen (the second engine,
  backgrounds only) is the title logo over a sky, and the bottom screen
  (the main engine) is the 3D landscape. The main engine shows bank D
  directly (control `0x000E115D`), and the display capture fills bank D
  with the main engine's picture, 256 by 192, of which the 3D picture is
  background 0 (capture control `0x00330010`): so the 3D picture reaches
  the screen by way of video memory, a frame late. In the ten seconds
  555 3D pictures are drawn and 295 captures made. As with the 3D layer,
  this was looked at, not compared with a console.
- It uses the queue's interrupt. With the queue always drained at once,
  the condition it asks to be told of holds whenever it asks, and it takes
  that interrupt 72,000 times in the ten seconds. On a console, where
  commands take time, it would come less often.

In the window (`bright-dawn`), the title screen waits for the touch
screen, not for a button: holding Start over frames 600 to 610 leaves frame
900 exactly as without it, and a touch over the same frames brings up the
next page, a menu drawn as a book, by frame 900, its rows still empty. With
no save in the save memory the game goes on by itself to its name entry, an
on-screen keyboard on the touch screen, shown by frame 1500. The window's
picture of frame 600 matches the trace tool's, every pixel of it.

What it runs on is still thin. 20 hardware registers it uses have no
model; no sequence is played, so there is no music.

## The first run of translated game code

The game builder (`tools/gamebuilder/build_game.py`) builds
`bright-dawn-game` from the ROM: 17,076 translated functions, 395 files,
built in a few minutes on four cores. The first time it ran, the
translated code ran natively from the entry point, calling into the
interpreter where nothing was translated and back, until the first
vertical blank. There the game waits for the interrupt by putting the
processor to sleep (the system coprocessor's "wait for interrupt") in a
loop, from translated code. The interrupt is requested, so each sleep ends
at once; but translated code does not take interrupts, so the request is
never cleared, and no console time passes while translated code runs. The
game slept millions of times at cycle 408,960 (line 192 of the first
frame) and the window froze. A program that sleeps 100,000 times in a row
without time passing is now stopped, with that explanation, after one
frame.

Interrupts are now taken in translated code where it polls for them (see
ARCHITECTURE.md, "The boundary between translated code and the runtime"),
and time passes with the instructions it counts. With that, the built game
runs as far as the interpreted one: the title screen, and with a touch and a
button press, the name entry, the same pictures (the clouds and fog drift a
little differently: the two count time differently). On the development
machine, a release build: 600 frames in 11 seconds instead of 24, 1500 frames
(to the name entry) in 26 instead of 58; about 135 places are still
interpreted, among them code the game copies into the fast memories at run
time.

Getting there turned up a fault in the translator: a function whose entry
lies above its lowest address (a loop placed before the entry) started at
the wrong instruction. The game has such routines in its overlays, which
decode parts of themselves when they are loaded; one of them, started
wrongly, overwrote code of the main program. Fixed, with a guest test.

Next: playing on from the name entry, by hand and with scripted input; the
save memory kept in a file; for sound, the sequence player.

Speed, for orientation only: on the development machine a release build
ran these 600 frames in about 35 seconds, with or without the window. A
profile of 60 frames put nearly 40% of the time in copying the processor's
state out of the interpreter after every instruction. It is now read only
when a branch, an exception or the end of a run needs it, and the 600
frames take about 29 seconds, about 21 frames a second (11.6 million
instructions a second all told), with the same screens and the same
report. The console's rate needs about 34 million. 16% of the time went
to memory reads, part of them each instruction read twice: once by the
interpreter, once to look for a BIOS call.
