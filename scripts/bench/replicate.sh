#!/usr/bin/env bash
# Run the citor parallel_bench on AWS bare metal spot instances.
#
# Usage:
#   scripts/bench/replicate.sh <amd|intel|both> [--filter SUBSTR]
#
# Pre-reqs:
#   - aws CLI v2 + jq + ssh + scp in PATH
#   - AWS_PROFILE=citor (or set CITOR_AWS_PROFILE=... below) wired to the
#     credentials in /run/agenix/aws-{access-key-id,secret-access-key}
#   - EC2 key pair `aws-default-key-pair` registered in eu-north-1 whose
#     private half is /run/agenix/aws-default-key-pair
#   - Spot quota high enough for c7a.metal-48xl (192 vCPU) and
#     c7i.metal-24xl (96 vCPU); raise via the EC2 Service Quotas console
#     under "All Standard ... Spot Instance Requests" if RunInstances fails
#     with MaxSpotInstanceCountExceeded
#
# Output:
#   bench_out/replication/<arch>-<sha>/results.json   (the bench export)
#   bench_out/replication/<arch>-<sha>/host.json      (provenance)
#   bench_out/replication/<arch>-<sha>/run.log        (full run log)
#
# Cost (eu-north-1 spot, May 2026):
#   amd  ~ $1.05-1.40 per run (c7a.metal-48xl @ ~$1.20/hr * ~1 hr)
#   intel ~ $0.46-0.55 per run (c7i.metal-24xl @ ~$0.46/hr * ~1 hr)
#
# The script self-provisions a single-use security group and instance
# tag, and ALWAYS cleans them up via a trap on EXIT.

set -euo pipefail

# ----- config -----
PROFILE="${CITOR_AWS_PROFILE:-citor}"
REGION="${CITOR_AWS_REGION:-us-east-1}"
KEY_NAME="${CITOR_AWS_KEY_NAME:-aws-default-key-pair}"
SSH_KEY_FILE="${CITOR_SSH_KEY_FILE:-/run/agenix/aws-default-key-pair}"

# The source tree is shipped to the remote as a `git archive` tarball
# (the repo is private; no `git clone` over HTTPS without a PAT). The
# tarball is whatever `git archive HEAD` produces from the local checkout,
# so uncommitted working-tree edits are NOT included; pushed history only.

# Per-arch settings: AMI is Ubuntu 24.04 LTS HVM x86_64 (Canonical owner).
# The exact AMI ID drifts; we resolve it at runtime via describe-images.
declare -A SKU=(
  [amd]=c7a.metal-48xl
  [intel]=c7i.metal-24xl
)
# Bench runs unpinned across the whole instance. c7i.metal-24xl exposes
# 48 physical cores (single Sapphire Rapids socket); c7a.metal-48xl
# exposes 96 across 2 Genoa NUMA nodes. The high-j cells (j=32/48/96)
# need that full width to clear the bench's `hasEnoughPhysicalCores`
# guard. A `taskset -c 0-15` mask here matches the local 9950X3D
# CCD-scale protocol but defeats the purpose of paying for bare metal.

REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SHA="$(git -C "$REPO_ROOT" rev-parse --short=12 HEAD)"
TS="$(date +%Y%m%dT%H%M%S)"

# ----- helpers -----
log() { printf '[%s] %s\n' "$(date +%H:%M:%S)" "$*" >&2; }

aws_cli() { aws --profile "$PROFILE" --region "$REGION" "$@"; }

# Shared with run_arch via globals (must NOT be local in run_arch so the
# EXIT trap can still see them after the function returns under set -e).
sg_id=""
instance_id=""
sir_id=""
cleaned=0

