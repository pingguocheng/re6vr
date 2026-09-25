#!/usr/bin/env python3
"""Resolve and download build dependencies for the RE6 VR mod.

Uses urllib directly (PowerShell/curl TLS is broken in this environment).
Prints the asset names/URLs it finds so the caller can pick the right one.
"""
import json
import os
import sys
import urllib.request

UA = {"User-Agent": "re6vr-dep-fetcher/1.0"}
TP = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "_third_party")


def get_json(url):
    req = urllib.request.Request(url, headers=UA)
    with urllib.request.urlopen(req, timeout=30) as r:
        return json.loads(r.read().decode("utf-8"))


def release_assets(repo, tag=None):
    url = f"https://api.github.com/repos/{repo}/releases/latest" if not tag \
        else f"https://api.github.com/repos/{repo}/releases/tags/{tag}"
    data = get_json(url)
    print(f"== {repo} tag={data.get('tag_name')} name={data.get('name')}")
    out = []
    for a in data.get("assets", []):
        print(f"   asset: {a['name']}  ({a['size']} bytes)")
        out.append((a["name"], a["browser_download_url"]))
    return data.get("tag_name"), out


def download(url, dest):
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    req = urllib.request.Request(url, headers=UA)
    with urllib.request.urlopen(req, timeout=120) as r, open(dest, "wb") as f:
        while True:
            chunk = r.read(1 << 16)
            if not chunk:
                break
            f.write(chunk)
    print(f"downloaded -> {dest} ({os.path.getsize(dest)} bytes)")


if __name__ == "__main__":
    os.makedirs(TP, exist_ok=True)
    print("OpenXR-SDK:")
    try:
        release_assets("KhronosGroup/OpenXR-SDK")
    except Exception as e:
        print("  FAILED:", e)
    print("OpenXR-SDK-Source:")
    try:
        release_assets("KhronosGroup/OpenXR-SDK-Source")
    except Exception as e:
        print("  FAILED:", e)
    print("MinHook:")
    try:
        release_assets("TsudaKageyu/minhook")
    except Exception as e:
        print("  FAILED:", e)
