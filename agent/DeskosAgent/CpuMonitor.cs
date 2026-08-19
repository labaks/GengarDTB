using System.Runtime.InteropServices;
using System.Runtime.InteropServices.ComTypes;

namespace DeskosAgent;

/// <summary>
/// CPU load only, for the "cpu" topic. Deliberately not
/// System.Diagnostics.PerformanceCounter: it depends on the Windows perf
/// counter registry data (PerfLib), which is a well-known source of
/// "Cannot load Counter Name data... may need to be repaired" failures on
/// machines where that data is corrupted or was never rebuilt — hit exactly
/// that on the dev machine this was written on. GetSystemTimes is a bare
/// kernel32 call with no such dependency.
///
/// Temperature is deliberately left out of v1: the reliable way to get it on
/// Windows is LibreHardwareMonitorLib, which needs a kernel driver and admin
/// rights to load it — more than a background tray app should demand for a
/// first cut. The wire format (host-protocol.md) already has room for
/// "temp_c": null | number, so adding it later is additive, not a protocol
/// change.
/// </summary>
public sealed class CpuMonitor
{
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool GetSystemTimes(out FILETIME idleTime, out FILETIME kernelTime, out FILETIME userTime);

    private long _lastIdle, _lastKernel, _lastUser;

    public CpuMonitor()
    {
        (_lastIdle, _lastKernel, _lastUser) = ReadTimes();
    }

    public int GetPercent()
    {
        var (idle, kernel, user) = ReadTimes();

        var idleDelta = idle - _lastIdle;
        // kernelTime includes idle time on Windows (documented GetSystemTimes
        // behaviour), so kernel + user is the correct total, not kernel*2.
        var totalDelta = (kernel - _lastKernel) + (user - _lastUser);
        _lastIdle = idle;
        _lastKernel = kernel;
        _lastUser = user;

        if (totalDelta <= 0)
        {
            return 0;
        }
        var busyFraction = 1.0 - (double)idleDelta / totalDelta;
        return (int)Math.Clamp(Math.Round(busyFraction * 100), 0, 100);
    }

    private static (long idle, long kernel, long user) ReadTimes()
    {
        GetSystemTimes(out var idle, out var kernel, out var user);
        return (ToLong(idle), ToLong(kernel), ToLong(user));
    }

    private static long ToLong(FILETIME ft) =>
        ((long)ft.dwHighDateTime << 32) | (uint)ft.dwLowDateTime;
}