cleanup() {
  [ "$cleaned" = "1" ] && return
  cleaned=1
  if [ -n "$instance_id" ]; then
    log "cleanup: terminating instance $instance_id"
    aws_cli ec2 terminate-instances --instance-ids "$instance_id" >/dev/null 2>&1 || true
    aws_cli ec2 wait instance-terminated --instance-ids "$instance_id" >/dev/null 2>&1 || true
  fi
  if [ -n "$sir_id" ]; then
    log "cleanup: cancelling spot request $sir_id"
    aws_cli ec2 cancel-spot-instance-requests --spot-instance-request-ids "$sir_id" >/dev/null 2>&1 || true
  fi
  if [ -n "$sg_id" ]; then
    log "cleanup: deleting sg $sg_id"
    aws_cli ec2 delete-security-group --group-id "$sg_id" >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT INT TERM

resolve_ubuntu_ami() {
  # Canonical owner id is 099720109477 for Ubuntu in every commercial region.
  aws_cli ec2 describe-images \
    --owners 099720109477 \
    --filters \
      "Name=name,Values=ubuntu/images/hvm-ssd-gp3/ubuntu-noble-24.04-amd64-server-*" \
      "Name=state,Values=available" \
    --query 'sort_by(Images, &CreationDate)[-1].ImageId' \
    --output text
}

ensure_security_group() {
  # One-shot SG named citor-bench-<ts>-<arch>. Allows SSH from the runner's
  # public IPv4 only. We force IPv4 with `curl -4` because EC2's IPv4
  # security-group ingress rejects v6 CIDR strings; routing the SSH itself
  # falls back to whichever stack the OS prefers.
  local sg_name="$1"
  local my_ip
  my_ip="$(curl -4 -fsSL https://ifconfig.me)/32"
  log "self-public-ipv4 = $my_ip"
  local sg_id
  sg_id=$(aws_cli ec2 create-security-group \
    --group-name "$sg_name" \
    --description "citor bench replication run (single use)" \
    --query 'GroupId' --output text)
  log "sg=$sg_id (created)"
  aws_cli ec2 authorize-security-group-ingress \
    --group-id "$sg_id" \
    --protocol tcp --port 22 --cidr "$my_ip" >/dev/null
  echo "$sg_id"
}

wait_for_ssh() {
  local host="$1"
  log "waiting for ssh on $host ..."
  for _ in $(seq 1 60); do
    if ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
        -o ConnectTimeout=5 -i "$SSH_KEY_FILE" ubuntu@"$host" 'echo ok' >/dev/null 2>&1; then
      log "ssh up"
      return 0
    fi
    sleep 5
  done
  log "ssh never came up"
  return 1
}

# ----- bench runner (runs on the remote instance) -----
remote_bench_script() {
  cat <<'REMOTE'
set -euo pipefail
sudo DEBIAN_FRONTEND=noninteractive apt-get update -qq
sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \
  ca-certificates wget gnupg >/dev/null
# Ubuntu 24.04's default repos cap at clang-18, which has an OpenMP
# frontend bug that rejects structured-binding capture in TUs that mix
# `#pragma omp` with citor's templated dispatch (the bench has both).
# Pull clang-19 from apt.llvm.org; CI uses the same build path.
sudo install -d -m 0755 /etc/apt/keyrings
wget -qO- https://apt.llvm.org/llvm-snapshot.gpg.key \
  | sudo gpg --dearmor -o /etc/apt/keyrings/llvm.gpg
echo "deb [signed-by=/etc/apt/keyrings/llvm.gpg] http://apt.llvm.org/noble/ llvm-toolchain-noble-19 main" \
  | sudo tee /etc/apt/sources.list.d/llvm.list >/dev/null
sudo DEBIAN_FRONTEND=noninteractive apt-get update -qq
sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \
  build-essential cmake ninja-build git curl numactl util-linux \
  clang-19 lld-19 libomp-19-dev libtbb-dev pkg-config \
  python3 linux-tools-common linux-tools-generic >/dev/null
# CPU governor to performance for honest numbers (no fight with cpufreq).
if [ -d /sys/devices/system/cpu/cpu0/cpufreq ]; then
  echo performance | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor >/dev/null || true
fi
mkdir -p /tmp/citor
tar -xzf /tmp/citor.tar.gz -C /tmp/citor
cd /tmp/citor
export CC=clang-19 CXX=clang++-19
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCITOR_BUILD_BENCHMARK=ON \
  -DCITOR_BUILD_TESTS=OFF \
  -DCITOR_ENABLE_CLANG_TIDY=OFF >&2
cmake --build build --target parallel_bench -j >&2
mkdir -p /tmp/out
# Unpinned: lets the bench's per-cell j sweep exercise the whole box.
if [ "$ISOLATED" = "1" ]; then
  ./build/benchmark/parallel_bench --list > /tmp/registry.txt
  if [ -f /tmp/resume.log ]; then
    # All started cells except the last (potentially crashed mid-run) are done.
    grep -oE '\[[0-9]+/[0-9]+\] \(.*\) starting: [A-Za-z0-9_]+' /tmp/resume.log \
      | awk '{print $NF}' | head -n -1 > /tmp/done.txt
    grep -vxF -f /tmp/done.txt /tmp/registry.txt > /tmp/pending.txt
  else
    cp /tmp/registry.txt /tmp/pending.txt
  fi
  if [ -n "${FILTER:-}" ]; then
    grep -F "$FILTER" /tmp/pending.txt > /tmp/pending.filtered && mv /tmp/pending.filtered /tmp/pending.txt
  fi
  total=$(wc -l < /tmp/pending.txt)
  idx=0
  rm -f /tmp/out/cell-*.json
  while read -r cell; do
    idx=$((idx + 1))
    echo "[$idx/$total] isolated: $cell"
    timeout 300 ./build/benchmark/parallel_bench \
      --filter "$cell" --export "/tmp/out/cell-$idx.json" || \
      echo "[$idx/$total] $cell exited non-zero (likely peer segfault); continuing"
  done < /tmp/pending.txt
  # Merge per-cell JSONs into one. Keep first schema_version + context;
  # concat samples.
  shopt -s nullglob
  cells=(/tmp/out/cell-*.json)
  if [ "${#cells[@]}" -gt 0 ]; then
    jq -s '{
      schema_version: .[0].schema_version,
      context: .[0].context,
      samples: (map(.samples) | add)
    }' "${cells[@]}" > /tmp/out/results.json
  else
    echo '{"schema_version":0,"context":{},"samples":[]}' > /tmp/out/results.json
  fi
else
  ./build/benchmark/parallel_bench \
    ${FILTER:+--filter "$FILTER"} \
    --export /tmp/out/results.json
fi
# Capture host provenance for the JSON sidecar.
{
  echo "{"
  echo "  \"hostname\": \"$(hostname)\","
  echo "  \"kernel\": \"$(uname -r)\","
  echo "  \"cpu_model\": \"$(grep -m1 'model name' /proc/cpuinfo | sed 's/^[^:]*: //')\","
  echo "  \"physical_cores\": $(grep -c '^processor' /proc/cpuinfo),"
  echo "  \"numa_nodes\": $(numactl --hardware 2>/dev/null | grep -c '^node ' || echo 0),"
  echo "  \"sku\": \"$INSTANCE_TYPE\","
  echo "  \"region\": \"$REGION\","
  echo "  \"governor\": \"$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo unknown)\""
  echo "}"
} > /tmp/out/host.json
REMOTE
}

