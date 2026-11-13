.. SPDX-License-Identifier: GPL-2.0-or-later

Reims graphics
--------------

``reims-graphics-pci`` uses the Reims graphics C API. Configure QEMU with
``--enable-reims-vgpu`` and set ``PKG_CONFIG_PATH`` to the directory containing
the matching ``reims-vgpu.pc``. The device accepts its UEFI option ROM through
the ``romfile`` property.

Windows build
~~~~~~~~~~~~~

Build the x86-64 GNU/UCRT Reims Vulkan backend with the runtime revision
pinned by its Cargo lockfile. Keep the UCRT toolchain's ``bin`` directory
first on ``PATH`` when GCC builds SDL3::

   cargo build --locked -p reims-vgpu-c --no-default-features --features vulkan

For a debug build, create ``reims-vgpu.pc`` with the following contents.
Replace the prefix with the Reims source tree's absolute path and use
forward slashes::

   prefix=C:/path/to/reims-vgpu
   libdir=${prefix}/target/debug
   includedir=${prefix}/crates/reims-vgpu-c/include
   Name: reims-vgpu
   Description: Reims graphics host library
   Version: 0.1.0
   Libs: -L${libdir} -lreims_vgpu_c
   Cflags: -I${includedir}

Configure QEMU for ``x86_64-softmmu`` with
``--enable-whpx --enable-reims-vgpu``, then run ``ninja``. Set
``libdir`` to the profile's actual output directory when using a custom
Cargo target directory. QEMU links the GNU import library on Windows.
At runtime add the directory containing ``reims_vgpu_c.dll`` to ``PATH``.

Shared RAM and a diskless check
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Use ``memory-backend-ram,share=on`` to make guest RAM available to the
renderer through file-backed views. The sparse backing file is created
in the process's temporary directory. Closing the last descriptor and
view releases the file. Windows sends control-ring notifications through
MMIO to the renderer's worker.

Set ``REIMS_VGPU_WINDOW=0`` and pass the Reims source tree's
``firmware/prebuilt/ReimsGraphics.rom`` to run the device without a
disk or running a vCPU::

   qemu-system-x86_64.exe -machine q35,memory-backend=ram,max-ram-below-4g=1G \
     -m 256M -object memory-backend-ram,id=ram,size=256M,share=on \
     -accel whpx -nodefaults -display none -S \
     -device reims-graphics-pci,romfile=ReimsGraphics.rom -qmp stdio

After the QMP greeting send ``{"execute":"qmp_capabilities"}``, then
``{"execute":"query-status"}`` and ``{"execute":"query-block"}``.
The machine should be stopped and its block-device list empty. End it
with ``{"execute":"quit"}``. Use ``-accel tcg`` for the same check with
software acceleration.

``tests/unit/test-win32-shared-ram.exe`` exercises shared-view lifetime,
large sparse-file extents and failed-allocation handle cleanup.
