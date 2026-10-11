.. SPDX-License-Identifier: GPL-2.0-or-later

VMApple machine emulation
========================================================================================

VMApple is the device model that the macOS built-in hypervisor called "Virtualization.framework"
exposes to Apple Silicon macOS guests. The "vmapple" machine model in QEMU implements the same
device model, but does not use any code from Virtualization.Framework.

Prerequisites
-------------

To run the vmapple machine model, you need to

 * Run on Apple Silicon
 * Run on macOS 12.0 or above
 * Have an already installed copy of a Virtualization.Framework macOS 12 or
   later virtual machine (see `Guest versions`_). I will assume that you
   installed it using the `macosvm <https://github.com/s-u/macosvm>`__ CLI.
 * Build QEMU with apple-gfx, which the machine's display needs. apple-gfx
   drives reims-vgpu's C API, a library that pkg-config finds as
   ``reims-vgpu``: pass ``--enable-pvg`` with ``PKG_CONFIG_PATH`` naming the
   directory of its ``reims-vgpu.pc``. reims-vgpu's ``cargo xtask
   dist`` writes one into its ``target/dist/lib/pkgconfig``; its ``make
   qemu`` builds that and QEMU together. Without pvg the vmapple machine is
   left out of the build.

First, we need to extract the UUID from the virtual machine that you installed. You can do this
by running the shell script in contrib/vmapple/uuid.sh on the macosvm.json file.

.. code-block:: bash
  :caption: uuid.sh script to extract the UUID from a macosvm.json file

  $ contrib/vmapple/uuid.sh "path/to/macosvm.json"

Now we also need to trim the aux partition. It contains metadata that we can just discard:

.. code-block:: bash
  :caption: Command to trim the aux file

  $ dd if="aux.img" of="aux.img.trimmed" bs=$(( 0x4000 )) skip=1

How to run
----------

Then, we can launch QEMU with the Virtualization.Framework pre-boot environment and the readily
installed target disk images. I recommend to port forward the VM's ssh and vnc ports to the host
to get better interactive access into the target system:

.. code-block:: bash
  :caption: Example execution command line

  $ UUID="$(contrib/vmapple/uuid.sh 'macosvm.json')"
  $ AVPBOOTER="/System/Library/Frameworks/Virtualization.framework/Resources/AVPBooter.vmapple2.bin"
  $ AUX="aux.img.trimmed"
  $ DISK="disk.img"
  $ qemu-system-aarch64 \
       -serial mon:stdio \
       -m 4G \
       -accel hvf \
       -M vmapple,uuid="$UUID" \
       -bios "$AVPBOOTER" \
       -drive file="$AUX",if=pflash,format=raw \
       -drive file="$DISK",if=pflash,format=raw \
       -drive file="$AUX",if=none,id=aux,format=raw \
       -drive file="$DISK",if=none,id=root,format=raw \
       -device vmapple-virtio-blk-pci,variant=aux,drive=aux \
       -device vmapple-virtio-blk-pci,variant=root,drive=root \
       -netdev user,id=net0,ipv6=off,hostfwd=tcp::2222-:22,hostfwd=tcp::5901-:5900 \
       -device virtio-net-pci,netdev=net0

Guest versions
--------------

macOS 12 and 13 guests boot without changes. macOS 14 and later need the
two below, which the machine makes when iBoot enters the kernel.

A vmapple guest kernel asks the hypervisor to manage its pointer
authentication keys through paravirtualized hypercalls: ``hvc #0`` with
SMCCC function IDs 0xC1000000 to 0xC10000FF.
Hypervisor.framework answers these calls itself, without an exit to QEMU,
and for a VM created through its public API it answers each of them as not
supported. The macOS 12 and 13 kernels carry on with the keys the firmware
set up, which all processes then share.

From macOS 14 on, the kernel checks the result of every one of these calls
and loops forever when one fails, with ``hvc #0; cbnz x0, .``.
Hypervisor.framework implements the calls only for VMs created with a
private ISA setting, which needs an entitlement that only Apple-signed
binaries can hold. QEMU does not use it. Instead, when iBoot enters the
kernel, the machine replaces each of those loops with a ``nop`` in the memory
iBoot loaded the kernel into, so the kernel carries on as macOS 13's does.
This takes the per-process keys away from the guest's pointer
authentication. Without it, such a guest stops right after iBoot hands over
to the kernel: the serial console ends with iBoot's ``End of iBootStage2
serial output.`` line, and the boot CPU keeps a host core busy without
exiting to QEMU.

