#!/usr/bin/env bash
# Find and terminate any leaked AWS resources from citor bench runs.
#
# Safe to run any time. Lists what exists, prompts before terminating.
# Use after a script crash, laptop reboot, or whenever paranoid.
#
# Usage:
#   scripts/bench/cleanup_aws.sh            # list + prompt
#   scripts/bench/cleanup_aws.sh --force    # terminate without prompt

set -euo pipefail

PROFILE="${CITOR_AWS_PROFILE:-citor}"
REGION="${CITOR_AWS_REGION:-us-east-1}"
FORCE=0
[ "${1:-}" = "--force" ] && FORCE=1

aws_cli() { aws --profile "$PROFILE" --region "$REGION" "$@"; }

echo "scanning region: $REGION"

# Running citor-bench instances.
inst=$(aws_cli ec2 describe-instances \
  --filters "Name=tag-key,Values=citor-bench" "Name=instance-state-name,Values=pending,running,stopping,stopped" \
  --query 'Reservations[].Instances[].[InstanceId,InstanceType,State.Name,LaunchTime,Tags[?Key==`citor-bench`]|[0].Value]' \
  --output text || true)
echo
echo "=== running / stopped citor-bench instances ==="
if [ -z "$inst" ]; then
  echo "(none)"
else
  echo "$inst"
fi

# Spot requests still open.
sirs=$(aws_cli ec2 describe-spot-instance-requests \
  --filters "Name=state,Values=open,active,failed" \
  --query 'SpotInstanceRequests[].[SpotInstanceRequestId,State,InstanceId]' \
  --output text || true)
echo
echo "=== open / active spot requests ==="
if [ -z "$sirs" ]; then
  echo "(none)"
else
  echo "$sirs"
fi

# Citor-bench security groups (named citor-bench-*).
sgs=$(aws_cli ec2 describe-security-groups \
  --filters "Name=group-name,Values=citor-bench-*" \
  --query 'SecurityGroups[].[GroupId,GroupName]' \
  --output text || true)
echo
echo "=== citor-bench security groups ==="
if [ -z "$sgs" ]; then
  echo "(none)"
else
  echo "$sgs"
fi

if [ -z "$inst" ] && [ -z "$sirs" ] && [ -z "$sgs" ]; then
  echo
  echo "nothing to clean up. done."
  exit 0
fi

echo
if [ "$FORCE" -ne 1 ]; then
  read -r -p "terminate everything listed above? [y/N] " ans
  [ "$ans" = "y" ] || [ "$ans" = "Y" ] || { echo "aborted."; exit 1; }
fi

# Terminate instances.
if [ -n "$inst" ]; then
  ids=$(echo "$inst" | awk '{print $1}' | tr '\n' ' ')
  echo "terminating: $ids"
  aws_cli ec2 terminate-instances --instance-ids $ids >/dev/null
fi

# Cancel spot requests.
if [ -n "$sirs" ]; then
  ids=$(echo "$sirs" | awk '{print $1}' | tr '\n' ' ')
  echo "cancelling spot: $ids"
  aws_cli ec2 cancel-spot-instance-requests --spot-instance-request-ids $ids >/dev/null
fi

# Delete SGs (only succeeds after instances are gone; AWS auto-detaches).
if [ -n "$sgs" ]; then
  echo "waiting for instances to fully terminate before deleting SGs ..."
  if [ -n "$inst" ]; then
    aws_cli ec2 wait instance-terminated --instance-ids $(echo "$inst" | awk '{print $1}' | tr '\n' ' ') || true
  fi
  while IFS= read -r line; do
    sg_id=$(echo "$line" | awk '{print $1}')
    sg_name=$(echo "$line" | awk '{print $2}')
    echo "deleting sg $sg_id ($sg_name)"
    aws_cli ec2 delete-security-group --group-id "$sg_id" >/dev/null || echo "  (failed, will retry next time)"
  done <<< "$sgs"
fi

echo
echo "done."
