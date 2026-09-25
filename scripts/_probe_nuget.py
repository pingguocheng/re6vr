import json
import urllib.request

UA = {"User-Agent": "re6vr-dep-fetcher/1.0"}
url = "https://azuresearch-usnc.nuget.org/query?q=openxr&prerelease=true&take=30"
d = json.loads(urllib.request.urlopen(urllib.request.Request(url, headers=UA), timeout=30).read())
for r in d["data"]:
    desc = (r.get("description") or "").replace("\n", " ")[:70]
    print("%-34s %-20s %s" % (r["id"], r["version"], desc))
