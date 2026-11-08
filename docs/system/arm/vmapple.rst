.. SPDX-License-Identifier: GPL-2.0-or-later

VMApple machine emulation
========================================================================================

VMApple is the device model that the macOS built-in hypervisor called "Virtualization.framework"
exposes to Apple Silicon macOS guests. The "vmapple" machine model in QEMU implements the same
device model, but does not use any code from Virtualization.Framework.

Prerequisites
-------------

To run the vmapple machine model, you need to

 * Run on Apple Silicon, on macOS 12.0 or above, for HVF; or on any host
   under TCG (see `Under TCG`_)
 * Have an already installed copy of a Virtualization.Framework macOS 12 or
   later virtual machine. I will assume that you installed it using the
   `macosvm <https://github.com/s-u/macosvm>`__ CLI.
 * Have a firmware for the machine, such as Ambrosia.
   Virtualization.framework's own, AVPBooter, does not boot on it (see
   `Guest firmware`_).
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

Then, we can launch QEMU with the firmware and the readily installed target disk images. I
recommend to port forward the VM's ssh and vnc ports to the host to get better interactive access
into the target system:

.. code-block:: bash
  :caption: Example execution command line

  $ UUID="$(contrib/vmapple/uuid.sh 'macosvm.json')"
  $ FIRMWARE="ambrosia.bin"
  $ AUX="aux.img.trimmed"
  $ DISK="disk.img"
  $ qemu-system-aarch64 \
       -serial mon:stdio \
       -m 4G \
       -accel hvf \
       -M vmapple,uuid="$UUID" \
       -bios "$FIRMWARE" \
       -drive file="$AUX",if=pflash,format=raw \
       -drive file="$DISK",if=pflash,format=raw \
       -drive file="$AUX",if=none,id=aux,format=raw \
       -drive file="$DISK",if=none,id=root,format=raw \
       -device vmapple-virtio-blk-pci,variant=aux,drive=aux \
       -device vmapple-virtio-blk-pci,variant=root,drive=root \
       -netdev user,id=net0,ipv6=off,hostfwd=tcp::2222-:22,hostfwd=tcp::5901-:5900 \
       -device virtio-net-pci,netdev=net0

The machine offers virtio-net devices without a control queue: macOS's
driver can miss the completion of its first control command, and its
interface then never comes up. Without the queue the device stays
promiscuous and passes the guest every frame its backend delivers.

Under TCG
---------

The machine also runs under TCG, on any host, with ``-accel tcg`` in place of
``-accel hvf``. Its CPU type defaults to ``max`` under both; under HVF,
``max`` is the host's CPU. Under TCG, the machine's generic timer counts at
24 MHz, the timebase frequency macOS's device tree gives every CPU, and the
frequency of Apple silicon's system counter, which HVF passes through.

The pointer authentication calls of `Guest firmware`_ reach QEMU under TCG,
whose PSCI code answers each of them as not supported, as
Hypervisor.framework does, so a guest needs the same firmware under both.

As it starts, macOS's platform driver asks Virtualization.framework's machine
for a configuration register by writing the physical address of a request to
0x004ffff8. The machine leaves the request unanswered, as HVF leaves an
access to an address with no device, and the driver goes on without the
register; ``-d unimp`` logs the write. Under TCG any other access to an
address with no device is an external abort, which the kernel does not
survive. So the firmware's device tree must not list a device that the
machine lacks and macOS probes, such as Virtualization.framework's CTRR
device at 0x30260000.

Guest firmware
--------------

The machine's RAM starts at 4 GiB, and below it the PCIe MMIO window runs
from 0x50000000 up to RAM. macOS places every PCI BAR below 4 GiB, aligned
to its size, so a device with a 1 GiB BAR needs a free, 1 GiB-aligned
gigabyte there. Virtualization.framework's machine has its RAM at
0x70000000, and its firmware, AVPBooter, keeps its stack there, so AVPBooter
does not boot on this machine. A firmware for it gives macOS this layout in
the device tree; Ambrosia is one.

The firmware also makes two changes that macOS needs on this machine before
it enters the kernel. The machine makes no change to the guest itself.

A vmapple guest kernel asks the hypervisor to manage its pointer
authentication keys through paravirtualized hypercalls: ``hvc #0`` with
SMCCC function IDs 0xC1000000 to 0xC10000FF. Hypervisor.framework answers
these calls itself, without an exit to QEMU, and for a VM created through
its public API it answers each of them as not supported. The macOS 12 and
13 kernels carry on with the keys the firmware set up, which all processes
then share. From macOS 14 on, the kernel checks the result of every one of
these calls and loops forever when one fails, with ``hvc #0; cbnz x0, .``.
Hypervisor.framework implements the calls only for VMs created with a
private ISA setting, which needs an entitlement that only Apple-signed
binaries can hold. QEMU does not use it. So the firmware replaces each of
those loops with a ``nop``.

The machine has no Secure Enclave. From macOS 15 on, the guest's APFS then
panics on its unencrypted data volume ("unencrypted data volume is not
allowed") unless the device tree's ``/filesystems`` node has a
``DoNotUseSEP`` property, so the firmware adds one.

Clock
-----

Beside its PL031 at 0x20050000, the machine has Virtualization.framework's
second clock at 0x30240000, which macOS's device tree lists as ``avp-rtc``.
macOS keeps the offset from it to calendar time in NVRAM, on the aux drive,
and its timed daemon reads that offset to tell whether the clock was reset.
macOS's PL031 driver keeps no offset: with the PL031 alone, timed takes the
clock for reset on every boot, and sets it back to the last write to the
guest's disk until it reaches a time server. Both clocks count QEMU's RTC
clock from the Unix epoch, so ``-rtc base=`` and ``-rtc clock=`` apply to
both. The firmware's device tree gives the guest one of the two. macOS 14
and later drive ``avp-rtc``; the device trees of earlier releases list only
the PL031.

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
