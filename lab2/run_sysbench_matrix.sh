#!/usr/bin/env bash
set -euo pipefail

# Run Lab2 sysbench matrix:
# (10,10), (50,50), (100,100) × {miniob_insert, miniob_select, miniob_delete}
# tables=5 table_size=1000 time=60, and disable secondary index creation.
#
# Usage:
#   bash lab2/run_sysbench_matrix.sh
#   PORT=6789 TIME=60 bash lab2/run_sysbench_matrix.sh
#   SYSBENCH_BIN=~/.local/sysbench-mysql/bin/sysbench bash lab2/run_sysbench_matrix.sh

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PORT="${PORT:-6789}"
TIME_SEC="${TIME:-60}"
TABLES="${TABLES:-5}"
TABLE_SIZE="${TABLE_SIZE:-1000}"
CREATE_SECONDARY="${CREATE_SECONDARY:-0}"

SYSBENCH_BIN="${SYSBENCH_BIN:-}"
if [[ -z "${SYSBENCH_BIN}" ]]; then
  if [[ -x "${HOME}/.local/sysbench-mysql/bin/sysbench" ]]; then
    SYSBENCH_BIN="${HOME}/.local/sysbench-mysql/bin/sysbench"
  else
    SYSBENCH_BIN="sysbench"
  fi
fi

OUTDIR_REL="lab2/sysbench-results-$(date +%Y%m%d-%H%M%S)"
OUTDIR="${REPO_ROOT}/${OUTDIR_REL}"
mkdir -p "${OUTDIR}"
SUMMARY="${OUTDIR}/summary.txt"

echo "# sysbench results (MiniOB LSM)" > "${SUMMARY}"
echo "# settings: tables=${TABLES} table_size=${TABLE_SIZE} time=${TIME_SEC} create_secondary=${CREATE_SECONDARY}" >> "${SUMMARY}"
echo "# columns: param_set, workload, tps, avg_latency_ms, last_split" >> "${SUMMARY}"
echo "Using sysbench: $("${SYSBENCH_BIN}" --version | tr -d '\r')" >> "${SUMMARY}"
echo "Repo: ${REPO_ROOT}" >> "${SUMMARY}"
echo "Out: ${OUTDIR_REL}" >> "${SUMMARY}"
echo "" >> "${SUMMARY}"

wait_port() {
  local port="$1"
  python3 - <<PY
import socket, time
port=int(${port})
for _ in range(240):
  try:
    s=socket.create_connection(("127.0.0.1", port), timeout=0.2)
    s.close()
    break
  except Exception:
    time.sleep(0.25)
else:
  raise SystemExit("observer not ready")
PY
}

extract_tps() {
  local run_file="$1"
  python3 - <<PY
import re
p=re.compile(r"^\\s*transactions:\\s*\\d+\\s*\\(([^)]+) per sec\\.\\)")
with open("${run_file}", errors="ignore") as f:
  for line in f:
    m=p.search(line)
    if m:
      print(m.group(1).strip())
      break
PY
}

extract_avg_latency_ms() {
  local run_file="$1"
  python3 - <<PY
import re
in_lat=False
p=re.compile(r"^\\s*avg:\\s*([0-9.]+)")
with open("${run_file}", errors="ignore") as f:
  for line in f:
    if line.strip()=="Latency (ms):":
      in_lat=True
      continue
    if in_lat:
      m=p.search(line)
      if m:
        print(m.group(1))
        break
PY
}

last_split_line() {
  # observer log name depends on config; pick the latest observer.log* in repo root.
  local latest
  latest="$(ls -1t "${REPO_ROOT}"/observer.log* 2>/dev/null | head -n 1 || true)"
  if [[ -z "${latest}" ]]; then
    echo ""
    return 0
  fi
  python3 - <<PY
last=""
with open("${latest}", errors="ignore") as f:
  for line in f:
    if "Split." in line:
      last=line.strip()
# Report only the required format:
# Split. <leaf split counts> <internal-node split counts> <the resulting tree height>
i = last.find("Split.")
print(last[i:] if i >= 0 else "")
PY
}

run_one() {
  local ichild="$1"
  local leaf="$2"
  local workload="$3"
  local tag="c${ichild}_l${leaf}_${workload}"

  echo "=== RUN internal_max_children=${ichild} leaf_max_entries=${leaf} workload=${workload} ===" | tee -a "${SUMMARY}"

  # clean db dir & stop any existing observer on the port
  bash "${REPO_ROOT}/lab2/clean_db.sh" "${PORT}" >/dev/null 2>&1 || true

  # start observer
  MINIOB_MEMTABLE_INTERNAL_MAX_CHILDREN="${ichild}" \
  MINIOB_MEMTABLE_LEAF_MAX_ENTRIES="${leaf}" \
    "${REPO_ROOT}/build/bin/observer" -f "${REPO_ROOT}/etc/observer.ini" -P mysql -p "${PORT}" -E lsm \
      > "${OUTDIR}/observer_${tag}.out" 2>&1 &
  local opid=$!

  wait_port "${PORT}"

  # run sysbench
  (
    cd "${REPO_ROOT}/test/sysbench"
    "${SYSBENCH_BIN}" "${workload}.lua" --db-driver=mysql \
      --mysql-host=127.0.0.1 --mysql-port="${PORT}" --mysql-db=test --mysql-ssl=off \
      --tables="${TABLES}" --table_size="${TABLE_SIZE}" --create_secondary="${CREATE_SECONDARY}" \
      prepare > "${OUTDIR}/sysbench_${tag}_prepare.txt" 2>&1

    "${SYSBENCH_BIN}" "${workload}.lua" --db-driver=mysql \
      --mysql-host=127.0.0.1 --mysql-port="${PORT}" --mysql-db=test --mysql-ssl=off \
      --tables="${TABLES}" --table_size="${TABLE_SIZE}" --create_secondary="${CREATE_SECONDARY}" --time="${TIME_SEC}" \
      run > "${OUTDIR}/sysbench_${tag}_run.txt" 2>&1
  )

  # stop observer
  kill "${opid}" 2>/dev/null || true
  wait "${opid}" 2>/dev/null || true

  local run_file="${OUTDIR}/sysbench_${tag}_run.txt"
  local tps avg_lat split
  tps="$(extract_tps "${run_file}" || true)"
  avg_lat="$(extract_avg_latency_ms "${run_file}" || true)"
  split="$(last_split_line || true)"

  echo "param_set=${ichild}/${leaf} workload=${workload} tps=${tps:-NA} avg_latency_ms=${avg_lat:-NA}" | tee -a "${SUMMARY}"
  echo "last_split=${split}" | tee -a "${SUMMARY}"
  echo "" | tee -a "${SUMMARY}"
}

cd "${REPO_ROOT}"

for v in 10 50 100; do
  for w in miniob_insert miniob_select miniob_delete; do
    run_one "${v}" "${v}" "${w}"
  done
done

echo "All done. Summary: ${OUTDIR_REL}/summary.txt"

