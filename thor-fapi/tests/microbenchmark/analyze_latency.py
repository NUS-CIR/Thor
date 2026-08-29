#!/usr/bin/env python3
"""Analyze nfapi-proxy ARRIVE/DEPART processing-latency events."""

import argparse
import csv
import json
from collections import defaultdict


def load_events(path):
    events = []
    with open(path, encoding="utf-8") as stream:
        for row in csv.reader(stream):
            if len(row) != 7:
                continue
            timestamp, direction, message_type, sfn, slot, pnf, event = row
            events.append((int(timestamp), direction, message_type,
                           int(sfn), int(slot), int(pnf), event))
    events.sort(key=lambda item: item[0])
    return events


def match_north(events):
    pending = {}
    completed = []
    for timestamp, direction, message_type, sfn, slot, _pnf, event in events:
        if direction != "NORTH":
            continue
        key = (message_type, sfn, slot)
        if event == "ARRIVE":
            previous = pending.pop(key, None)
            if previous is not None and previous[2]:
                completed.append(previous)
            pending[key] = [message_type, timestamp, []]
        elif event == "DEPART" and key in pending:
            pending[key][2].append(timestamp)
            if len(pending[key][2]) >= 2:
                completed.append(pending.pop(key))
    completed.extend(item for item in pending.values() if item[2])
    return completed


def match_south(events, max_pnfs):
    pending = {}
    completed = []
    for timestamp, direction, message_type, sfn, slot, pnf, event in events:
        if direction != "SOUTH":
            continue
        key = (message_type, sfn, slot)
        if event == "ARRIVE":
            state = pending.setdefault(key, [message_type, {}])
            if len(state[1]) >= max_pnfs and pnf not in state[1]:
                state = [message_type, {}]
                pending[key] = state
            state[1][pnf] = timestamp
        elif event == "DEPART" and key in pending and pending[key][1]:
            state = pending.pop(key)
            completed.append((state[0], list(state[1].values()), timestamp))
    return completed


def percentile(values, percent):
    if not values:
        return None
    ordered = sorted(values)
    position = (len(ordered) - 1) * percent / 100
    lower = int(position)
    upper = min(lower + 1, len(ordered) - 1)
    value = ordered[lower] + (ordered[upper] - ordered[lower]) * (position - lower)
    return round(value, 3)


def summarize(values):
    if not values:
        return None
    return {
        "samples": len(values),
        "p50_us": percentile(values, 50),
        "p90_us": percentile(values, 90),
        "p99_us": percentile(values, 99),
        "max_us": max(values),
    }


def summarize_by_type(samples):
    grouped = defaultdict(list)
    for message_type, latency in samples:
        grouped[message_type].append(latency)
    return {name: summarize(values) for name, values in sorted(grouped.items())}


def analyze(path, max_pnfs, configuration=None):
    events = load_events(path)
    north = match_north(events)
    south = match_south(events, max_pnfs)

    # NORTH is complete after the final routed copy leaves the proxy.
    north_samples = [(message_type, max(departs) - arrival)
                     for message_type, arrival, departs in north
                     if departs and max(departs) >= arrival]
    # SOUTH merge work begins after the final contributing L1 packet arrives.
    south_samples = [(message_type, departure - max(arrivals))
                     for message_type, arrivals, departure in south
                     if arrivals and departure >= max(arrivals)]

    result = {
        "events": len(events),
        "dl": {
            "overall": summarize([value for _, value in north_samples]),
            "by_message_type": summarize_by_type(north_samples),
        },
        "ul": {
            "overall": summarize([value for _, value in south_samples]),
            "by_message_type": summarize_by_type(south_samples),
        },
    }
    if configuration is not None:
        result = {"configuration": configuration, **result}
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("csv", nargs="?", default="/tmp/proxy_message_log.csv")
    parser.add_argument("--max-pnfs", type=int, default=2)
    parser.add_argument("--output-json")
    parser.add_argument("--payload-size", type=int)
    parser.add_argument("--ues", type=int)
    parser.add_argument("--seed", type=int)
    parser.add_argument("--migrated-rnti")
    parser.add_argument("--migration-target", type=int)
    args = parser.parse_args()
    configuration = None
    if any(value is not None for value in (
            args.payload_size, args.ues, args.seed, args.migrated_rnti,
            args.migration_target)):
        configuration = {
            "payload_size_bytes_per_ue": args.payload_size,
            "ue_count": args.ues,
            "seed": args.seed,
            "migrated_rnti": args.migrated_rnti,
            "migration_target_l1": args.migration_target,
        }
    rendered = json.dumps(analyze(args.csv, args.max_pnfs, configuration), indent=2)
    if args.output_json:
        with open(args.output_json, "w", encoding="utf-8") as stream:
            stream.write(rendered)
            stream.write("\n")
    print(rendered)


if __name__ == "__main__":
    main()
