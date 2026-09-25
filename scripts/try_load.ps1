param(
    [Parameter(Mandatory = $true)][string]$Path,
    [string]$TraceFile = ""
)
# try_load.ps1 - loads a DLL with LoadLibrary and reports the exact failure.
# An access violation surfaces as a .NET TypeInitializationException wrapping
# SEHException, so the catch block has to look at the inner exception chain.
$sig = @'
using System;
using System.Runtime.InteropServices;
public static class NativeLoader {
    [DllImport("kernel32", SetLastError=true, CharSet=CharSet.Unicode)]
    public static extern IntPtr LoadLibraryW(string path);
    [DllImport("kernel32", SetLastError=true)]
    public static extern IntPtr GetProcAddress(IntPtr mod, string name);
}
'@
Add-Type -TypeDefinition $sig -ErrorAction Stop

try {
    $h = [NativeLoader]::LoadLibraryW($Path)
    if ($h -eq [IntPtr]::Zero) {
        $err = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
        Write-Host "LoadLibrary FAILED, Win32 error $err"
        exit 1
    }
    Write-Host "LoadLibrary OK, handle = $h"
    $p = [NativeLoader]::GetProcAddress($h, "Direct3DCreate9")
    Write-Host "Direct3DCreate9 = $p"
    exit 0
} catch {
    $ex = $_.Exception
    $depth = 0
    while ($ex -and $depth -lt 6) {
        Write-Host ("  [{0}] {1}: {2}" -f $depth, $ex.GetType().FullName, $ex.Message)
        $ex = $ex.InnerException
        $depth++
    }
    Write-Host "LoadLibrary CRASHED (SEH inside DllMain/static init)"
    exit 2
}
