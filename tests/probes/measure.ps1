#requires -Version 7.0
[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string]$ProbeDirectory,

    [switch]$IncludeEngineProbes,

    [switch]$Benchmark,

    [ValidateRange(1, 30)]
    [int]$DurationSeconds = 3,

    [string]$OutputPath
)

$ErrorActionPreference = 'Stop'
$probeDirectory = (Resolve-Path -LiteralPath $ProbeDirectory).Path

if (-not ('AIMouseProbeMatrix' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Threading;

public static class AIMouseProbeMatrix
{
    [StructLayout(LayoutKind.Sequential)]
    public struct Point { public int X; public int Y; }

    [StructLayout(LayoutKind.Sequential)]
    private struct GuiThreadInfo {
        public uint Size, Flags;
        public IntPtr Active, Focus, Capture, MenuOwner, MoveSize, Caret;
        public int CaretLeft, CaretTop, CaretRight, CaretBottom;
    }

    public sealed class BenchmarkResult {
        public int sent { get; set; }
        public double elapsed_ms { get; set; }
        public double achieved_hz { get; set; }
        public double send_p50_us { get; set; }
        public double send_p95_us { get; set; }
        public double send_p99_us { get; set; }
        public double send_max_us { get; set; }
        public double scheduled_start_late_p99_us { get; set; }
    }

    [DllImport("user32.dll")]
    public static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint processId);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern bool ClientToScreen(IntPtr hwnd, ref Point point);

    [DllImport("user32.dll")]
    private static extern bool GetGUIThreadInfo(uint thread, ref GuiThreadInfo info);

    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern IntPtr SendMessageTimeout(
        IntPtr hwnd, uint message, UIntPtr wParam, IntPtr lParam,
        uint flags, uint timeout, out UIntPtr result);

    public static long Coordinates(int x, int y) {
        return (ushort)x | ((long)(ushort)y << 16);
    }

    public static long ScreenCoordinates(IntPtr hwnd, int x, int y) {
        var point = new Point { X = x, Y = y };
        if (!ClientToScreen(hwnd, ref point)) throw new InvalidOperationException("ClientToScreen failed.");
        return Coordinates(point.X, point.Y);
    }

    public static string Capture(uint thread) {
        var info = new GuiThreadInfo { Size = (uint)Marshal.SizeOf<GuiThreadInfo>() };
        return GetGUIThreadInfo(thread, ref info) ? "0x" + info.Capture.ToInt64().ToString("x") : null;
    }

    public static double Send(IntPtr hwnd, uint message, ulong wParam, long lParam) {
        var start = Stopwatch.GetTimestamp();
        if (SendMessageTimeout(hwnd, message, new UIntPtr(wParam), new IntPtr(lParam),
                               0x23, 250, out _) == IntPtr.Zero) {
            throw new InvalidOperationException("SendMessageTimeout failed for message " + message +
                                                " with error " + Marshal.GetLastWin32Error());
        }
        return (Stopwatch.GetTimestamp() - start) * 1000000.0 / Stopwatch.Frequency;
    }

    private static double Percentile(double[] sorted, double proportion) {
        return sorted[(int)Math.Ceiling(sorted.Length * proportion) - 1];
    }

    public static BenchmarkResult Benchmark(IntPtr hwnd, int rate, int seconds) {
        const uint WM_MOUSEMOVE = 0x0200;
        const double Microseconds = 1000000.0;
        int count = rate * seconds;
        var durations = new double[count];
        var startLatency = new double[count];
        long frequency = Stopwatch.Frequency;
        long origin = Stopwatch.GetTimestamp();
        long deadline = origin;
        for (int i = 0; i < count; ++i) {
            deadline += ((i + 1) * frequency / rate) - (i * frequency / rate);
            long now;
            while ((now = Stopwatch.GetTimestamp()) < deadline) Thread.SpinWait(16);
            startLatency[i] = (now - deadline) * Microseconds / frequency;
            durations[i] = Send(hwnd, WM_MOUSEMOVE, 0, Coordinates(40 + i % 300, 100 + i % 120));
            // Preserve spacing after a scheduling stall instead of catching up with a burst.
            deadline = now;
        }
        double elapsed = (Stopwatch.GetTimestamp() - origin) * 1000.0 / frequency;
        Array.Sort(durations);
        Array.Sort(startLatency);
        return new BenchmarkResult {
            sent = count,
            elapsed_ms = elapsed,
            achieved_hz = count * 1000.0 / elapsed,
            send_p50_us = Percentile(durations, 0.50),
            send_p95_us = Percentile(durations, 0.95),
            send_p99_us = Percentile(durations, 0.99),
            send_max_us = durations[count - 1],
            scheduled_start_late_p99_us = Percentile(startLatency, 0.99),
        };
    }
}
'@
}

