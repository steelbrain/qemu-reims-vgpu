.. SPDX-License-Identifier: GPL-2.0-or-later

Apple paravirtual graphics
-------------------------

``apple-gfx-pci`` and ``apple-gfx-iosurface-pci`` use the matching
``reims-vgpu`` C API library. Configure QEMU with ``--enable-pvg`` and
``PKG_CONFIG_PATH`` pointing to the directory containing ``reims-vgpu.pc``.
The option ROM is compiled from that library's ``firmware`` sources.

The separate ``reims-graphics-pci`` device needs a newer C API. Configure
checks the installed header before enabling it on supported hosts and
reports that capability separately. A stable Apple PVG library remains
usable without that independent API. Windows currently supports the
Apple PVG devices.

Windows build
~~~~~~~~~~~~~

Use the x86-64 Windows GNU/UCRT build of the Reims Vulkan backend and its
matching Metal-to-Vulkan runtime. Build the runtime first, then Reims,
then QEMU. Reims' ``Cargo.toml`` pins and ``Cargo.lock`` must name the same
reviewed runtime Git revision. In the Reims source tree, build and test
the library as described in ``tests/windows-platform/README.md``; keep
the lockfile fixed during the build::

   cargo build --locked -p reims-vgpu-c --no-default-features --features vulkan

Keep the UCRT toolchain's ``bin`` first on ``PATH`` when GCC builds SDL3;
its runtime DLLs must match GCC's subprocesses. Build the firmware with
clang and lld-link. In an MSYS2 shell::

   MSYS2_ARG_CONV_EXCL='*' make -C firmware efi
   cd firmware
   clang --target=x86_64-unknown-none-elf \
     -DEFI_IMAGE='"build/ReimsVgpu.efi"' \
     -c src/OptionRom.S -o build/obj/OptionRom.o
   llvm-readobj --relocations build/obj/OptionRom.o
   llvm-objcopy -O binary build/obj/OptionRom.o build/ReimsVgpu.rom

``OptionRom.S`` contains the ROM section and embedded EFI image without
relocations; the inspection above must show an empty relocation list.
Extracting that section handles Windows LLVM installations whose linker
does not implement ``--oformat binary``.

For a debug Cargo build, create ``reims-vgpu.pc`` with the following
contents, replacing the prefix with the source tree's absolute path.
Use forward slashes in its paths::

   prefix=C:/path/to/reims-vgpu
   libdir=${prefix}/target/debug
   includedir=${prefix}/crates/reims-vgpu-c/include
   optionrom=${prefix}/firmware/build/ReimsVgpu.rom
   Name: reims-vgpu
   Description: Apple PVG host library
   Version: 0.1.0
   Libs: -L${libdir} -lreims_vgpu_c
   Cflags: -I${includedir}

Set ``PKG_CONFIG_PATH`` to that file's directory, configure QEMU for
``x86_64-softmmu`` with ``--enable-whpx --enable-pvg``, and run ``ninja``.
At runtime, add the directory containing ``reims_vgpu_c.dll`` to ``PATH``.
Windows links the GNU import library; Unix packages continue to link
statically. A release or custom target directory needs the corresponding
``libdir`` in the package file.

Shared RAM and a diskless device check
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

On Windows, back machine RAM with ``memory-backend-ram,share=on`` so WHPX
and the GPU's independent aliases see the same file pages. This backend
has no ``reserve`` QOM property. Its sparse temporary file uses the
process's temporary directory and is deleted after its last view and
descriptor close.

The following check creates the devices without a disk, native display
window or running vCPU::

   REIMS_VGPU_WINDOW=0 qemu-system-x86_64.exe \
     -machine q35,memory-backend=ram -m 256M \
     -object memory-backend-ram,id=ram,size=256M,share=on \
     -accel whpx -nodefaults -display none -S \
     -device apple-gfx-pci -device apple-gfx-iosurface-pci -qmp stdio

After the QMP greeting, send ``{"execute":"qmp_capabilities"}``, then
``{"execute":"query-status"}`` and ``{"execute":"query-block"}``.
The machine must be stopped and its block-device list empty. End it with
``{"execute":"quit"}``. ``-accel tcg`` provides a software-accelerator
device check on the same host; neither check boots a guest.

``tests/unit/test-win32-shared-ram.exe`` exercises shared-view lifetime,
large sparse-file extents and failed-allocation handle cleanup. Guest
boot, application rendering and native-window performance remain
separate integration checks.
