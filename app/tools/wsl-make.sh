#!/bin/sh
# Run make inside WSL from Windows: wsl -d Ubuntu-26.04 -- sh tools/wsl-make.sh <targets>
cd "$(dirname "$0")/.." && exec make "$@"
