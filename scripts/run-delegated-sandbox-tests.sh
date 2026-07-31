#!/bin/sh
set -eu

if [ "${CAPSID_DELEGATED_SANDBOX_CONTAINER:-}" != "1" ]; then
    echo "refusing to modify cgroups outside the dedicated test container" >&2
    exit 2
fi

build_dir=${1:-/build}
repeat=${CAPSID_DELEGATED_SANDBOX_REPEAT:-1}
fixture="${build_dir}/generated/test-global-surface.js"
worker="${build_dir}/capsid-worker"
cgroup_test="${build_dir}/test-sandbox-cgroup"
netns_test="${build_dir}/test-sandbox-network-namespace"

case "${repeat}" in
    ''|*[!0-9]*|0)
        echo "CAPSID_DELEGATED_SANDBOX_REPEAT must be a positive integer" >&2
        exit 2
        ;;
esac

for required in \
    "${fixture}" \
    "${worker}" \
    "${cgroup_test}" \
    "${netns_test}"
do
    if [ ! -e "${required}" ]; then
        echo "missing delegated sandbox test artifact: ${required}" >&2
        exit 2
    fi
done

if [ ! -r /sys/fs/cgroup/cgroup.controllers ]; then
    echo "delegated sandbox job requires a cgroup v2 mount" >&2
    exit 1
fi
if [ ! -w /sys/fs/cgroup/cgroup.procs ] ||
   [ ! -w /sys/fs/cgroup/cgroup.subtree_control ]; then
    echo "delegated sandbox job requires writable cgroup v2 delegation" >&2
    exit 1
fi

driver_cgroup=/sys/fs/cgroup/capsid-delegated-driver
mkdir "${driver_cgroup}"
echo "$$" >"${driver_cgroup}/cgroup.procs"
echo "+cpu +memory +pids" >/sys/fs/cgroup/cgroup.subtree_control

run_required_pass() {
    label=$1
    executable=$2
    round=1
    while [ "${round}" -le "${repeat}" ]; do
        status=0
        "${executable}" "${worker}" "${fixture}" || status=$?
        if [ "${status}" -eq 77 ]; then
            echo "${label} round ${round}/${repeat}: SKIP is a CI failure" >&2
            exit 1
        fi
        if [ "${status}" -ne 0 ]; then
            echo "${label} round ${round}/${repeat}: exit ${status}" >&2
            exit "${status}"
        fi
        echo "${label} round ${round}/${repeat}: PASS"
        round=$((round + 1))
    done
}

run_required_pass "cgroup-v2" "${cgroup_test}"
run_required_pass "network-namespace" "${netns_test}"
