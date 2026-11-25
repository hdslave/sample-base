#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# After ac50.service stops, clear only the developer-use record on known
# 1.4 firmwares. Never write an absent marker or guess at an unreadable env.
# Best effort: a failure must not prevent the app or stock recovery running.
[ "$(uname -m)" = aarch64 ] || exit 0
case "$(sha256sum '/usr/bin/MPC Sample' 2>/dev/null | cut -d ' ' -f 1)" in
    2271252ffded26830546d4ee1852e39a2c1f4c574a40c47fba10b5a397727ec2) ;;
    473fb7260e597e61cc55d9d30595eb9d9aa1304ff1b88960e633ad19457dc91a) ;;
    *) exit 0 ;;
esac
warn() { echo "sample-base: developer-use marker cleanup: $*" >&2; }
command -v fw_printenv >/dev/null 2>&1 && command -v fw_setenv >/dev/null 2>&1 || {
    warn 'environment tools unavailable'; exit 0;
}
ENVIRONMENT=$(fw_printenv 2>/dev/null) || { warn 'cannot read environment; left unchanged'; exit 0; }
printf '%s\n' "$ENVIRONMENT" | grep -q '^imb-dev-mode-used=' || exit 0
fw_setenv imb-dev-mode-used || { warn 'could not clear marker'; exit 0; }
ENVIRONMENT=$(fw_printenv 2>/dev/null) || { warn 'cannot verify removal'; exit 0; }
if printf '%s\n' "$ENVIRONMENT" | grep -q '^imb-dev-mode-used='; then
    warn 'marker still present'
else
    echo 'sample-base: cleared imb-dev-mode-used'
fi
exit 0