The machine has no Secure Enclave. From macOS 15 on, the guest's APFS then
panics on its unencrypted data volume ("unencrypted data volume is not
allowed") unless the device tree's ``/filesystems`` node has a
``DoNotUseSEP`` property, and the guest resets and boots again, over and
over. When iBoot enters the kernel, the machine also adds such a node to the
device tree iBoot hands the kernel, if the tree has no ``/filesystems``
node. macOS 13's tree has one, which is left as it is.

To catch iBoot entering the kernel, guest RAM is not executable from reset.
Each page iBoot runs becomes executable on its first instruction fetch, until
a fetch lands between the boot arguments' ``physBase`` and
``topOfKernelData`` with ``x0`` at those boot arguments, as iBoot enters the
kernel. All of RAM is executable from then on.

Display
-------

apple-gfx gives the guest one display by default; ``-global
apple-gfx-mmio.displays=N`` gives it N, up to 8, each with a console of its
own. Under ``-display none``, reims-vgpu opens a window for each display on
the host's screen, draws the guest's frames into it, and sends its pointer,
wheel and keys to the guest's USB tablet and keyboard; closing a window
powers the VM off. Each display then runs 1920x1080 at 1x, which the window
scales to its size on the GPU, at the host screen's refresh rate;
``-global apple-gfx-mmio.resolution=WxH`` picks another mode, and
``resolution=host`` offers the modes that fill the host's screen at its
density (HiDPI on a Retina screen). ``resolution=WxH@PPI`` also tells the
guest the mode's density. A macOS 13.6 guest reads the width in whole
millimetres, rounded to nearest, then in whole centimetres, rounded up, and
runs a HiDPI desktop at the mode's pixels where that comes to 146.67 ppi or
more: ``2560x1600@148`` is told 440.39 mm, which reads as 44 cm, 147.8 ppi,
and looks like 1280x800, where ``2560x1600@146`` reads as 45 cm, 144.5
ppi, and runs at 1x. A density that does not tell the guest's scale is
refused: one just below 146.67 ppi; one below it for a mode wider than 3811
pixels, which may run HiDPI anyway (5832 pixels wide did at 128.8 ppi); one
above it for a mode narrower than 1920 pixels or 29 cm, which may stay at
1x (``1280x800@220`` did); and one so low that the width comes to more than
65535 mm, which the device's display settings do not hold. Only the chosen
mode is checked: the smaller modes offered with it are told the same size,
so a guest switched to one may run a scale the window does not expect. A
window is sized by the scale the guest runs: on a Retina screen, a HiDPI
guest's pixel is a screen pixel where half the mode fits on the screen, and
a borderless window fits the image to its screen. The device's
``display-modes`` property, where given, sets the modes instead. Set
``REIMS_VGPU_WINDOW=0`` in QEMU's environment to keep ``-display none``
headless. Either way, ``screendump`` draws the display's current frame.

Input
-----

The machine gives the guest a USB keyboard and a USB pointing device,
``usb-vmapple-tablet`` unless the machine's ``tablet`` property names another
USB device: ``-M vmapple,tablet=usb-tablet`` gives it QEMU's usual USB
tablet instead.

``usb-vmapple-tablet`` is the pointing device Virtualization.framework gives
its macOS guests: an absolute pointer with three buttons, a wheel and a
horizontal wheel, whose reports also carry smooth scrolling, that is, the
distance scrolled with the phases of the gesture it belongs to and of the
momentum scrolling after it. macOS drives it with a driver of its own
(AppleVirtualPlatformHIDEventDriver in macOS 13.6), and scrolls as it does
with a trackpad: smooth scroll events, such as a window's under ``-display
none`` or the ``scroll`` events of ``input-send-event``, scroll the guest
point by point, both ways, with the momentum the host gave the gesture. A
wheel's notch scrolls a fixed 20 pixels there. Other guests see a HID mouse
with an absolute pointer and two wheels.

With ``usb-tablet``, macOS scrolls by whole wheel notches, which it
accelerates, vertically only.
