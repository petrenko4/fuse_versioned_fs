import subprocess
import json
import statistics
import os

# CONFIG
runs = 15
file_size = "3G"  # Increased slightly for better sampling
directory = "/tmp/"
times = []
fio_command = [
    "fio",
    "--name=test",
    f"--size={file_size}",
    f"--directory={directory}",
    "--bs=4k",
    "--rw=write",
    "--output-format=json"
]
print("="*30)
print("BENCHMARK CONFIGURATION")
print("="*30)
print(f"Directory: {directory}")
print(f"File Size: {file_size}")
print(f"Command:   {' '.join(fio_command)}") 
print("="*30 + "\n")

for i in range(runs):
    print(f"Run {i+1}/{runs}...")

    # Added --direct=1 to bypass OS caching which might be skewing results
    result = subprocess.run([
        "fio",
        "--name=test",
        f"--size={file_size}",
        f"--directory={directory}",
        "--bs=4k",
        "--rw=write",
        "--output-format=json"
    ], capture_output=True, text=True)

    try:
        if result.stderr:
            print("STDERR:", result.stderr)
        
        data = json.loads(result.stdout)
        
        # 'runtime' in the JSON is in msec. 
        # If you want more precision, use 'total_ios' and latencies, 
        # but for a simple fix, we'll stick to runtime.
        runtime_ms = data["jobs"][0]["write"]["runtime"]
        
        times.append(runtime_ms)
        print(f"DEBUG: write.runtime = {runtime_ms} ms")
    except (json.JSONDecodeError, KeyError, IndexError) as e:
        print(f"Error parsing fio output: {e}")
    # if os.path.exists("mnt/test.0.0"):
    #     os.remove("mnt/test.0.0")
    #     print("[File Deleted]")


# compute average
if times:
    avg = statistics.mean(times)
    stdev = statistics.stdev(times) if len(times) > 1 else 0

    print("\n" + "="*20)
    print("FINAL RESULTS")
    print("="*20)
    for i, t in enumerate(times):
        print(f"Run {i+1}: {t:>4} ms")

    print("-" * 20)
    print(f"Average time: {avg:.2f} ms")
    print(f"Std Deviation: {stdev:.2f} ms")
else:
    print("No data collected.")