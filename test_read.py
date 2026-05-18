import subprocess
import json
import statistics

NUM_RUNS = 35

fio_command = [
    "fio",
    "--name=seqread",
    "--bs=4k",
    "--rw=read",
    "--filename=passthrough_mnt/tmp/fbt.0.0",
    "--output-format=json"
]

times_ms = []

for i in range(NUM_RUNS):
    result = subprocess.run(
        fio_command,
        capture_output=True,
        text=True
    )

    data = json.loads(result.stdout)
    job = data["jobs"][0]

    runtime_ms = job["read"]["runtime"]  # already in milliseconds
    times_ms.append(runtime_ms)

    print(f"Run {i + 1}: {runtime_ms:.3f} ms")

avg = statistics.mean(times_ms)

print("\n====================")
print(f"Average fio runtime: {avg:.3f} ms")
print("====================")