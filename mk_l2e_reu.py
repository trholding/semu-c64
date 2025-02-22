#!/usr/bin/env python

l2e_riscv_bm = open("runq_semu.bin", "rb").read()
dtb = open("minimal.dtb", "rb").read()

REUSIZE = 2 * 1024 * 1024
DTB_SIZE = 16384

# Write the REU file
with open("l2e_c64.reu", "wb") as outf:
    outf.write(l2e_riscv_bm)
    outf.write(b"\x00" * (REUSIZE - len(l2e_riscv_bm) - DTB_SIZE))
    outf.write(dtb)
    outf.write(b"\x00" * (DTB_SIZE - len(dtb)))
