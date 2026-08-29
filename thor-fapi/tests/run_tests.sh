#!/bin/bash

# Test script for nfapi-proxy P7 message handling
# This script starts the nfapi-proxy in test mode and runs test stubs

set -e

# Color codes for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Default configuration
TEST_DURATION=10
VNF_PORT=60001
PNF_PORT_0=60010
PNF_PORT_1=60011
PROXY_P7_PORT=50012
CONTROL_SOCKET=/tmp/nfapi_proxy_control.sock
BUILD_DIR="build"
PROXY_BIN="$BUILD_DIR/nfapi-proxy"
VNF_STUB_BIN="$BUILD_DIR/test_vnf_stub"
PNF_STUB_BIN="$BUILD_DIR/test_pnf_stub"

# PIDs to track
PROXY_PID=""
VNF_STUB_PID=""
PNF_STUB_0_PID=""
PNF_STUB_1_PID=""

# Cleanup function
cleanup() {
    echo -e "${YELLOW}Cleaning up processes...${NC}"
    
    if [ ! -z "$VNF_STUB_PID" ]; then
        kill -TERM $VNF_STUB_PID 2>/dev/null || true
        wait $VNF_STUB_PID 2>/dev/null || true
    fi
    
    if [ ! -z "$PNF_STUB_0_PID" ]; then
        kill -TERM $PNF_STUB_0_PID 2>/dev/null || true
        wait $PNF_STUB_0_PID 2>/dev/null || true
    fi
    
    if [ ! -z "$PNF_STUB_1_PID" ]; then
        kill -TERM $PNF_STUB_1_PID 2>/dev/null || true
        wait $PNF_STUB_1_PID 2>/dev/null || true
    fi
    
    if [ ! -z "$PROXY_PID" ]; then
        kill -TERM $PROXY_PID 2>/dev/null || true
        wait $PROXY_PID 2>/dev/null || true
    fi
    
    echo -e "${GREEN}Cleanup complete${NC}"
}

# Set trap for cleanup
trap cleanup EXIT INT TERM

# Parse command line arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --duration)
            TEST_DURATION="$2"
            shift 2
            ;;
        --build-dir)
            BUILD_DIR="$2"
            PROXY_BIN="$BUILD_DIR/nfapi-proxy"
            VNF_STUB_BIN="$BUILD_DIR/test_vnf_stub"
            PNF_STUB_BIN="$BUILD_DIR/test_pnf_stub"
            shift 2
            ;;
        --help)
            echo "Usage: $0 [options]"
            echo "Options:"
            echo "  --duration <seconds>  Test duration in seconds (default: 10)"
            echo "  --build-dir <path>    Build directory path (default: build)"
            echo "  --help                Show this help message"
            exit 0
            ;;
        *)
            echo "Unknown option: $1"
            exit 1
            ;;
    esac
done

echo -e "${GREEN}========================================${NC}"
echo -e "${GREEN}NFAPI Proxy P7 Message Handling Tests${NC}"
echo -e "${GREEN}========================================${NC}"
echo ""
echo "Configuration:"
echo "  Test duration: ${TEST_DURATION}s"
echo "  VNF port: $VNF_PORT"
echo "  PNF 0 port: $PNF_PORT_0"
echo "  PNF 1 port: $PNF_PORT_1"
echo "  Proxy P7 port: $PROXY_P7_PORT"
echo "  Build directory: $BUILD_DIR"
echo ""

# Check if binaries exist
if [ ! -f "$PROXY_BIN" ]; then
    echo -e "${RED}Error: nfapi-proxy binary not found at $PROXY_BIN${NC}"
    echo "Please build the project first with: cmake -B $BUILD_DIR && cmake --build $BUILD_DIR"
    exit 1
fi

if [ ! -f "$VNF_STUB_BIN" ]; then
    echo -e "${RED}Error: test_vnf_stub binary not found at $VNF_STUB_BIN${NC}"
    echo "Please build the test stubs first"
    exit 1
fi