# ----- top-level per-arch driver -----
run_arch() {
  local arch="$1"
  local sku="${SKU[$arch]}"
  local out_dir="$REPO_ROOT/bench_out/replication/${arch}-${SHA}-${TS}"
  mkdir -p "$out_dir"
  local log_file="$out_dir/run.log"
  exec 4>"$log_file"

  log "=== arch=$arch sku=$sku (unpinned) ==="
  log "output dir: $out_dir"

  # Do not localize `sg_id`/`sir_id`/`instance_id`: the EXIT trap's
  # `cleanup` reads them from global scope.
  local ami host
  ami="$(resolve_ubuntu_ami)"
  log "ami=$ami"

  local sg_name="citor-bench-${TS}-${arch}"
  # Reset the shared globals before each arch (`both` invokes run_arch twice
  # in sequence).
  sg_id=""
  instance_id=""
  sir_id=""
  cleaned=0

  sg_id="$(ensure_security_group "$sg_name")"

  if [ "$ON_DEMAND" = "1" ]; then
    log "requesting on-demand $sku (override, --on-demand) ..."
    # Safety belts on top of the EXIT trap:
    #  * `instance-initiated-shutdown-behavior=terminate` means a `shutdown
    #    -h now` from inside the box also terminates instead of stopping
    #    (stopping leaves the EBS root volume paying).
    #  * Tag the instance with `citor-bench` so any leaked instance is
    #    findable via `scripts/bench/cleanup_aws.sh` later.
    instance_id=$(aws_cli ec2 run-instances \
      --image-id "$ami" \
      --instance-type "$sku" \
      --key-name "$KEY_NAME" \
      --security-group-ids "$sg_id" \
      --instance-initiated-shutdown-behavior terminate \
      --tag-specifications "ResourceType=instance,Tags=[{Key=citor-bench,Value=$SHA},{Key=auto-terminate,Value=true}]" \
      --count 1 \
      --query 'Instances[0].InstanceId' --output text)
    log "instance: $instance_id"
  else
    log "requesting spot $sku ..."
    sir_id=$(aws_cli ec2 request-spot-instances \
      --instance-count 1 \
      --type one-time \
      --launch-specification "$(jq -nc \
        --arg ami "$ami" \
        --arg sku "$sku" \
        --arg key "$KEY_NAME" \
        --arg sg "$sg_id" \
        '{ImageId:$ami, InstanceType:$sku, KeyName:$key, SecurityGroupIds:[$sg]}')" \
      --query 'SpotInstanceRequests[0].SpotInstanceRequestId' --output text)
    log "spot request id: $sir_id"

    log "waiting for spot fulfilment ..."
    aws_cli ec2 wait spot-instance-request-fulfilled --spot-instance-request-ids "$sir_id"
    instance_id=$(aws_cli ec2 describe-spot-instance-requests \
      --spot-instance-request-ids "$sir_id" \
      --query 'SpotInstanceRequests[0].InstanceId' --output text)
    log "instance: $instance_id"
  fi

  aws_cli ec2 create-tags --resources "$instance_id" \
    --tags Key=Name,Value="citor-bench-$arch-$TS" Key=citor-bench,Value="$SHA" >/dev/null

  log "waiting for instance running ..."
  aws_cli ec2 wait instance-running --instance-ids "$instance_id"
  host=$(aws_cli ec2 describe-instances --instance-ids "$instance_id" \
    --query 'Reservations[0].Instances[0].PublicIpAddress' --output text)
  log "host: $host"

  wait_for_ssh "$host"

  log "packing local repo as git archive ..."
  local repo_tarball="$out_dir/citor-src.tar.gz"
  git -C "$REPO_ROOT" archive --format=tar.gz -o "$repo_tarball" HEAD

  log "uploading repo tarball + bench runner ..."
  scp -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
    -i "$SSH_KEY_FILE" \
    "$repo_tarball" ubuntu@"$host":/tmp/citor.tar.gz

  if [ -n "$RESUME_LOG" ]; then
    log "shipping resume log $RESUME_LOG ..."
    scp -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
      -i "$SSH_KEY_FILE" \
      "$RESUME_LOG" ubuntu@"$host":/tmp/resume.log
  fi

  local remote_script="/tmp/citor-bench-runner.sh"
  remote_bench_script | ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
    -i "$SSH_KEY_FILE" ubuntu@"$host" "cat > $remote_script && chmod +x $remote_script"

  log "running bench remotely (this takes ~1 h on the slow tail; live progress lands in run.log)..."
  ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
    -i "$SSH_KEY_FILE" ubuntu@"$host" \
    "SHA='$SHA' INSTANCE_TYPE='$sku' REGION='$REGION' FILTER='${FILTER:-}' ISOLATED='$ISOLATED' bash $remote_script" \
    2>&1 | tee -a "$log_file"

  log "pulling results ..."
  scp -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
    -i "$SSH_KEY_FILE" \
    ubuntu@"$host":/tmp/out/results.json "$out_dir/results.json"
  scp -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
    -i "$SSH_KEY_FILE" \
    ubuntu@"$host":/tmp/out/host.json "$out_dir/host.json"

  log "done. results -> $out_dir/"
  cleanup
  trap - EXIT INT TERM
}

# ----- argv -----
if [ "$#" -lt 1 ]; then
  echo "usage: $0 <amd|intel|both> [--filter SUBSTR]" >&2
  exit 2
fi

WHAT="$1"; shift
FILTER=""
ON_DEMAND=0
ISOLATED=0
RESUME_LOG=""
while [ "$#" -gt 0 ]; do
  case "$1" in
    --filter) FILTER="$2"; shift 2 ;;
    --on-demand) ON_DEMAND=1; shift ;;
    --isolated) ISOLATED=1; shift ;;
    --resume-from) RESUME_LOG="$2"; ISOLATED=1; shift 2 ;;
    *) echo "unknown arg: $1" >&2; exit 2 ;;
  esac
done
if [ -n "$RESUME_LOG" ] && [ ! -f "$RESUME_LOG" ]; then
  echo "--resume-from: file not found: $RESUME_LOG" >&2
  exit 2
fi

case "$WHAT" in
  amd|intel) run_arch "$WHAT" ;;
  both) run_arch amd; run_arch intel ;;
  *) echo "first arg must be amd|intel|both, got: $WHAT" >&2; exit 2 ;;
esac