function Invoke-Probe {
    param([hashtable]$Definition)

    $exe = Join-Path $probeDirectory $Definition.Exe
    $info = [Diagnostics.ProcessStartInfo]::new($exe)
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    foreach ($arg in $Definition.Args) { $info.ArgumentList.Add($arg) }
    $info.ArgumentList.Add('--timeout-ms')
    $info.ArgumentList.Add($(if ($Definition.Rate) { [string](1000 * $DurationSeconds + 4000) } else { '1500' }))
    $process = [Diagnostics.Process]::Start($info)
    try {
        $stderr = $process.StandardError.ReadToEndAsync()
        $initial = $process.StandardOutput.ReadLineAsync()
        if (-not $initial.Wait(8000)) { throw "$($Definition.Label) failed to report readiness" }
        if (-not $initial.Result) { throw "$($Definition.Label) exited early. $($stderr.Result)" }
        $ready = $initial.Result | ConvertFrom-Json
        if ($ready.event -ne 'ready') { throw "Unexpected first event: $($initial.Result)" }
        $remaining = $process.StandardOutput.ReadToEndAsync()
        $hwnd = [IntPtr][Convert]::ToInt64($ready.hwnd, 16)
        [uint32]$ownerId = 0
        $ownerThread = [AIMouseProbeMatrix]::GetWindowThreadProcessId($hwnd, [ref]$ownerId)
        if ($ownerThread -eq 0 -or $ownerId -ne $process.Id) {
            throw "Probe window does not belong to child process $($process.Id)"
        }
        if ($ready.hwnd -eq $ready.foreground_hwnd) { throw 'The probe acquired foreground unexpectedly' }

        if ($Definition.Rate) {
            $rateResult = [AIMouseProbeMatrix]::Benchmark($hwnd, $Definition.Rate, $DurationSeconds)
            $process.Refresh()
            $processCpu = $process.TotalProcessorTime.TotalMilliseconds
            $processWorkingSet = $process.WorkingSet64
        } else {
            $start = [AIMouseProbeMatrix]::Coordinates(100, 80)
            $end = [AIMouseProbeMatrix]::Coordinates(170, 130)
            $screen = [AIMouseProbeMatrix]::ScreenCoordinates($hwnd, 170, 130)
            $sends = @(
                [AIMouseProbeMatrix]::Send($hwnd, 0x0200, 0, $start)  # WM_MOUSEMOVE
                [AIMouseProbeMatrix]::Send($hwnd, 0x0201, 1, $start)  # WM_LBUTTONDOWN
            )
            $captureAfterDown = [AIMouseProbeMatrix]::Capture($ownerThread)
            $sends += @(
                [AIMouseProbeMatrix]::Send($hwnd, 0x0200, 1, $end)    # WM_MOUSEMOVE / MK_LBUTTON
                [AIMouseProbeMatrix]::Send($hwnd, 0x0202, 0, $end)    # WM_LBUTTONUP
                [AIMouseProbeMatrix]::Send($hwnd, 0x020a, (120 -shl 16), $screen) # WM_MOUSEWHEEL
                [AIMouseProbeMatrix]::Send($hwnd, 0x0100, 0x57, 0x00110001) # WM_KEYDOWN
                [AIMouseProbeMatrix]::Send($hwnd, 0x0101, 0x57, 0xc0110001L) # WM_KEYUP
            )
            $captureAfterUp = [AIMouseProbeMatrix]::Capture($ownerThread)
        }
        if (-not $process.WaitForExit(5000)) { throw "$($Definition.Label) failed to exit on time" }
        if ($process.ExitCode -ne 0) {
            throw "$($Definition.Label) exit code $($process.ExitCode). $($stderr.Result)"
        }
        $events = @($remaining.Result -split '\r?\n' | Where-Object { $_ } | ConvertFrom-Json)
        $final = $events[-1]
        if ($final.event -ne 'final') { throw "$($Definition.Label) failed to report final counters" }
        $windowSamples = @($ready) + @($events | Where-Object { $_.foreground_hwnd })
        $foregroundChanges = @($windowSamples | Where-Object { $_.foreground_hwnd -ne $ready.foreground_hwnd })
        $cursorChanges = @($windowSamples | Where-Object {
            -not $_.cursor_ok -or $_.cursor_x -ne $ready.cursor_x -or $_.cursor_y -ne $ready.cursor_y
        })
        $case = [ordered]@{
            case = $Definition.Label
            args = $Definition.Args
            receiver = $Definition.Exe
            receiver_sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $exe).Hash
            foreground_stable_at_samples = $foregroundChanges.Count -eq 0
            cursor_stable_at_samples = $cursorChanges.Count -eq 0
            ready = $ready
            samples = $events
        }
        if ($Definition.Rate) {
            $case.requested_rate_hz = $Definition.Rate
            $case.benchmark = $rateResult
            $case.receiver_count = $final.moves
            $case.receiver_cpu_ms_from_start = $processCpu
            $case.receiver_working_set_bytes = $processWorkingSet
        } else {
            $case.send_duration_us = $sends
            $case.receiver_thread_capture_after_down = $captureAfterDown
            $case.receiver_thread_capture_after_up = $captureAfterUp
        }
        return $case
    } finally {
        if (-not $process.HasExited) { $process.Kill() }
        $process.Dispose()
    }
}

