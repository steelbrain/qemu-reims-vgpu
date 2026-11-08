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
   later virtual machine. macOS 14 and later need a firmware other than
   AVPBooter (see `Guest versions`_). I will assume that you installed it
   using the `macosvm <https://github.com/s-u/macosvm>`__ CLI.
 * Build QEMU with a crypto library (gnutls, nettle or libgcrypt), whose AES
   ciphers the machine's AES engine runs on. Without one, the engine fails to
   realize and QEMU exits at start.

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

macOS 12 and 13 guests boot. A vmapple guest kernel asks the hypervisor to
manage its pointer authentication keys through paravirtualized hypercalls:
``hvc #0`` with SMCCC function IDs 0xC1000000 to 0xC10000FF.
Hypervisor.framework answers these calls itself, without an exit to QEMU,
and for a VM created through its public API it answers each of them as not
supported. The macOS 12 and 13 kernels carry on with the keys the firmware
set up, which all processes then share.

From macOS 14 on, the kernel checks the result of every one of these calls
and loops forever when one fails, with ``hvc #0; cbnz x0, .``.
Hypervisor.framework implements the calls only for VMs created with a
private ISA setting, which needs an entitlement that only Apple-signed
binaries can hold. QEMU does not use it. On AVPBooter, such a guest stops
right after iBoot hands over to the kernel: the serial console ends with
iBoot's ``End of iBootStage2 serial output.`` line, and the boot CPU keeps a
host core busy without exiting to QEMU.

The machine has no Secure Enclave. From macOS 15 on, the guest's APFS then
panics on its unencrypted data volume ("unencrypted data volume is not
allowed") unless the device tree's ``/filesystems`` node has a
``DoNotUseSEP`` property.

So macOS 14 and later boot only with a firmware, given with ``-bios`` in
place of AVPBooter, that replaces each of those loops with a ``nop`` and
adds that node before it enters the kernel. Ambrosia is one. The machine
makes no change to the guest itself.

Display
-------

The machine gives the guest no display. Virtualization.framework's
paravirtualized display, which macOS drives with a driver of its own, is not
in this tree. A guest has a display only from a PCI display device given with
``-device`` that the guest has a driver for. Without one, reach the guest
through the serial console, or through ``ssh`` on host port 2222, which the
example forwards to the guest's port 22.

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
