# Aegis kernel GDB init script
# Loaded automatically by: make gdb
# Connects to QEMU's GDB server on localhost:1234

set architecture i386:x86-64:intel
set pagination off
set print pretty on

# Load kernel symbols
symbol-file build/aegis.elf

# Connect to QEMU GDB stub
target remote :1234

echo \n[GDB] Connected to QEMU. Kernel halted at first instruction.\n
echo [GDB] Useful commands:\n
echo   c                           — start (continue)\n
echo   Ctrl-C                      — pause execution\n
echo   bt                          — backtrace at current position\n
echo   break isr_dispatch          — break on any exception/IRQ\n
echo   break panic_backtrace       — break when kernel panics\n
echo   info registers              — all registers\n
echo   x/20gx $rsp                 — 20 qwords from stack\n
echo   x/5i $rip                   — 5 instructions at current PC\n
echo   p *s                        — print cpu_state_t (when in isr_dispatch)\n
echo \n
echo [GDB] Serial output is captured to build/debug.log\n
echo [GDB] To resolve an address: make sym ADDR=0x<addr>\n
echo \n