if [ ! -f "$PNF_STUB_BIN" ]; then
    echo -e "${RED}Error: test_pnf_stub binary not found at $PNF_STUB_BIN${NC}"
    echo "Please build the test stubs first"
    exit 1
fi

echo -e "${YELLOW}Starting nfapi-proxy in test mode...${NC}"
THOR_CTRL_SOCK="$CONTROL_SOCKET" "$PROXY_BIN" --test-mode --test-vnf-port "$VNF_PORT" --test-pnf-port "$PNF_PORT_0,$PNF_PORT_1" --p7-port "$PROXY_P7_PORT" > /tmp/nfapi-proxy-test.log 2>&1 &
PROXY_PID=$!
echo "  PID: $PROXY_PID"
sleep 2

# Check if proxy is still running
if ! kill -0 $PROXY_PID 2>/dev/null; then
    echo -e "${RED}Error: nfapi-proxy failed to start${NC}"
    echo "Log output:"
    cat /tmp/nfapi-proxy-test.log
    exit 1
fi

echo -e "${YELLOW}Starting VNF test stub...${NC}"
$VNF_STUB_BIN $VNF_PORT $PROXY_P7_PORT > /tmp/vnf-stub-test.log 2>&1 &
VNF_STUB_PID=$!
echo "  PID: $VNF_STUB_PID"

echo -e "${YELLOW}Starting PNF test stub 0...${NC}"
$PNF_STUB_BIN $PNF_PORT_0 $PROXY_P7_PORT 0 > /tmp/pnf-stub-0-test.log 2>&1 &
PNF_STUB_0_PID=$!
echo "  PID: $PNF_STUB_0_PID"

echo -e "${YELLOW}Starting PNF test stub 1...${NC}"
$PNF_STUB_BIN $PNF_PORT_1 $PROXY_P7_PORT 1 > /tmp/pnf-stub-1-test.log 2>&1 &
PNF_STUB_1_PID=$!
echo "  PID: $PNF_STUB_1_PID"

echo ""
echo -e "${GREEN}All processes started successfully!${NC}"
echo -e "${GREEN}Running test for ${TEST_DURATION} seconds...${NC}"
echo ""

# Wait for test duration
sleep $TEST_DURATION

echo ""
echo -e "${GREEN}Test completed!${NC}"
echo ""
echo -e "${YELLOW}Checking logs for errors...${NC}"

# Check logs for errors
ERRORS=0

if grep -i "error\|failed" /tmp/nfapi-proxy-test.log > /dev/null 2>&1; then
    echo -e "${RED}Found errors in nfapi-proxy log${NC}"
    ERRORS=$((ERRORS + 1))
fi

if grep -i "error\|failed" /tmp/vnf-stub-test.log > /dev/null 2>&1; then
    echo -e "${RED}Found errors in VNF stub log${NC}"
    ERRORS=$((ERRORS + 1))
fi

if grep -i "error\|failed" /tmp/pnf-stub-0-test.log > /dev/null 2>&1; then
    echo -e "${RED}Found errors in PNF stub 0 log${NC}"
    ERRORS=$((ERRORS + 1))
fi

if grep -i "error\|failed" /tmp/pnf-stub-1-test.log > /dev/null 2>&1; then
    echo -e "${RED}Found errors in PNF stub 1 log${NC}"
    ERRORS=$((ERRORS + 1))
fi

echo ""
echo -e "${YELLOW}Log files:${NC}"
echo "  nfapi-proxy: /tmp/nfapi-proxy-test.log"
echo "  VNF stub:    /tmp/vnf-stub-test.log"
echo "  PNF stub 0:  /tmp/pnf-stub-0-test.log"
echo "  PNF stub 1:  /tmp/pnf-stub-1-test.log"
echo ""

if [ $ERRORS -eq 0 ]; then
    echo -e "${GREEN}========================================${NC}"
    echo -e "${GREEN}Tests PASSED!${NC}"
    echo -e "${GREEN}========================================${NC}"
    exit 0
else
    echo -e "${RED}========================================${NC}"
    echo -e "${RED}Tests FAILED with $ERRORS error(s)${NC}"
    echo -e "${RED}========================================${NC}"
    exit 1
fi
