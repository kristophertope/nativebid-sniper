import csv
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
SIZE = "0x5000"


def read_env(path):
    values = {}
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            key, value = line.split("=", 1)
            value = value.strip()
            if len(value) >= 2 and value[0] == value[-1] and value[0] in "\"'":
                value = value[1:-1]
            values[key.strip()] = value
    return values


def main():
    if len(sys.argv) != 4:
        sys.exit("usage: make_settings.py <wifi name> <posix timezone> <output .bin>   (WIFI_PASS in the environment)")
    ssid, tz, out = sys.argv[1:]
    password = os.environ.get("WIFI_PASS", "")
    env = read_env(os.path.join(ROOT, ".env"))
    missing = [k for k in ("NB_USER", "NB_PASS", "TG_TOKEN", "TG_CHAT_ID") if not env.get(k)]
    if missing or not password:
        sys.exit("missing: " + ", ".join(missing + ([] if password else ["WIFI_PASS"])))
    rows = [
        ("sniper", "namespace", "", ""),
        ("wifi_ssid", "data", "string", ssid),
        ("wifi_pass", "data", "string", password),
        ("nb_user", "data", "string", env["NB_USER"]),
        ("nb_pass", "data", "string", env["NB_PASS"]),
        ("tg_token", "data", "string", env["TG_TOKEN"]),
        ("tg_chat", "data", "string", env["TG_CHAT_ID"]),
        ("tz", "data", "string", tz),
    ]
    state = os.path.join(ROOT, "snipes.json")
    if os.path.exists(state):
        rows.append(("state", "file", "binary", state))
    fd, path = tempfile.mkstemp(suffix=".csv")
    try:
        with os.fdopen(fd, "w", newline="") as f:
            writer = csv.writer(f)
            writer.writerow(("key", "type", "encoding", "value"))
            writer.writerows(rows)
        result = subprocess.run(
            [sys.executable, "-m", "esp_idf_nvs_partition_gen", "generate", path, out, SIZE],
            capture_output=True, text=True,
        )
        if result.returncode:
            sys.exit("settings image failed:\n" + result.stdout[-500:] + result.stderr[-500:])
    finally:
        os.remove(path)
    os.chmod(out, 0o600)
    names = [r[0] for r in rows[1:]]
    print(f"Wrote {out} with: {', '.join(names)}")


if __name__ == "__main__":
    main()
