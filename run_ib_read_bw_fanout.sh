#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage:
  run_ib_read_bw_fanout.sh --target-ipv6 <ipv6> [options]

Options:
  --target-ipv6 <ipv6>      IPv6 адрес node-1303, куда идет нагрузка (required)
  --target-host <fqdn>      FQDN целевого хоста (default: node-1303.ydb-dev.ik8s.nebiuscloud.net)
  --user <name>             Пользователь для tsh ssh (default: текущий пользователь)
  --host-start <num>        Начало диапазона нод (default: 1302)
  --host-end <num>          Конец диапазона нод (default: 1309)
  --domain <domain>         Домен нод (default: ydb-dev.ik8s.nebiuscloud.net)
  --instances <num>         Число инстансов на каждый source host (default: 16)
  --base-port <num>         Базовый порт для первой пары (default: 18516)
  --duration <sec>          Значение -D для ib_read_bw (default: 10)
  --size <bytes>            Значение -s для ib_read_bw (default: 256000)
  --device <name>           Значение -d для ib_read_bw (default: mlx5_0)
  --gid-index <num>         Значение -x для ib_read_bw (default: 5)
  --log-dir <path>          Каталог логов на удаленных хостах (default: /tmp/ib_read_bw_fanout)
  --server-warmup <sec>     Пауза после старта серверов на target (default: 2)
  --tsh-bin <path>          Путь к tsh бинарнику (default: tsh)
  --dry-run                 Только показать команды, не выполнять
  -h, --help                Показать help

Example:
  ./run_ib_read_bw_fanout.sh \
    --target-ipv6 2a13:5945:a:15e:ce40:f3ff:fe1d:a48c \
    --user robdrynkin
EOF
}

USER_NAME="${USER}"
TSH_BIN="tsh"
DOMAIN="ydb-dev.ik8s.nebiuscloud.net"
HOST_START=1302
HOST_END=1309
TARGET_HOST="node-1303.${DOMAIN}"
TARGET_IPV6=""
INSTANCES_PER_HOST=16
BASE_PORT=18516
DURATION=10
SIZE=256000
DEVICE="mlx5_0"
GID_INDEX=5
LOG_DIR="/tmp/ib_read_bw_fanout"
SERVER_WARMUP_SEC=2
DRY_RUN=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --target-ipv6)
      TARGET_IPV6="${2:?missing value for --target-ipv6}"
      shift 2
      ;;
    --target-host)
      TARGET_HOST="${2:?missing value for --target-host}"
      shift 2
      ;;
    --user)
      USER_NAME="${2:?missing value for --user}"
      shift 2
      ;;
    --host-start)
      HOST_START="${2:?missing value for --host-start}"
      shift 2
      ;;
    --host-end)
      HOST_END="${2:?missing value for --host-end}"
      shift 2
      ;;
    --domain)
      DOMAIN="${2:?missing value for --domain}"
      shift 2
      ;;
    --instances)
      INSTANCES_PER_HOST="${2:?missing value for --instances}"
      shift 2
      ;;
    --base-port)
      BASE_PORT="${2:?missing value for --base-port}"
      shift 2
      ;;
    --duration)
      DURATION="${2:?missing value for --duration}"
      shift 2
      ;;
    --size)
      SIZE="${2:?missing value for --size}"
      shift 2
      ;;
    --device)
      DEVICE="${2:?missing value for --device}"
      shift 2
      ;;
    --gid-index)
      GID_INDEX="${2:?missing value for --gid-index}"
      shift 2
      ;;
    --log-dir)
      LOG_DIR="${2:?missing value for --log-dir}"
      shift 2
      ;;
    --server-warmup)
      SERVER_WARMUP_SEC="${2:?missing value for --server-warmup}"
      shift 2
      ;;
    --tsh-bin)
      TSH_BIN="${2:?missing value for --tsh-bin}"
      shift 2
      ;;
    --dry-run)
      DRY_RUN=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown argument: $1" >&2
      usage
      exit 1
      ;;
  esac
done

if [[ -z "${TARGET_IPV6}" ]]; then
  echo "Error: --target-ipv6 is required" >&2
  usage
  exit 1
fi

if (( HOST_START > HOST_END )); then
  echo "Error: host range is invalid: ${HOST_START}..${HOST_END}" >&2
  exit 1
fi

if (( INSTANCES_PER_HOST < 1 )); then
  echo "Error: --instances must be >= 1" >&2
  exit 1
