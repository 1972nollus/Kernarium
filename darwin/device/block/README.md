# Android guest storage

`uml_android_boot` (`UML::AndroidBoot`) is the host side of an Android guest's
disks. An Android guest has exactly two block devices: the release's read-only
**system disk**, attached as `ubd0r`, and the instance's writable **data
disk**, attached as `ubd1`. The producer's side of the contract, the disk
policy and the layout file, is in the
[Android README](../../../android/lineage-24/README.md#static-disk-and-boot-contract);
this README is what the host guarantees about those files. The macOS launcher
(`darwin/app/macos/android17_main.cpp`) and the Kernarium runtime
(`Kernarium/Runtime`) call the library and nothing else touches a guest's disks.

## The layout file

`readLayout` and `parseLayout` read a release's `guest/partition-layout.json`
(schema 2) with a strict parser: objects, arrays, ASCII strings, non-negative
integers and `null` only, no duplicate or unknown key, and the same rules
`tools/ubd-disk.py` applies when it writes the file (aligned, exact-fit and
back-to-back system partitions, the boot partitions present, a data template
whose names differ from the system names). The host reads the layout and
nothing else about it, so a release changes a size or an offset without a host
change, and a layout it cannot parse is a release it cannot boot.

## Partition tables

`buildGpt` builds all of a disk's metadata, the protective MBR, both headers
and both entry arrays, from a size and a partition list. The same function
creates a data disk and checks both disks: `verifyGpt` compares the disk's
metadata byte for byte with what the layout implies, so a disk another tool
touched (a different protective MBR, a regrown table, extra entries) is not a
disk of this layout. The unit test builds the release and a reference data disk
with `tools/ubd-disk.py` and requires the library to agree with it to the byte.
Payload bytes are never read at boot; the installer verified them.

## Data disk

`createDataDisk`, `growDataDisk` and `prepareDataDisk` write the instance's
disk; `validateDataDisk` checks it.

- A disk is a sparse file of a whole number of GiB within the template's range
  whose tables are exactly the template's for that size: `misc`, `metadata`,
  the reserved gap, and `userdata` filling the rest up to the free space at the
  end. Only the tables are written, so any capacity costs the same, and the
  guest formats `metadata` and `userdata` itself.
- A disk is created beside its path and linked into place, so it never replaces
  a file and a failure leaves nothing behind.
- A disk only grows. The grow is built on an APFS clone and renamed over the
  disk, so a failure leaves the old disk. A volume without clones grows it in
  place instead, and a crash then can leave a disk that fails validation. The
  old backup tables, which lie in what becomes `userdata`, are zeroed.
  The grow is refused while another process holds a lock on the disk, as a
  running guest does. The guest's filesystem grows at its next boot.
- `prepareDataDisk` is the single entry point of both products: create when
  absent (default capacity for 0), grow when asked for more, leave it
  otherwise, refuse less.

## Exactly two disks

`Storage` validates a release root and a data disk and is the only source of a
guest's disk arguments (`kernelArguments`). First-stage init takes a partition
by name from any Android GPT disk it finds and waits twenty seconds and reboots
when a name that matters is missing, so a third disk, or a second instance's,
must never be attached. The library therefore refuses:

- a kernel argument from the host's command line or from the release's
  `kernel-command-line.txt` that names a block device or a ramdisk (`ubd*`,
  `initrd*`, `root=`, `rootfstype=`), see `rejectStorageArguments` and
  `readKernelCommandLine`;
- a release command line without exactly
  `androidboot.boot_devices=uml-blkdev.0,uml-blkdev.1`;
- a system disk that is not exactly the layout's (size, tables, link), for
  example the previous single disk; and
- a data disk that is not exactly a data disk of the layout, or a path a UBD
  argument cannot carry.

Both disks are checked again at each boot, because a guest that restarts boots
on whatever the files are by then.

## Boot selection

The boot ramdisks are read in place from the system disk's `init_boot` and
`recovery` partitions (kernelless boot images, version 4). The bootloader
message of the data disk's `misc` partition picks the boot in automatic mode:
`boot-recovery` selects recovery. There is no bootloader, so a restart clears
the one-shot `bootonce-bootloader` request that `reboot bootloader` leaves,
and every restart selects its ramdisk in automatic mode whatever the first boot
was given.
