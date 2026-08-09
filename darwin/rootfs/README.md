# Legacy rootfs payload fragments

These files are retained as historical payload inputs for older minimal
images. They are not an acquisition, verification, test, or maintenance entry
point for the unified `rootfs.erofs` artifact.

The canonical repository-owned workflow is under [`../../tools/rootfs`](../../tools/rootfs):

- `provision.py` verifies a local artifact against the pinned image digest, or
  authenticates a network release statement before verifying that digest;
- `verify.py` checks the pinned digest and raw EROFS superblock contract;
- `with-verified-rootfs.sh` wraps every ordinary guest campaign; and
- `provision.py --replace` atomically installs the verified replacement.

EROFS images are rebuilt as complete artifacts; there is no in-place writable
maintenance path. The artifact also owns tty0 getty enablement and login
policy; launchers must not inject a replacement getty unit into their tmpfs
overlay. Systemd OverlayFS launchers mount a separate tmpfs at
`/var/lib/docker` before systemd starts so Docker storage is not nested on the
root overlay. Desktop and WebGL launchers reuse the image's non-root `uml`
account and never create a `plasma` account in the overlay.

No build or test target consumes the scripts in this directory. Keep them
data-only unless a future image-construction workflow deliberately adopts and
tests them under `tools/rootfs`.
