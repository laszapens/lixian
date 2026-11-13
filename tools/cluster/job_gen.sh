#!/bin/bash
#SBATCH --job-name=lixian-datagen
#SBATCH --partition=xahctest
#SBATCH --nodes=1
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=64
#SBATCH --time=24:00:00
#SBATCH --output=gen-%j.log
#SBATCH --error=gen-%j.err

set -euo pipefail
WORK=${LX_WORK:-$HOME/lixian-cloud}
cd "$WORK"
echo "host=$(hostname) cores=$(nproc) start=$(date -Is)"
mkdir -p data
exec ./datagen --workers 32 --games 1000000 --play-nodes 3000 \
     --play-jitter 6000 --label-nodes 12000 --out data --openings openings.txt
