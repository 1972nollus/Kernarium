# Legacy rootfs payload fragments

These files are retained as historical payload inputs for older minimal
images. They are not an acquisition, verification, test, or maintenance entry
point for the unified `rootfs.erofs` artifact.

Optional artifact installation support is under
[`../../tools/rootfs`](../../tools/rootfs):

- `provision.py` authenticates a local candidate against the pinned manifest or
  verifies a signed network release statement, then installs it atomically;
- `verify.py` owns the provisioning-time digest and raw EROFS checks.

Ordinary guest campaigns use `rootfs.erofs` directly. They do not hash it or
take a sidecar lock before and after execution; EROFS itself supplies the
read-only filesystem contract.

EROFS images are rebuilt as complete artifacts; there is no in-place writable
maintenance path. The artifact also owns tty0 getty enablement and login
policy; launchers must not inject a replacement getty unit into their tmpfs
overlay. Systemd OverlayFS launchers mount separate tmpfs filesystems at
`/var/lib/docker` and `/var/lib/containerd` before systemd starts so Docker and
containerd state are not nested on the root overlay. The systemd integration
smoke imports a local rootfs archive containing its source-built,
freestanding arm64/Linux hello-world payload into that disposable state and
runs it with `--pull=never --network none`; it never needs a registry. Desktop
and WebGL launchers reuse the image's non-root `uml` account and never create a
`plasma` account in the overlay.

No build or test target consumes the scripts in this directory. Keep them
data-only unless a future image-construction workflow deliberately adopts and
tests them under `tools/rootfs`.
