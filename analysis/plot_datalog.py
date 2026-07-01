#!/usr/bin/env python3
"""
Plot the ESP32-BME690 sensor node's datalog.csv.

The firmware logs one averaged row per minute with this schema:

    boot_id,uptime_s,epoch,synced,temp_c,hum_pct,press_hpa,iaq,iaq_acc,
    co2_ppm,voc_ppm,lux,sound_db,sound_avg,sound_peak,batt_v,batt_pct

Timestamps: `epoch` is UTC seconds and is only trustworthy where `synced==1`
(NTP was anchored that power session). This script reconstructs absolute time
for the *whole* of any power session that synced NTP at any point, using
    real_epoch = boot_epoch + uptime_s,   boot_epoch = epoch - uptime_s (synced row)
Rows from a session that never synced NTP have no absolute time and are dropped
(with a warning) — they're still correctly ordered by boot_id + uptime if you
need them.

Air-quality channels (IAQ / CO2 / VOC) are masked out where the BSEC accuracy
(`iaq_acc`) is below --min-accuracy, so the warm-up period doesn't draw
misleading flat lines.

Usage:
    pip install pandas matplotlib
    python plot_datalog.py datalog.csv                 # plot a local file
    python plot_datalog.py --ip 192.168.0.65           # fetch from the board
    python plot_datalog.py datalog.csv -o trends.png    # save instead of show
    python plot_datalog.py datalog.csv --min-accuracy 3 # stricter AQ filter
"""

import argparse
import datetime as dt
import io
import sys
import urllib.request

import pandas as pd
import matplotlib
import matplotlib.pyplot as plt
import matplotlib.dates as mdates

COLUMNS = [
    "boot_id", "uptime_s", "epoch", "synced", "temp_c", "hum_pct", "press_hpa",
    "iaq", "iaq_acc", "co2_ppm", "voc_ppm", "lux", "sound_db", "sound_avg",
    "sound_peak", "batt_v", "batt_pct",
]


def load_csv(source: str, is_url: bool) -> pd.DataFrame:
    """Read the log from a file path or from http://<ip>/datalog.csv."""
    if is_url:
        url = source if source.startswith("http") else f"http://{source}/datalog.csv"
        print(f"[info] fetching {url}")
        with urllib.request.urlopen(url, timeout=15) as r:
            text = r.read().decode("utf-8", "replace")
        df = pd.read_csv(io.StringIO(text))
    else:
        df = pd.read_csv(source)

    missing = [c for c in COLUMNS if c not in df.columns]
    if missing:
        sys.exit(f"[error] CSV is missing expected columns: {missing}")
    # Coerce everything numeric; drop rows that are all-NaN / malformed.
    for c in COLUMNS:
        df[c] = pd.to_numeric(df[c], errors="coerce")
    df = df.dropna(subset=["boot_id", "uptime_s"]).reset_index(drop=True)
    return df


def reconstruct_time(df: pd.DataFrame) -> pd.DataFrame:
    """Fill absolute UTC epoch for every row whose power session synced NTP."""
    df["epoch_utc"] = pd.NA
    anchored, orphan_boots = 0, []
    for boot_id, grp in df.groupby("boot_id"):
        synced = grp[(grp["synced"] == 1) & (grp["epoch"] > 0)]
        mask = df["boot_id"] == boot_id
        if len(synced):
            # boot_epoch = wall-clock time at uptime 0 (median guards outliers).
            boot_epoch = float((synced["epoch"] - synced["uptime_s"]).median())
            df.loc[mask, "epoch_utc"] = boot_epoch + df.loc[mask, "uptime_s"]
            anchored += int(mask.sum())
        else:
            orphan_boots.append(int(boot_id))

    if orphan_boots:
        dropped = int((~df["boot_id"].isin(
            df.loc[df["epoch_utc"].notna(), "boot_id"])).sum())
        print(f"[warn] {dropped} row(s) from boot session(s) {orphan_boots} never "
              f"synced NTP -> no absolute time, dropped from the time plots.")

    df = df[df["epoch_utc"].notna()].copy()
    if df.empty:
        sys.exit("[error] No rows have a reconstructable timestamp "
                 "(no power session ever synced NTP).")

    # UTC epoch -> local wall-clock (naive) for readable axis labels.
    local_tz = dt.datetime.now().astimezone().tzinfo
    t = pd.to_datetime(df["epoch_utc"].astype(float), unit="s", utc=True)
    df["time"] = t.dt.tz_convert(local_tz).dt.tz_localize(None)
    return df.sort_values("time").reset_index(drop=True)