fi

run_remote_cmd() {
  local host="$1"
  local cmd="$2"
  if (( DRY_RUN )); then
    printf 'DRY-RUN: %s ssh %s@%s -- bash -lc %q\n' "${TSH_BIN}" "${USER_NAME}" "${host}" "${cmd}"
    return 0
  fi
  "${TSH_BIN}" ssh "${USER_NAME}@${host}" -- bash -lc "${cmd}"
}

declare -a SOURCE_HOSTS=()
for n in $(seq "${HOST_START}" "${HOST_END}"); do
  host="node-${n}.${DOMAIN}"
  if [[ "${host}" == "${TARGET_HOST}" ]]; then
    continue
  fi
  SOURCE_HOSTS+=("${host}")
done

if (( ${#SOURCE_HOSTS[@]} == 0 )); then
  echo "No source hosts found after excluding target host ${TARGET_HOST}" >&2
  exit 1
fi

TOTAL_INSTANCES=$(( ${#SOURCE_HOSTS[@]} * INSTANCES_PER_HOST ))
LAST_PORT=$(( BASE_PORT + TOTAL_INSTANCES - 1 ))

echo "Target host: ${TARGET_HOST}"
echo "Target IPv6: ${TARGET_IPV6}"
echo "Source hosts (${#SOURCE_HOSTS[@]}): ${SOURCE_HOSTS[*]}"
echo "Instances per source host: ${INSTANCES_PER_HOST}"
echo "Total client/server pairs: ${TOTAL_INSTANCES}"
echo "Port range: ${BASE_PORT}-${LAST_PORT}"
echo "Log dir on remote hosts: ${LOG_DIR}"

server_cmd=$(
  cat <<EOF
set -euo pipefail
mkdir -p "${LOG_DIR}"
for port in \$(seq "${BASE_PORT}" "${LAST_PORT}"); do
  nohup ib_read_bw -d "${DEVICE}" -x "${GID_INDEX}" --ipv6 --ipv6-addr --bind_source_ip "${TARGET_IPV6}" -s "${SIZE}" -D "${DURATION}" --report_gbits -F -p "\${port}" > "${LOG_DIR}/server_\${port}.log" 2>&1 &
done
EOF
)

echo "Starting ${TOTAL_INSTANCES} server instances on ${TARGET_HOST}..."
run_remote_cmd "${TARGET_HOST}" "${server_cmd}"

echo "Waiting ${SERVER_WARMUP_SEC}s for server side warmup..."
sleep "${SERVER_WARMUP_SEC}"

declare -a JOB_PIDS=()
declare -a JOB_HOSTS=()

for idx in "${!SOURCE_HOSTS[@]}"; do
  host="${SOURCE_HOSTS[$idx]}"
  host_tag="${host//./_}"
  start_port=$(( BASE_PORT + idx * INSTANCES_PER_HOST ))
  end_port=$(( start_port + INSTANCES_PER_HOST - 1 ))

  client_cmd=$(
    cat <<EOF
set -euo pipefail
mkdir -p "${LOG_DIR}"
for port in \$(seq "${start_port}" "${end_port}"); do
  ib_read_bw -d "${DEVICE}" -x "${GID_INDEX}" --ipv6 --ipv6-addr -s "${SIZE}" -D "${DURATION}" --report_gbits -F "${TARGET_IPV6}" -p "\${port}" > "${LOG_DIR}/client_${host_tag}_\${port}.log" 2>&1 &
done
wait
EOF
  )

  echo "Launching ${INSTANCES_PER_HOST} clients from ${host} (ports ${start_port}-${end_port})..."
  if (( DRY_RUN )); then
    run_remote_cmd "${host}" "${client_cmd}"
    continue
  fi
  run_remote_cmd "${host}" "${client_cmd}" &
  JOB_PIDS+=("$!")
  JOB_HOSTS+=("${host}")
done

if (( DRY_RUN )); then
  echo "DRY-RUN complete."
  exit 0
fi

failures=0
for i in "${!JOB_PIDS[@]}"; do
  pid="${JOB_PIDS[$i]}"
  host="${JOB_HOSTS[$i]}"
  if ! wait "${pid}"; then
    echo "Client batch failed on ${host}" >&2
    failures=1
  fi
done

if (( failures != 0 )); then
  echo "At least one source host failed. Check logs in ${LOG_DIR} on each host." >&2
  exit 1
fi

echo "All client batches finished successfully."
