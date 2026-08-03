# Legacy rootfs payload fragments

These files are retained as historical payload inputs for the older minimal
rootfs and as recovery reference for the immutable image. They are not an
acquisition, verification, test, or writable-maintenance entry point.

The canonical repository-owned workflow is under [`../../tools/rootfs`](../../tools/rootfs):

- `provision.py` authenticates a release statement and verifies the pinned
  image digest before installation;
- `verify.py` checks the immutable image metadata and GPT layout;
- `with-verified-rootfs.sh` wraps every ordinary guest campaign; and
- `maintenance.py` is the separately authorized exceptional write path.

No build or test target consumes the scripts in this directory. Keep them
data-only unless a future image-construction workflow deliberately adopts and
tests them under `tools/rootfs`.
