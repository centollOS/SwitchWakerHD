#!/usr/bin/env python3
"""Summarize WWHD_VK_STUTTER_TRACE CSV (frame intervals end at swap completion).

Pipeline keys are diagnostic hashes, not collision-free serialized recipe identities.
No pipeline-cache-hit inference is made from creation duration.
"""
import argparse
import bisect
import csv
import json
import math


def summarize(path, worst=20, frame_from=None, frame_to=None):
    with open(path, newline="") as source:
        events = list(csv.DictReader(source))
    for event in events:
        for key in ("frame", "start_ns", "end_ns", "thread", "result"):
            event[key] = int(event[key])
        if event["end_ns"] < event["start_ns"]:
            raise ValueError("negative event duration")
        event["cpu_ns"] = int(event.get("cpu_ns", -1))
        event["duration_ms"] = (event["end_ns"] - event["start_ns"]) / 1e6
    frames = [e for e in events if e["kind"] == "frame"]
    if frame_from is not None or frame_to is not None:
        frames = [e for e in frames if (frame_from is None or e["frame"] >= frame_from) and
                  (frame_to is None or e["frame"] < frame_to)]
        # Scope background work by execution time, not its request's frame ID.
        # This also handles discontinuous selections after a frame-ID reset.
        windows = sorted((e["start_ns"], e["end_ns"]) for e in frames)
        starts = [start for start, _ in windows]
        def in_window(event):
            index = bisect.bisect_right(starts, event["start_ns"]) - 1
            return index >= 0 and event["start_ns"] < windows[index][1]
        events = [e for e in events if e["kind"] != "frame" and in_window(e)] + frames
    pipelines = [e for e in events if e["kind"].endswith("_pipeline") or e["kind"].endswith("_pipeline_retry")]
    requests = [e for e in events if e["kind"] == "pipeline_request"]
    pending = [e for e in events if e["kind"] == "pipeline_pending"]
    shader_pending = [e for e in events if e["kind"] == "shader_pending"]
    translation_pending = [e for e in events if e["kind"] == "translation_wait"]
    prerequisites = [e for e in events if e["kind"] == "pipeline_prerequisites"]
    def execution_summary(selected):
        sampled = [e for e in selected if e["cpu_ns"] >= 0]
        return dict(count=len(selected), total_ms=sum(e["duration_ms"] for e in selected),
                    max_ms=max((e["duration_ms"] for e in selected), default=0),
                    sampled_calls=len(sampled), unavailable_calls=len(selected)-len(sampled),
                    calling_thread_cpu_ms=sum(e["cpu_ns"] for e in sampled)/1e6)
    repeats = [e for e in events if e["kind"] == "repeated_frame"]
    repeat_ages = [e["duration_ms"] for e in events if e["kind"] == "repeated_frame_age"]
    render_threads = {e["thread"] for e in frames}
    shader_events = [e for e in events if e["kind"] in ("shader_translation", "spirv_compile")]
    intervals = sorted(e["duration_ms"] for e in frames)
    compile_events = pipelines + [e for e in events if e["kind"] in ("spirv_compile", "utility_spirv_compile")]
    cpu_by_kind = {}
    for kind in sorted({e["kind"] for e in compile_events}):
        matching = [e for e in compile_events if e["kind"] == kind]
        sampled = [e for e in matching if e["cpu_ns"] >= 0]
        cpu_by_kind[kind] = dict(sampled_calls=len(sampled), unavailable_calls=len(matching)-len(sampled),
                                calling_thread_cpu_ms=sum(e["cpu_ns"] for e in sampled) / 1e6)
    startup = [dict(elapsed_ms=e["duration_ms"],
                    calling_thread_cpu_ms=e["cpu_ns"] / 1e6 if e["cpu_ns"] >= 0 else None,
                    published_ready=int(e["key"], 16), failures=e["result"],
                    start_ns=e["start_ns"], end_ns=e["end_ns"])
               for e in events if e["kind"] == "startup_warmup"]


    def percentile(p):
        return intervals[max(0, math.ceil(len(intervals) * p) - 1)] if intervals else None

    slowest = []
    for frame in sorted(frames, key=lambda e: e["duration_ms"], reverse=True)[:worst]:
        # Timestamp containment remains valid even when a save state resets frame IDs.
        compilations = [e for e in pipelines if
                        frame["start_ns"] <= e["start_ns"] < frame["end_ns"]]
        slowest.append(dict(frame=frame["frame"], duration_ms=frame["duration_ms"],
                            pipeline_compilations=compilations,
                            shader_pending=[e for e in shader_pending if frame["start_ns"] <= e["start_ns"] < frame["end_ns"]],
                            pipeline_pending=[e for e in pending if frame["start_ns"] <= e["start_ns"] < frame["end_ns"]],
                            repeated_presentations=sum(frame["start_ns"] <= e["start_ns"] < frame["end_ns"] for e in repeats),
                            shader_events=[e for e in shader_events if
                                           frame["start_ns"] <= e["start_ns"] < frame["end_ns"]],
                            render_thread_compilations=sum(e["thread"] == frame["thread"] for e in compilations),
                            pipeline_ms=sum(e["duration_ms"] for e in compilations)))
    return dict(frame_range=dict(first_inclusive=frame_from, last_exclusive=frame_to),
                frame_count=len(frames), median_ms=percentile(.5), p95_ms=percentile(.95),
                p99_ms=percentile(.99), max_ms=percentile(1),
                frames_over_ms={str(t): sum(v > t for v in intervals) for t in (16.67, 33.33, 50, 100)},
                compilation_cpu_by_kind=cpu_by_kind,
                startup_warmup=startup,
                pipeline_creation_calls=len(pipelines),
                startup_pipeline_hits=sum(e["kind"] == "startup_pipeline_hit" for e in events),
                demand_requests=len(requests),
                max_demand_queue_depth=max((e["result"] for e in requests), default=0),
                pending_execution=dict(count=len(pending),
                                       total_ms=sum(e["duration_ms"] for e in pending),
                                       max_ms=max((e["duration_ms"] for e in pending), default=0)),
                shader_requests=sum(e["kind"] == "shader_request" for e in events),
                max_shader_queue_depth=max((e["result"] for e in events if e["kind"] == "shader_request"), default=0),
                shader_pending_execution=dict(count=len(shader_pending),
                    total_ms=sum(e["duration_ms"] for e in shader_pending),
                    max_ms=max((e["duration_ms"] for e in shader_pending), default=0)),
                translation_pending_execution=execution_summary(translation_pending),
                pipeline_prerequisites=execution_summary(prerequisites),
                render_thread_shader_translations=sum(e["kind"] == "shader_translation" and e["thread"] in render_threads for e in shader_events),
                other_thread_shader_translations=sum(e["kind"] == "shader_translation" and e["thread"] not in render_threads for e in shader_events),
                render_thread_spirv_compiles=sum(e["kind"] == "spirv_compile" and e["thread"] in render_threads for e in shader_events),
                other_thread_spirv_compiles=sum(e["kind"] == "spirv_compile" and e["thread"] not in render_threads for e in shader_events),
                repeated_presentations=len(repeats),
                max_repeated_picture_age_ms=max(repeat_ages, default=0),
                render_thread_pipeline_creation_calls=sum(e["thread"] in render_threads for e in pipelines),
                other_thread_pipeline_creation_calls=sum(e["thread"] not in render_threads for e in pipelines),
                diagnostic_unique_guest_pipeline_hashes=len({e["key"] for e in pipelines if e["kind"].startswith("guest_pipeline")}),
                pipeline_creation_ms=sum(e["duration_ms"] for e in pipelines),
                pipeline_calls_by_kind={kind: sum(e["kind"] == kind for e in pipelines)
                                        for kind in sorted({e["kind"] for e in pipelines})},
                failed_creation_calls=sum(e["result"] != 0 for e in pipelines),
                shader_costs={kind: dict(count=sum(e["kind"] == kind for e in shader_events),
                                         total_ms=sum(e["duration_ms"] for e in shader_events if e["kind"] == kind))
                              for kind in ("shader_translation", "spirv_compile")},
                worst_frames=slowest,
                limitations=["CPU samples measure only the calling thread, excluding driver/helper threads; unavailable samples are not zero cost. Startup worker lifetime includes reading, waiting and cleanup and overlaps compilation samples.",
                             "Warmup completion means the finite queue finished/cancelled, not that all scene pipelines are available; check published-ready and failure counts and runtime cancellation logs.",
                             "Utility pipeline keys are not recorded; guest keys are diagnostic hashes.",
                             "Guest frame intervals retain the full pause; repeats do not turn them into new guest frames.",
                             "Pending execution and picture age are software delay measures, not physical input-to-photon latency.",
                             "First interval omitted; frame IDs can reset on state restore.",
                             "Driver cache hits are unknown. Check runtime log for dropped events and successful trace write."])


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace")
    parser.add_argument("--worst", type=int, default=20)
    parser.add_argument("--frame-from", type=int, help="include events at or after this frame ID")
    parser.add_argument("--frame-to", type=int, help="exclude events at or after this frame ID")
    args = parser.parse_args()
    print(json.dumps(summarize(args.trace, args.worst, args.frame_from, args.frame_to), indent=2))
