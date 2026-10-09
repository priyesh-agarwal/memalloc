#!/bin/sh
# sudo /bin/sh experiment/compare.sh
exec python3 "$(CDPATH= cd -- "$(dirname "$0")" && pwd)/compare.py"
