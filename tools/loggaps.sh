#!/bin/sh
# loggaps.sh LOG PROCESS [SECONDS]: where PROCESS (e.g. Preferences) went quiet in an oslog capture, i.e. the
# line before each gap of at least SECONDS (default 0.4) between its log lines, and the line after it. Run on the phone.
log=$1; proc=$2; min=${3:-0.4}
grep -a "$proc\[" "$log" | awk -v min="$min" '{
	split($1, t, ":"); s = t[1] * 3600 + t[2] * 60 + t[3]
	if (NR > 1 && s - last >= min) { printf "---- %.2f s quiet after:\n%s\n%s\n", s - last, substr(prev, 1, 220), substr($0, 1, 220) }
	last = s; prev = $0
}'
