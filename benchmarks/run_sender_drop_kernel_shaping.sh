#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 5 ]]; then
  echo "usage: $0 VM_WORK_ROOT DIAGNOSTIC_INSTALL OUTPUT_ROOT NETEM_RUNNER BUILD_MANIFEST" >&2
  exit 2
fi

work_root=$1
diagnostic_install=$2
output_root=$3
runner=$4
manifest=$5
script_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
# Never overwrite or append to a prior evidence set.
mkdir "$output_root"
output_root=$(cd -- "$output_root" && pwd)
mkdir "$output_root/evidence"
python3 "$script_root/sender_drop_provenance.py" "$manifest" \
  --robotweax-library "$diagnostic_install/lib/libsrt.so.0.2.4" \
  --robotweax-peer "$work_root/telemetry-robotweax-build/robotweax_srt_telemetry_public_srt_incident_peer" \
  --haivision-library "$work_root/haivision-install/lib/libsrt.so.1.5.7" \
  --haivision-peer "$work_root/telemetry-haivision-build/robotweax_srt_telemetry_public_srt_incident_peer" \
  --telemetry-source "$work_root/telemetry" \
  --output "$output_root/evidence/build-provenance.json" \
  --args-output "$output_root/evidence/provenance-args.bin"
mapfile -d '' -t provenance_args < "$output_root/evidence/provenance-args.bin"
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
  "${provenance_args[@]}"
  --telemetry-source "$work_root/telemetry"
  --uid "$uid_value" --gid "$gid_value"
  --robotweax-peer "$work_root/telemetry-robotweax-build/robotweax_srt_telemetry_public_srt_incident_peer"
  --robotweax-library "$diagnostic_install/lib/libsrt.so.0.2.4"
  --haivision-peer "$work_root/telemetry-haivision-build/robotweax_srt_telemetry_public_srt_incident_peer"
  --haivision-library "$work_root/haivision-install/lib/libsrt.so.1.5.7"
)

run_case() {
  local name=$1 active_pps=$2
  local trace="$output_root/$name/sender-drop-trace.jsonl"
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
  test "$code" -eq 0
  test -s "$trace"
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
