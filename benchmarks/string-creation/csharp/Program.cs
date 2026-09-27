/** Measure decimal string creation in a managed ring buffer. */
using System;
using System.Diagnostics;
using System.Globalization;
using System.Runtime.CompilerServices;

/// <summary>Compare invariant decimal formatting using owned managed strings.</summary>
internal static class Program
{
    private static readonly string[] Buffer = new string[1024];

    /// <summary>Replace ring slots with newly formatted decimal integers.</summary>
    [MethodImpl(MethodImplOptions.NoInlining)]
    private static void FromInt(long n)
    {
        for (long i = 0; i < n; ++i)
            Buffer[(int)(i & 1023)] = i.ToString(CultureInfo.InvariantCulture);
    }

    /// <summary>Validate every retained value and return the total string length.</summary>
    private static int Verify(long n)
    {
        int total = 0;
        for (int slot = 0; slot < Buffer.Length; ++slot)
        {
            long expected = n - 1 - ((n - 1 - slot) & 1023);
            if (Buffer[slot] != expected.ToString(CultureInfo.InvariantCulture))
                throw new InvalidOperationException("Incorrect retained decimal string");
            total += Buffer[slot].Length;
        }
        return total;
    }

    /// <summary>Warm up the runtime, time conversions, and report checked results.</summary>
    private static void Main()
    {
        const long n = 100_000_000;
        FromInt(1_000_000);
        Verify(1_000_000);
        double best = double.PositiveInfinity;
        for (int run = 0; run < 5; ++run)
        {
            long start = Stopwatch.GetTimestamp();
            FromInt(n);
            double elapsed = (Stopwatch.GetTimestamp() - start) * (1e9 / Stopwatch.Frequency);
            best = Math.Min(best, elapsed);
            Verify(n);
        }
        double ns = best / n;
        Console.WriteLine($"C# (.NET {Environment.Version})");
        Console.WriteLine(FormattableString.Invariant(
            $"i.ToString() {ns:F2} ns/string  {1e3 / ns:F1} M/s   (check {Verify(n)})"));
    }
}
