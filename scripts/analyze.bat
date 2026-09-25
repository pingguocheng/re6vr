@echo off
REM Build (or rebuild) the static analysis cache for BH6.exe.
REM
REM The cache is a JSON dump of every function the recursive-descent analyser reached, plus
REM every cross reference it found. re6dis.py reads it for the `func` and `stats` commands;
REM `disasm`, `xref` and `str` work against the exe directly and need no cache.
REM
REM Takes a couple of minutes on a 17 MB .text. Run it once after a decoder change.
setlocal
cd /d "%~dp0"
python -m disasm_lib.analyze -o "%~dp0..\_work\bh6_analysis.json" %*
endlocal
