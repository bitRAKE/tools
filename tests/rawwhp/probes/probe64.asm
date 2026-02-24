format binary as 'bin'
use bits64
org 0

; Template for custom 64-bit probes.
; Build:
;   C:\fasm\fasm2\fasm2.cmd probe64.asm probe64.bin

main:
    hlt
