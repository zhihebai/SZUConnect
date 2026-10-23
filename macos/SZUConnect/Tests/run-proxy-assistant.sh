#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
task_dir=$(mktemp -d "${TMPDIR:-/tmp}/szu-proxy-tests.XXXXXX")
trap 'rm -rf "$task_dir"' EXIT
xcrun swiftc -Osize -whole-module-optimization -parse-as-library \
  Sources/ProxyAssistantConfig.swift Sources/ProxyAssistantFiles.swift Tests/ProxyAssistantTests.swift \
  -o "$task_dir/proxy-assistant-tests"
"$task_dir/proxy-assistant-tests" "$task_dir/generated-script.js"
# Executes only the fixed production script against synthetic inputs, never a selected file.
node ../../windows/SZUConnect/tests/proxy_script_tests.mjs --script-file "$task_dir/generated-script.js"