def mask_air_quality(df: pd.DataFrame, min_acc: int) -> pd.DataFrame:
    """Blank IAQ/CO2/VOC where BSEC accuracy is below the threshold."""
    ok = df["iaq_acc"] >= min_acc
    for col in ("iaq", "co2_ppm", "voc_ppm"):
        df[col + "_f"] = df[col].where(ok)
    n_ok, n = int(ok.sum()), len(df)
    print(f"[info] air-quality: {n_ok}/{n} rows at accuracy >= {min_acc} "
          f"({100*n_ok/max(n,1):.0f}%).")
    return df


def twin(ax, x, y1, l1, c1, y2, l2, c2, ylab1, ylab2):
    """Draw two series on shared-x twin axes and build a combined legend."""
    ax.plot(x, y1, color=c1, lw=1.2, label=l1)
    ax.set_ylabel(ylab1, color=c1)
    ax.tick_params(axis="y", labelcolor=c1)
    ax2 = ax.twinx()
    ax2.plot(x, y2, color=c2, lw=1.2, label=l2)
    ax2.set_ylabel(ylab2, color=c2)
    ax2.tick_params(axis="y", labelcolor=c2)
    lines = ax.get_lines() + ax2.get_lines()
    ax.legend(lines, [ln.get_label() for ln in lines], loc="upper left", fontsize=8)
    ax.grid(True, alpha=0.25)


def plot(df: pd.DataFrame, title: str, out: str | None):
    x = df["time"]
    fig, axes = plt.subplots(6, 1, figsize=(12, 15), sharex=True)
    fig.suptitle(title, fontsize=14, y=0.995)

    twin(axes[0], x, df["temp_c"], "Temp", "#d9480f",
         df["hum_pct"], "Humidity", "#1c7ed6", "Temp (°C)", "Humidity (%)")

    axes[1].plot(x, df["press_hpa"], color="#7048e8", lw=1.2)
    axes[1].set_ylabel("Pressure (hPa)")
    axes[1].grid(True, alpha=0.25)

    axes[2].plot(x, df["iaq_f"], color="#2f9e44", lw=1.2)
    axes[2].set_ylabel("IAQ (accuracy-filtered)")
    axes[2].grid(True, alpha=0.25)

    twin(axes[3], x, df["co2_ppm_f"], "CO2 eq", "#e8590c",
         df["voc_ppm_f"], "VOC eq", "#0c8599", "CO₂ eq (ppm)", "VOC eq (ppm)")

    twin(axes[4], x, df["lux"], "Light", "#f08c00",
         df["sound_db"], "Sound", "#5f3dc4", "Light (lux)", "Sound (dB)")

    twin(axes[5], x, df["batt_v"], "Voltage", "#2b8a3e",
         df["batt_pct"], "Charge", "#c2255c", "Battery (V)", "Battery (%)")

    axes[-1].xaxis.set_major_formatter(mdates.DateFormatter("%m-%d %H:%M"))
    fig.autofmt_xdate()
    fig.tight_layout(rect=(0, 0, 1, 0.99))

    if out:
        fig.savefig(out, dpi=120)
        print(f"[info] wrote {out}")
    else:
        plt.show()


def main():
    p = argparse.ArgumentParser(description="Plot ESP32-BME690 datalog.csv")
    p.add_argument("csv", nargs="?", help="path to datalog.csv")
    p.add_argument("--ip", help="fetch from http://<ip>/datalog.csv instead of a file")
    p.add_argument("-o", "--out", help="save PNG to this path instead of showing a window")
    p.add_argument("--min-accuracy", type=int, default=1,
                   help="hide IAQ/CO2/VOC below this BSEC accuracy (0-3, default 1; use 3 for strict)")
    args = p.parse_args()

    if not args.csv and not args.ip:
        p.error("give a CSV path or --ip")
    if args.out:
        matplotlib.use("Agg")  # headless save, no display needed

    source = args.ip if args.ip else args.csv
    df = load_csv(source, is_url=bool(args.ip))
    print(f"[info] loaded {len(df)} rows across {df['boot_id'].nunique()} boot session(s).")
    df = reconstruct_time(df)
    df = mask_air_quality(df, args.min_accuracy)
    span = df["time"].iloc[-1] - df["time"].iloc[0]
    print(f"[info] {len(df)} plottable rows, {df['time'].iloc[0]} -> "
          f"{df['time'].iloc[-1]} (span {span}).")

    title = f"ESP32-BME690 sensor log  ({len(df)} pts, {df['time'].iloc[0]:%Y-%m-%d} -> {df['time'].iloc[-1]:%Y-%m-%d})"
    plot(df, title, args.out)


if __name__ == "__main__":
    main()
