"""Check the final binary too: nested core rebuilds can mask checkprogsize errors."""
import csv
from pathlib import Path

Import("env")


def check_image_size(source, target, env):
    partition_file = Path(env.subst("$PROJECT_DIR")) / env.BoardConfig().get("build.partitions", "partitions.csv")
    limits = []
    for row in csv.reader(partition_file.read_text().splitlines()):
        if len(row) >= 5 and not row[0].strip().startswith("#") and row[1].strip() == "app":
            value = row[4].strip()
            limits.append(int(value, 0))
    if not limits:
        raise RuntimeError("No application partition found; cannot validate final firmware size")
    binary = Path(env.subst("$BUILD_DIR/${PROGNAME}.bin"))
    size = binary.stat().st_size
    limit = min(limits)
    if size > limit:
        raise RuntimeError(f"Final firmware {size} bytes exceeds OTA partition {limit} bytes")
    print(f"Final OTA binary verified: {size}/{limit} bytes")


env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", check_image_size)
