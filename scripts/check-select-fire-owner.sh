#!/usr/bin/env bash
# check-select-fire-owner.sh — the select claim+wake discipline has ONE owner.
#
# Rule (DupFamily select_claim_wake_discipline, consolidated @f5848a4d):
# firing a select case — claiming SelectGroup::fired_index and waking the
# task exactly once — happens ONLY in select_fire(). MT-4b consolidated the
# two sites (chan_send's delivery arm, io_fire_waiter's inline copy); MT-5's
# send cases are exactly where a third copy would be born.
#
# Marker: assignments to fired_index in src/madc_task_chan.cpp. Exactly two
# are legal: the struct initializer (= -1) and select_fire's claim.
set -u

FILE="$(dirname "$0")/../src/madc_task_chan.cpp"

count_claims()
{
	grep -c "fired_index = " "$1"
}

n=$(count_claims "$FILE")
if [ "$n" -ne 2 ]; then
	echo "check-select-fire-owner: FAIL — $n 'fired_index = ' sites in" \
	     "src/madc_task_chan.cpp (expected 2: the initializer +" \
	     "select_fire's claim). A new claim site must route through" \
	     "select_fire — see the DupFamily select_claim_wake_discipline." >&2
	exit 1
fi

# Negative control: the check must fail on a synthetic third claim site.
tmp=$(mktemp)
cat "$FILE" > "$tmp"
echo "	w->group->fired_index = w->index;	// synthetic" >> "$tmp"
if [ "$(count_claims "$tmp")" -ne 3 ]; then
	rm -f "$tmp"
	echo "check-select-fire-owner: FAIL — negative control did not" \
	     "detect a synthetic claim site (the marker went blind)." >&2
	exit 1
fi
rm -f "$tmp"

echo "check-select-fire-owner: OK (one claim owner: select_fire)"
exit 0
