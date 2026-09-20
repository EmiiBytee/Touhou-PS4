#!/usr/bin/env python3
"""Upload a local directory tree to an anonymous FTP server without deleting remote files."""

import argparse
import ftplib
import posixpath
from pathlib import Path


def ensure_directory(ftp: ftplib.FTP, directory: str) -> None:
    current = ""
    for part in directory.strip("/").split("/"):
        if not part:
            continue
        current += "/" + part
        try:
            ftp.mkd(current)
        except ftplib.error_perm as error:
            if not str(error).startswith("550"):
                raise


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("host")
    parser.add_argument("port", type=int)
    parser.add_argument("local_directory", type=Path)
    parser.add_argument("remote_directory")
    args = parser.parse_args()

    local_root = args.local_directory.resolve()
    files = sorted(path for path in local_root.rglob("*") if path.is_file())

    with ftplib.FTP() as ftp:
        ftp.connect(args.host, args.port, timeout=15)
        ftp.login()
        ensure_directory(ftp, args.remote_directory)

        uploaded_bytes = 0
        for path in files:
            relative = path.relative_to(local_root).as_posix()
            remote_path = posixpath.join(args.remote_directory.rstrip("/"), relative)
            ensure_directory(ftp, posixpath.dirname(remote_path))
            with path.open("rb") as source:
                ftp.storbinary(f"STOR {remote_path}", source, blocksize=256 * 1024)
            uploaded_bytes += path.stat().st_size

        ftp.quit()

    print(f"Uploaded {len(files)} files ({uploaded_bytes} bytes) to {args.remote_directory}")


if __name__ == "__main__":
    main()