$native = 'aimouse_input_probe.exe'
$definitions = @(
    @{ Label = 'Win32 windowed'; Exe = $native; Args = @('win32') }
    @{ Label = 'Win32 borderless'; Exe = $native; Args = @('win32', '--borderless') }
    @{ Label = 'Raw Input foreground registration'; Exe = $native; Args = @('raw') }
    @{ Label = 'Raw Input sink'; Exe = $native; Args = @('raw', '--input-sink') }
    @{ Label = 'DirectInput foreground acquisition'; Exe = $native; Args = @('directinput') }
    @{ Label = 'DirectInput background nonexclusive'; Exe = $native; Args = @('directinput', '--background') }
)
if ($Benchmark) {
    $definitions = @(125, 500, 1000 | ForEach-Object {
        @{ Label = "Win32 $_ Hz"; Exe = $native; Args = @('win32', '--quiet'); Rate = $_ }
    })
} elseif ($IncludeEngineProbes) {
    $definitions += @(
        @{ Label = 'SDL 3 Direct3D 11 windowed'; Exe = 'aimouse_sdl_probe.exe'; Args = @() }
        @{ Label = 'SDL 3 Direct3D 11 borderless'; Exe = 'aimouse_sdl_probe.exe'; Args = @('--borderless') }
        @{ Label = 'GLFW OpenGL windowed'; Exe = 'aimouse_glfw_probe.exe'; Args = @() }
        @{ Label = 'GLFW OpenGL borderless'; Exe = 'aimouse_glfw_probe.exe'; Args = @('--borderless') }
    )
}

$report = [ordered]@{
    schema_version = 1
    timestamp = [DateTimeOffset]::Now.ToString('o')
    windows_version = [Environment]::OSVersion.VersionString
    scope = 'Synchronous window messages addressed only to newly launched background receiver processes.'
    cases = @($definitions | ForEach-Object { Invoke-Probe -Definition $_ })
}
$json = $report | ConvertTo-Json -Depth 8
if ($OutputPath) {
    Set-Content -LiteralPath $OutputPath -Value $json -Encoding utf8NoBOM
}
$json
