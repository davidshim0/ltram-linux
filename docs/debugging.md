## Debugging the kernel with GDB

1. Add `-s -S` to the `qemu-system-x86_64` command in `scripts/run-vm.sh
    1. `-s`: shorthand for -gdb tcp::1234, lets you connect to the vm in gdb via `target remote :1234`
    1. `-S`: pauses the CPU at startup so you can add breakpoints
1. In another pane, run `gdb linux/vmlinux`.
1. In gdb, connect to the vm with `target remote :1234` and then let the vm startup with `c` (continue).
1. `Ctrl-C` to stop the vm so you can add breakpoints with `br [function name]`, then use `c` to continue the vm.
[more gdb commands](https://darkdust.net/files/GDB%20Cheat%20Sheet.pdf)
