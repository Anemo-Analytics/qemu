#!/usr/bin/env python3
"""
Anonymous FTP server for MPC5200 boot-mode emulation.

Serves the Vestas runtime image (`ct6003/vxworks`) to a VxWorks BSP
booting in network-recovery mode. Pairs with the QEMU `mpc5200-fec`
device + SLIRP `-netdev user,host=169.254.254.252` configuration.

Boot-mode parameters confirmed by Daniele's BSP diagnosis (see
`BSP_park_findings.md`):

  Server IP : 169.254.254.252  (target boots at .254)
  User      : anonymous
  Password  : test@cotas.dk    (any password actually works for anonymous)
  File      : ct6003/vxworks   (relative to FTP root)

Usage (default):

  python3 scripts/mpc5200_boot_ftp.py

  Bind:  127.0.0.1:21  (loopback — SLIRP exposes host's loopback to
                        the guest as 169.254.254.252 when QEMU is
                        launched with -netdev user,host=169.254.254.252)
  Root:  /tmp/mpc5200_ftp/   (must contain ct6003/vxworks)

For root-bound port 21 you may need:

  sudo setcap 'cap_net_bind_service=+ep' $(which python3)

…or pass `--port 2121` and use `hostfwd=tcp::21-:2121` on the QEMU
command line. Privileged-port path is the simpler default.

Tested against pyftpdlib 2.x (Python ≥3.10).
"""

import argparse
import logging
import os
import sys

from pyftpdlib.authorizers import DummyAuthorizer
from pyftpdlib.handlers import FTPHandler
from pyftpdlib.servers import FTPServer


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--root", default="/tmp/mpc5200_ftp",
                   help="FTP serve root (must contain ct6003/vxworks). "
                        "Default: /tmp/mpc5200_ftp")
    p.add_argument("--bind", default="127.0.0.1",
                   help="Address to bind. Default: 127.0.0.1 (paired with "
                        "SLIRP). Use 0.0.0.0 to expose externally.")
    p.add_argument("--port", type=int, default=21,
                   help="Port to bind. Default: 21. Use 2121 if running "
                        "as a non-root user without CAP_NET_BIND_SERVICE.")
    p.add_argument("--verbose", action="store_true",
                   help="Verbose logging.")
    args = p.parse_args()

    if not os.path.isdir(args.root):
        print(f"error: root {args.root} does not exist or isn't a directory",
              file=sys.stderr)
        return 1

    expected = os.path.join(args.root, "ct6003", "vxworks")
    if not os.path.isfile(expected):
        print(f"warn: {expected} not found — boot will fail. Stage the "
              f"runtime image there.", file=sys.stderr)

    logging.basicConfig(level=logging.DEBUG if args.verbose else logging.INFO,
                        format="%(asctime)s %(levelname)s %(message)s")

    authorizer = DummyAuthorizer()
    # Anonymous read-only access to the entire root.
    authorizer.add_anonymous(args.root, perm="elr")

    handler = FTPHandler
    handler.authorizer = authorizer
    handler.banner = "mpc5200_boot_ftp ready"

    server = FTPServer((args.bind, args.port), handler)
    server.max_cons = 16
    server.max_cons_per_ip = 8

    print(f"serving FTP on {args.bind}:{args.port}, root={args.root}")
    print(f"expected file: ct6003/vxworks ({os.path.getsize(expected) if os.path.isfile(expected) else 'MISSING'} bytes)")
    print("paired qemu netdev: -netdev user,id=n0,net=169.254.254.0/24,"
          "host=169.254.254.252,hostfwd=tcp::21-:21")

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        return 0
    return 0


if __name__ == "__main__":
    sys.exit(main())
