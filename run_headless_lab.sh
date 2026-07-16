#!/usr/bin/env sh
# SPDX-License-Identifier: Apache-2.0
# Convenience forwarder. The lab lives with its sources: test_lab/run_headless_lab.sh
exec "$(dirname "$0")/test_lab/run_headless_lab.sh" "$@"
