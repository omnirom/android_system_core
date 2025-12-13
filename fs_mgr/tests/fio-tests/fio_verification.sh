#!/bin/sh

# Copyright 2025 Google Inc. All rights reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# fio_verification.sh: Script to run on the device to do basic data
# verification.

# --- Configuration ---
# The path to the Fio binary - This should be switched to /system/bin/fio when
# FIO is built into images for userdebug builds. For now, fio binary has to be
# pushed to the device.
FIO_BIN="/data/fio/fio"

# The base directory for test files and logs is now passed as the new first argument.
BASE_DIR="$1"

# Test file and log directory on the device.
TEST_FILE_BASE="${BASE_DIR}/test"
FIO_LOG_DIR="${BASE_DIR}"

# --- Fio Test Parameters ---
FILE_SIZE="100m"
BLOCK_SIZES="4k 8k 16k 32k 64k 128k 256k 512k 1m 4m 8m 16m"
O_DIRECT="0 1"
IO_ENGINES="psync mmap"
RW_MODES="read randread"

TEST_RESULTS=() # To store results for the final summary

# --- Function to display help message ---
show_help() {
    echo "Usage: $(basename "$0") <base_directory> [-h|--help]"
    echo ""
    echo "This script performs data integrity verification using Fio."
    echo ""
    echo "Arguments:"
    echo "  <base_directory>       The base directory for test files and logs. Directory will be created."
    echo "                         E.g., /data/fio_tests or /home/user/fio_results."
    echo ""
    echo "Options:"
    echo "  -h, --help             Show this help message and exit."
    exit 0
}

# --- Pre-Checks ---

# --- Check for help flag ---
if [ "$1" = "-h" ] || [ "$1" = "--help" ]; then
    show_help
fi

# --- Check for required arguments ---
if [ -z "$FIO_BIN" ] || [ -z "$BASE_DIR" ]; then
    echo "Error: Missing required arguments."
    show_help
fi

# Ensure FIO_BIN is provided and exists
if [ -z "$FIO_BIN" ] || [ ! -x "$FIO_BIN" ]; then
    echo "Error: Fio binary path not provided or not executable: $FIO_BIN"
    show help
fi

# --- Prepare the test directory ---
echo "Preparing test directory at: $BASE_DIR"
if [ -d "$BASE_DIR" ]; then
    echo "  Deleting existing directory..."
    rm -rf "$BASE_DIR"
fi
echo "  Creating new directory..."
mkdir -p "$BASE_DIR" || { echo "Failed to create directory: $BASE_DIR"; exit 1; }


# Create test data and log directory on the device
mkdir -p "$FIO_LOG_DIR" || { echo "Failed to create log directory: $FIO_LOG_DIR"; exit 1; }

echo "Starting Fio data verification test (Device-side execution)..."
echo "Fio binary path: $FIO_BIN"
echo "Target file base: ${TEST_FILE_BASE}_<bs>_<ioengine>_<rwmode>.img"
echo "File size: $FILE_SIZE"
echo "Block sizes to test: $BLOCK_SIZES"
echo "I/O Engines to test: $IO_ENGINES"
echo "Read/Verification modes to test: $RW_MODES"
echo "Logs will be saved in: $FIO_LOG_DIR"
echo "--------------------------------------------------------"

VERIFICATION_FAILURES=0

for IOENGINE in $IO_ENGINES; do
    echo "========================================================"
    echo "             Testing with IOENGINE: $IOENGINE           "
    echo "========================================================"

  for DIRECT in $O_DIRECT; do
    for BS in $BLOCK_SIZES; do
        CURRENT_FILE_UNIQUE_ID="${BS}_${IOENGINE}"
        UNCCHED_STATUS="N/A"
        ADDITIONAL_OPTIONS=""

        if [ "$IOENGINE" = "pvsync2" ]; then
            CURRENT_FILE_UNIQUE_ID="${BS}_${IOENGINE}_uncached"
            UNCCHED_STATUS="YES"
            ADDITIONAL_OPTIONS="uncached=1"
        fi
        CURRENT_TEST_FILE="${TEST_FILE_BASE}_${CURRENT_FILE_UNIQUE_ID}.img"

        echo ""
        echo "--- Testing Block Size: $BS, IOENGINE: $IOENGINE O_DIRECT: $DIRECT"
        echo "Current test file for write: $CURRENT_TEST_FILE"

        WRITE_CONFIG_FILE="${FIO_LOG_DIR}/fio_write_config_${CURRENT_FILE_UNIQUE_ID}.ini"
        WRITE_LOG_FILE="${FIO_LOG_DIR}/fio_write_${CURRENT_FILE_UNIQUE_ID}.log"

        cat << EOF > "$WRITE_CONFIG_FILE"
