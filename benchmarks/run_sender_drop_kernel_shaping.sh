#!/usr/bin/env bash
set -u

if [[ $# -ne 4 ]]; then
  echo "usage: $0 VM_WORK_ROOT DIAGNOSTIC_INSTALL OUTPUT_ROOT NETEM_RUNNER" >&2
  exit 2
fi

work_root=$1
diagnostic_install=$2
output_root=$3
runner=$4
uid_value=$(id -u)
gid_value=$(id -g)
old_rmem=$(sysctl -n net.core.rmem_max)
old_wmem=$(sysctl -n net.core.wmem_max)
mkdir -p "$output_root/evidence"
printf 'net.core.rmem_max=%s\nnet.core.wmem_max=%s\n' \
  "$old_rmem" "$old_wmem" > "$output_root/evidence/udp-before.txt"

cleanup() {
  sudo sysctl -q -w net.core.rmem_max="$old_rmem" \
    net.core.wmem_max="$old_wmem"
  printf 'net.core.rmem_max=%s\nnet.core.wmem_max=%s\n' \
    "$(sysctl -n net.core.rmem_max)" "$(sysctl -n net.core.wmem_max)" \
    > "$output_root/evidence/udp-after.txt"
  ip netns list > "$output_root/evidence/namespaces-after.txt"
}
trap cleanup EXIT
trap 'trap - EXIT; cleanup; exit 130' INT TERM

sudo sysctl -q -w net.core.rmem_max=33554432 \
  net.core.wmem_max=33554432
printf 'net.core.rmem_max=%s\nnet.core.wmem_max=%s\n' \
  "$(sysctl -n net.core.rmem_max)" "$(sysctl -n net.core.wmem_max)" \
  > "$output_root/evidence/udp-active.txt"

sha256sum "$diagnostic_install/lib/libsrt.so.0.2.4" \
  > "$output_root/evidence/diagnostic-library.sha256"
LD_LIBRARY_PATH="$diagnostic_install/lib" \
  ldd "$work_root/telemetry-robotweax-build/robotweax_srt_telemetry_public_srt_incident_peer" \
  > "$output_root/evidence/peer-ldd.txt"

common=(
  --telemetry-source "$work_root/telemetry"
  --uid "$uid_value" --gid "$gid_value"
  --robotweax-peer "$work_root/telemetry-robotweax-build/robotweax_srt_telemetry_public_srt_incident_peer"
  --robotweax-library "$diagnostic_install/lib/libsrt.so.0.2.4"
  --robotweax-version v0.2.4-26-gce3d36f
  --robotweax-revision ce3d36f77c567b69c0429f7f8aea6d9717d91045
  --robotweax-build-profile linux-arm64-release-shared-openssl-sender-drop-trace
  --haivision-peer "$work_root/telemetry-haivision-build/robotweax_srt_telemetry_public_srt_incident_peer"
  --haivision-library "$work_root/haivision-install/lib/libsrt.so.1.5.7"
  --haivision-version v1.5.7
  --haivision-revision 899348d8318eb9a3c5a5b6ec43c4a1114288773a
  --haivision-build-profile linux-arm64-release-shared-openssl
  --telemetry-revision 9fe30a833c0c5f755a5f57249103f1a955790b80
)

run_case() {
  local name=$1 active_pps=$2
  local trace="/tmp/robotweax-sender-drop-${name}.jsonl"
  mkdir -p "$output_root/$name"
  local -a argv=(
    sudo -E env
    "LD_LIBRARY_PATH=$diagnostic_install/lib"
    "ROBOTWEAX_SRT_SENDER_DROP_TRACE=$trace"
    python3 "$runner" "${common[@]}"
    --scenario bandwidth-reduction
    --direction robotweax-to-haivision
    --repetition 1
    --output "$output_root/$name/run"
    --active-pps "$active_pps"
  )
  printf '%q ' "${argv[@]}" >> "$output_root/evidence/commands.sh"
  printf '\n' >> "$output_root/evidence/commands.sh"
  set +e
  "${argv[@]}" > "$output_root/$name/runner.stdout" \
    2> "$output_root/$name/runner.stderr"
  local code=$?
  set -e
  printf '%s\n' "$code" > "$output_root/$name/runner.exit"
  if [[ -f "$trace" ]]; then
    mv "$trace" "$output_root/$name/sender-drop-trace.jsonl"
  fi
  printf '%s active_pps=%s exit=%s\n' "$name" "$active_pps" "$code"
}

set -e
printf '#!/usr/bin/env bash\n' > "$output_root/evidence/commands.sh"
printf '%s\n' "$(date -u +%FT%TZ)" \
  > "$output_root/evidence/started-at.txt"
run_case rep1 1400
run_case rep2 1400
run_case rep3 1400
run_case rate2800 2800
printf '%s\n' "$(date -u +%FT%TZ)" \
  > "$output_root/evidence/finished-at.txt"
