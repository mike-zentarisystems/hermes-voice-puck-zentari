#!/usr/bin/env python3
"""Explicit operator-only password provisioning; never invoked by server startup."""
import argparse
import getpass
import importlib.util
import json
import os
from pathlib import Path
import secrets
import stat
import sys
import warnings

spec = importlib.util.spec_from_file_location("pwa_passwords", Path(__file__).resolve().parents[1] / "pwa/passwords.py")
passwords = importlib.util.module_from_spec(spec)
spec.loader.exec_module(passwords)


def private_directory(path):
    """Anchor traversal at /; never follow symlinks or untrusted ancestors."""
    if not path.is_absolute() or ".." in path.parts:
        raise ValueError("reset requires an absolute path without parent traversal")
    descriptor = os.open("/", os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    try:
        for index, name in enumerate(path.parts[1:]):
            child = os.open(name, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW, dir_fd=descriptor)
            os.close(descriptor)
            descriptor = child
            info = os.fstat(descriptor)
            # Root-owned sticky ancestors such as /tmp protect private children.
            sticky_ancestor = (index < len(path.parts) - 2 and info.st_uid == 0 and
                               bool(info.st_mode & stat.S_ISVTX))
            if (info.st_uid not in (0, os.geteuid()) or
                    (info.st_mode & 0o022 and not sticky_ancestor)):
                raise ValueError("reset requires operator/root-owned directories without group/world write access")
        return descriptor
    except BaseException:
        os.close(descriptor)
        raise


def reset_record(path, record):
    """Atomically replace one existing private hash, retaining its owner/mode.

    Linux operator action only. The bridge must be recreated afterward: its
    configuration is loaded once, and Docker file mounts retain the old inode.
    """
    if os.name != "posix":
        raise ValueError("reset requires a Linux private terminal on the PWA host")
    import fcntl

    path = Path(path)
    if not path.is_absolute() or ".." in path.parts:
        raise ValueError("reset requires an absolute path without parent traversal")
    directory = private_directory(path.parent)
    temporary = None
    replaced = False
    try:
        # Lock the directory, leaving no additional credential or lock files.
        try:
            fcntl.flock(directory, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise ValueError("another password reset is in progress") from None
        descriptor = os.open(path.name, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=directory)
        with os.fdopen(descriptor, "rb") as stream:
            previous = os.fstat(stream.fileno())
            if (not stat.S_ISREG(previous.st_mode) or previous.st_nlink != 1 or
                    stat.S_IMODE(previous.st_mode) not in (0o400, 0o600) or
                    (os.geteuid() != 0 and previous.st_uid != os.geteuid())):
                raise ValueError("reset requires a single private regular file owned by the operator or managed by root")
            raw = stream.read(4097)
        try:
            value = json.loads(raw)
            valid = (len(raw) <= 4096 and isinstance(value, dict) and
                     type(value.get("v")) is int and value["v"] == 1 and
                     value.get("algorithm") == "scrypt-131072-8-1" and
                     len(bytes.fromhex(value["salt"])) == 32 and
                     len(bytes.fromhex(value["hash"])) == 32)
        except (ValueError, KeyError, TypeError):
            valid = False
        if not valid:
            raise ValueError("existing file is not a valid PWA password record; nothing replaced")

        candidate = f".{path.name}.{secrets.token_hex(16)}.tmp"
        descriptor = os.open(candidate, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW,
                             0o600, dir_fd=directory)
        temporary = candidate
        with os.fdopen(descriptor, "w") as stream:
            json.dump(record, stream)
            stream.write("\n")
            current = os.fstat(stream.fileno())
            os.fchmod(stream.fileno(), stat.S_IMODE(previous.st_mode))
            if (current.st_uid, current.st_gid) != (previous.st_uid, previous.st_gid):
                os.fchown(stream.fileno(), previous.st_uid, previous.st_gid)
            stream.flush()
            os.fsync(stream.fileno())
        current = os.stat(path.name, dir_fd=directory, follow_symlinks=False)
        signature = lambda info: (info.st_dev, info.st_ino, info.st_size, info.st_mtime_ns,
                                  info.st_ctime_ns, info.st_uid, info.st_gid, info.st_mode, info.st_nlink)
        if signature(current) != signature(previous):
            raise ValueError("password record changed during reset; nothing replaced")
        os.replace(temporary, path.name, src_dir_fd=directory, dst_dir_fd=directory)
        temporary = None
        replaced = True
        os.fsync(directory)
    except OSError:
        if replaced:
            raise RuntimeError("record replaced but durability check failed; review privately before activation") from None
        raise
    finally:
        try:
            if temporary is not None:
                os.unlink(temporary, dir_fd=directory)
        finally:
            os.close(directory)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Privately provision or stage a PWA password reset.")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--reset", action="store_true",
                        help="replace an existing private Linux password record; recreate only the PWA afterward")
    args = parser.parse_args()
    # Requires a real operator terminal; getpass fallback must not echo.
    if not sys.stdin.isatty():
        parser.error("interactive private terminal required")
    if args.reset and os.name != "posix":
        parser.error("reset requires a Linux private terminal on the PWA host")
    try:
        with warnings.catch_warnings():
            warnings.simplefilter("error", getpass.GetPassWarning)
            first = getpass.getpass("New PWA password: ")
            confirmation = getpass.getpass("Confirm PWA password: ")
    except (getpass.GetPassWarning, EOFError):
        parser.error("hidden input unavailable; use an interactive private terminal")
    if first != confirmation:
        parser.error("passwords differ")
    try:
        record = passwords.password_record(first)
    except ValueError as error:
        # The message states only the length rule, never the password.
        parser.error(str(error))
    try:
        if args.reset:
            reset_record(args.output, record)
            print("Password record replaced; NOT ACTIVE YET. Recreate only the PWA container with its existing deployment configuration.")
            print("A plain container restart is insufficient for an atomically replaced file mount. Recreating the PWA ends all existing PWA sessions.")
            sys.exit(0)
        descriptor = os.open(args.output, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(descriptor, "w") as stream:
            json.dump(record, stream)
            stream.write("\n")
    except FileExistsError:
        parser.error("private password file already exists; existing files are never overwritten")
    except (ValueError, RuntimeError) as error:
        parser.error(str(error))
    except OSError as error:
        parser.error(f"could not {'replace' if args.reset else 'create'} private password file: {error.strerror}")
