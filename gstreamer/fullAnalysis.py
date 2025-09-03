#!/usr/bin/env python3
import os
import subprocess
from datetime import datetime
import glob
import sys
import json
import pydot
from latency import parse_and_save_latency

def parse_cluster(cluster):
    """Recursively parse a cluster into a dict structure"""
    label = cluster.get_label() or cluster.get_name()
    node = {
        "name": label,
        "type": "bin",
        "children": []
    }

    # Add subclusters (nested bins)
    for subc in cluster.get_subgraphs():
        node["children"].append(parse_cluster(subc))

    # Add elements
    for elem in cluster.get_nodes():
        label = elem.get_label() or elem.get_name()
        node["children"].append({
            "name": label,
            "type": "element"
        })

    return node

def parse_pipeline(dot_file):
    graphs = pydot.graph_from_dot_file(dot_file)
    graph = graphs[0]  # first graph

    tree = {
        "name": graph.get_name(),
        "type": "pipeline",
        "children": []
    }

    for subgraph in graph.get_subgraphs():
        tree["children"].append(parse_cluster(subgraph))

    return tree

def main():
    # --- Step 1: Create timestamped results directory ---
    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    results_dir = os.path.abspath(f"results_{timestamp}")
    dot_dir = os.path.join(results_dir, "dot")
    png_dir = os.path.join(results_dir, "png")
    os.makedirs(dot_dir, exist_ok=True)
    os.makedirs(png_dir, exist_ok=True)

    # --- Step 2: Paths ---
    tracer_log_path = os.path.join(results_dir, "tracer.log")
    metadata_path = os.path.join(results_dir, "metadata.json")

    # --- Step 3: Environment variables ---
    env = os.environ.copy()
    env["GST_DEBUG"] = "*:7,GST_TRACER:7"
    env["GST_DEBUG_DUMP_DOT_DIR"] = dot_dir
    env["GST_DEBUG_FILE"] = tracer_log_path
    env["GST_TRACERS"] = "stats"

    # --- Step 4: Run pipeline ---
    print("▶️ Starting pipeline... (press 'q' + Enter to stop)")
    proc = subprocess.Popen(
        ["python", "undistortedLiveGstreamer.py"],
        env=env,
    )

    try:
        while True:
            user_input = input()
            if user_input.strip().lower() == "q":
                print("🛑 Stopping pipeline...")
                proc.terminate()
                proc.wait()
                break
    except KeyboardInterrupt:
        print("⚠️ KeyboardInterrupt: stopping pipeline...")
        proc.terminate()
        proc.wait()

    # --- Step 5: Convert DOT -> PNG ---
    dot_files = glob.glob(os.path.join(dot_dir, "*.dot"))
    for dot_file in dot_files:
        png_file = os.path.join(
            png_dir, os.path.splitext(os.path.basename(dot_file))[0] + ".png"
        )
        try:
            subprocess.run(
                ["dot", "-Tpng", dot_file, "-o", png_file],
                check=True
            )
        except FileNotFoundError:
            print("❌ 'dot' command not found. Please install Graphviz (apt install graphviz).")
            break

    # --- Step 6: Generate metadata.json from DOT ---
    metadata = {}
    for dot_file in dot_files:
        name = os.path.splitext(os.path.basename(dot_file))[0]
        try:
            metadata[name] = parse_pipeline(dot_file)
        except Exception as e:
            metadata[name] = {"error": str(e)}

    with open(metadata_path, "w") as f:
        json.dump(metadata, f, indent=2)

    parse_and_save_latency(results_dir)

    print(f"\n✅ Results stored in: {results_dir}")
    print(f"   - DOT files: {dot_dir}")
    print(f"   - PNG files: {png_dir}")
    print(f"   - Tracer log: {tracer_log_path}")
    print(f"   - Metadata: {metadata_path}")
    print(f"   - Created latency files")

if __name__ == "__main__":
    main()
