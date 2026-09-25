import json
import subprocess
import sys
import time

GAME = r"C:\Program Files (x86)\Steam\steamapps\common\Resident Evil 6"
STEAM = r"C:\rjrj\steam\steam.exe"
APPID = "221040"


def bh6_pids():
    out = subprocess.run(
        ["tasklist", "/FI", "IMAGENAME eq BH6.exe", "/FO", "CSV", "/NH"],
        capture_output=True, text=True).stdout
    return [line.split('","')[0].strip('"') for line in out.splitlines() if line.startswith('"BH6')]


def launch_and_watch(seconds=45):
    """Launch through Steam, return (lifetime_seconds, still_running)."""
    subprocess.Popen([STEAM, "-applaunch", APPID])
    start = time.time()
    seen = False
    last_seen = None
    while time.time() - start < seconds:
        pids = bh6_pids()
        if pids:
            seen = True
            last_seen = time.time()
        elif seen:
            return last_seen - start, False
        time.sleep(0.5)
    return (last_seen - start if last_seen else 0.0), bool(bh6_pids())


if __name__ == "__main__":
    label = sys.argv[1] if len(sys.argv) > 1 else "run"
    alive, running = launch_and_watch(int(sys.argv[2]) if len(sys.argv) > 2 else 45)
    print(f"[{label}] BH6 alive for ~{alive:.1f}s, still running at timeout: {running}")
