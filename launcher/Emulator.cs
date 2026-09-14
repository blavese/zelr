using System.Diagnostics;
using System.IO.Compression;
using System.IO;
using System.Reflection;
using Microsoft.Win32;

namespace ZelrLauncher;

/// <summary>
/// Finds QEMU, unpacks the kernel, and starts the machine. Kept away from the
/// window so the UI has nothing to do but call three methods.
/// </summary>
public static class Emulator
{
    private static readonly string[] LikelyPaths =
    {
        @"C:\Program Files\qemu\qemu-system-x86_64.exe",
        @"C:\Program Files (x86)\qemu\qemu-system-x86_64.exe",
        @"C:\qemu\qemu-system-x86_64.exe",
    };

    /// <summary>Full path to qemu-system-x86_64.exe, or null if it is not installed.</summary>
    public static string? FindQemu()
    {
        foreach (var p in LikelyPaths)
            if (File.Exists(p)) return p;

        // installed somewhere unusual but on PATH
        var path = Environment.GetEnvironmentVariable("PATH") ?? "";
        foreach (var dir in path.Split(';', StringSplitOptions.RemoveEmptyEntries))
        {
            try
            {
                var candidate = Path.Combine(dir.Trim(), "qemu-system-x86_64.exe");
                if (File.Exists(candidate)) return candidate;
            }
            catch (ArgumentException) { /* malformed PATH entry */ }
        }

        // the installer records its location here
        try
        {
            using var key = Registry.LocalMachine.OpenSubKey(
                @"SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\QEMU");
            var loc = key?.GetValue("InstallLocation") as string;
            if (!string.IsNullOrEmpty(loc))
            {
                var candidate = Path.Combine(loc, "qemu-system-x86_64.exe");
                if (File.Exists(candidate)) return candidate;
            }
        }
        catch (Exception) { /* registry unavailable, fall through */ }

        return null;
    }

    /// <summary>Writes the embedded kernel next to the user's temp folder and returns its path.</summary>
    public static string ExtractKernel()
    {
        var dir = Path.Combine(Path.GetTempPath(), "zelr");
        Directory.CreateDirectory(dir);
        var dest = Path.Combine(dir, "zelr.bin");

        var asm = Assembly.GetExecutingAssembly();
        var name = asm.GetManifestResourceNames()
                      .FirstOrDefault(n => n.EndsWith("zelr.bin", StringComparison.OrdinalIgnoreCase))
                   ?? throw new FileNotFoundException("The kernel is missing from this build.");

        using var src = asm.GetManifestResourceStream(name)
                        ?? throw new FileNotFoundException("The kernel could not be read.");
        using var outFile = File.Create(dest);
        src.CopyTo(outFile);
        return dest;
    }

    /// <summary>Where the user's disk image lives, so files survive between runs.</summary>
    public static string DiskPath
    {
        get
        {
            var local = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
            var dir = Path.Combine(local, "zelr");
            var disk = Path.Combine(dir, "disk.img");

            // Anyone who ran this before the rename has their files in the
            // old folder. Moving the image is the difference between keeping
            // them and silently starting over with an empty disk.
            // Failing to move it is not worth refusing to start over: the
            // file can be locked by a machine still running, or sitting
            // somewhere this user cannot write. Falling through leaves the
            // old image untouched and makes a new one.
            var older = Path.Combine(local, "nyx", "disk.img");
            if (!File.Exists(disk) && File.Exists(older))
            {
                try
                {
                    Directory.CreateDirectory(dir);
                    File.Move(older, disk);
                    return disk;
                }
                catch (IOException) { }
                catch (UnauthorizedAccessException) { }
            }

            Directory.CreateDirectory(dir);
            return disk;
        }
    }

    private const long DiskBytes = 32L * 1024 * 1024;

    /// <summary>
    /// Creates the disk on first run. A prepared image is used if one was
    /// embedded, otherwise the disk is left blank and the kernel formats it
    /// on boot. The programs live in the kernel, so a blank disk is enough.
    /// An existing disk is never touched, so nothing the user saved is lost
    /// on an upgrade.
    /// </summary>
    public static void EnsureDisk()
    {
        var path = DiskPath;
        if (File.Exists(path) && new FileInfo(path).Length >= DiskBytes) return;

        var asm = Assembly.GetExecutingAssembly();
        var name = asm.GetManifestResourceNames()
                      .FirstOrDefault(n => n.EndsWith("starter.img.gz", StringComparison.OrdinalIgnoreCase));

        if (name is not null)
        {
            using var src = asm.GetManifestResourceStream(name)!;
            using var gz = new GZipStream(src, CompressionMode.Decompress);
            using var outFile = File.Create(path);
            gz.CopyTo(outFile);
            if (outFile.Length >= DiskBytes) return;
            outFile.SetLength(DiskBytes);
            return;
        }

        // no starter available: a blank disk, which the kernel formats itself
        using var blank = new FileStream(path, FileMode.Create, FileAccess.Write);
        blank.SetLength(DiskBytes);
    }

    /// <summary>Boots the kernel in its own window, with a disk and a network card.</summary>
    public static Process Start(string qemu, string kernel)
    {
        EnsureDisk();

        var psi = new ProcessStartInfo(qemu)
        {
            UseShellExecute = false,
            CreateNoWindow = true,
        };
        psi.ArgumentList.Add("-kernel");      psi.ArgumentList.Add(kernel);
        psi.ArgumentList.Add("-m");           psi.ArgumentList.Add("64");
        psi.ArgumentList.Add("-no-reboot");
        psi.ArgumentList.Add("-name");        psi.ArgumentList.Add("zelr");

        // a persistent disk, so anything saved is still there next time
        psi.ArgumentList.Add("-drive");
        psi.ArgumentList.Add($"file={DiskPath},format=raw,if=ide,index=0");

        // user mode networking: no admin rights, no bridge, no host exposure
        psi.ArgumentList.Add("-netdev"); psi.ArgumentList.Add("user,id=n0");
        psi.ArgumentList.Add("-device");  psi.ArgumentList.Add("rtl8139,netdev=n0");

        // no -serial stdio: that would pop a console window next to the machine
        return Process.Start(psi) ?? throw new InvalidOperationException("QEMU did not start.");
    }

    /// <summary>Runs the official winget package install, visibly, and waits.</summary>
    public static Process InstallQemu()
    {
        var psi = new ProcessStartInfo("winget")
        {
            UseShellExecute = true,     // show the console so the user sees what happens
        };
        psi.ArgumentList.Add("install");
        psi.ArgumentList.Add("--id");
        psi.ArgumentList.Add("SoftwareFreedomConservancy.QEMU");
        psi.ArgumentList.Add("--accept-source-agreements");
        psi.ArgumentList.Add("--accept-package-agreements");
        return Process.Start(psi) ?? throw new InvalidOperationException("winget did not start.");
    }
}