[global]
ioengine=${IOENGINE}
rw=write
bs=${BS}
direct=${DIRECT}
verify=sha256
do_verify=1
verify_dump=1
fsync=1
${ADDITIONAL_OPTIONS}

[write_job]
filename=${CURRENT_TEST_FILE}
size=${FILE_SIZE}
EOF

        # Execute Fio using the full path to the binary passed as argument
        "$FIO_BIN" "$WRITE_CONFIG_FILE" > "$WRITE_LOG_FILE" 2>&1
        WRITE_EXIT_CODE=$?

        if [ $WRITE_EXIT_CODE -ne 0 ]; then
            echo "Error: Fio write failed for BS=$BS, IOENGINE=$IOENGINE, O_DIRECT=$DIRECT Exit code: $WRITE_EXIT_CODE."
            echo "See $WRITE_LOG_FILE for details."
            VERIFICATION_FAILURES=$((VERIFICATION_FAILURES + 1))
            TEST_RESULTS+=("${BS} ${IOENGINE} ${DIRECT} N/A FAILED (Write Error)")
            rm -f "$WRITE_CONFIG_FILE"
            continue
        else
            echo "Initial data write complete for BS=$BS, IOENGINE=$IOENGINE O_DIRECT=$DIRECT"
            echo "Data flushed to disk using fsync."
        fi
        rm -f "$WRITE_CONFIG_FILE"
        echo "Write output logged to: $WRITE_LOG_FILE"

        for RW_MODE in $RW_MODES; do
            echo ""
            echo "Phase 2: Verifying data in $CURRENT_TEST_FILE using RW_MODE: $RW_MODE."

            VERIFY_CONFIG_FILE="${FIO_LOG_DIR}/fio_verify_config_${CURRENT_FILE_UNIQUE_ID}_${RW_MODE}.ini"
            VERIFY_LOG_FILE="${FIO_LOG_DIR}/fio_verify_${CURRENT_FILE_UNIQUE_ID}_${RW_MODE}.log"

            cat << EOF > "$VERIFY_CONFIG_FILE"
[global]
ioengine=${IOENGINE}
rw=${RW_MODE}
bs=${BS}
direct=${DIRECT}
verify=sha256
do_verify=1
verify_dump=1
${ADDITIONAL_OPTIONS}

[verify_job]
filename=${CURRENT_TEST_FILE}
size=${FILE_SIZE}
EOF

            # Execute Fio using the full path to the binary passed as argument
            "$FIO_BIN" "$VERIFY_CONFIG_FILE" > "$VERIFY_LOG_FILE" 2>&1
            VERIFY_EXIT_CODE=$?

            if [ $VERIFY_EXIT_CODE -ne 0 ]; then
                echo "Verification FAILED for BS=$BS, IOENGINE=$IOENGINE, O_DIRECT=$DIRECT, RW_MODE=$RW_MODE. See $VERIFY_LOG_FILE for details."
                VERIFICATION_FAILURES=$((VERIFICATION_FAILURES + 1))
                TEST_RESULTS+=("${BS} ${IOENGINE} ${DIRECT} ${RW_MODE} FAILED (Verification Error)")
            else
                echo "Verification SUCCESSFUL for BS=$BS, IOENGINE=$IOENGINE, O_DIRECT=$DIRECT, RW_MODE=$RW_MODE."
                TEST_RESULTS+=("${BS} ${IOENGINE} ${DIRECT} ${RW_MODE} PASS")
            fi
            rm -f "$VERIFY_CONFIG_FILE"
            echo "Verification output logged to: $VERIFY_LOG_FILE"
            echo "--------------------------------------------------------"
        done
        rm -f "$CURRENT_TEST_FILE"
        echo "$CURRENT_TEST_FILE removed."
        echo "--------------------------------------------------------"
    done
  done
done

echo ""
echo "--- Test Summary ---"
echo ""

printf "%-10s %-10s %-10s %-10s %-25s\n" "Block Size" "IO Engine" "O_DIRECT" "RW Mode" "Result"
printf "%s\n" "---------------------------------------------------------------------------------"

for RESULT in "${TEST_RESULTS[@]}"; do
    printf "%-10s %-10s %-10s %-10s %-25s\n" $(echo "$RESULT")
done

printf "%s\n" "---------------------------------------------------------------------------------"

if [ $VERIFICATION_FAILURES -eq 0 ]; then
    echo "✅ All data verification tests passed successfully across all configurations!"
    exit 0
else
    echo "❌ Some data verification tests failed. Total failures: $VERIFICATION_FAILURES"
    echo "Please review the logs in $FIO_LOG_DIR for more details."
    exit 1
fi
