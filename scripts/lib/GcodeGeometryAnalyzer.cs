using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Security.Cryptography;
using System.Text;

namespace Magpie.Verification
{
    public sealed class GcodeGeometryResult
    {
        public string SegmentSha256 { get; set; }
        public string CoverageSha256 { get; set; }
        public string MetricsSha256 { get; set; }
        public int PositiveSegments { get; set; }
        public int CoverageCells { get; set; }
        public double TotalLength { get; set; }
        public double TotalExtrusion { get; set; }
        public int LayerCount { get; set; }
        public string ToolIds { get; set; }
        public double MinX { get; set; }
        public double MinY { get; set; }
        public double MaxX { get; set; }
        public double MaxY { get; set; }
        internal HashSet<ulong> Coverage { get; set; }

        public double CoverageDifferenceRatio(GcodeGeometryResult other)
        {
            if (Coverage == null || other == null || other.Coverage == null) return double.NaN;
            int intersection = 0;
            HashSet<ulong> smaller = Coverage.Count <= other.Coverage.Count ? Coverage : other.Coverage;
            HashSet<ulong> larger = ReferenceEquals(smaller, Coverage) ? other.Coverage : Coverage;
            foreach (ulong cell in smaller) if (larger.Contains(cell)) ++intersection;
            long union = (long)Coverage.Count + other.Coverage.Count - intersection;
            return union == 0 ? 0.0 : ((double)(union - intersection) / union);
        }
    }

    public static class GcodeGeometryAnalyzer
    {
        private static readonly CultureInfo Invariant = CultureInfo.InvariantCulture;

        public static GcodeGeometryResult Analyze(string path, double gridMm, bool includeCoverage)
        {
            var segments = new List<string>();
            var coverage = includeCoverage ? new HashSet<ulong>() : null;
            var lengths = new Dictionary<string, double>(StringComparer.Ordinal);
            var extrusions = new Dictionary<string, double>(StringComparer.Ordinal);
            double x = 0, y = 0, z = 0, e = 0;
            bool xyzAbsolute = true, eAbsolute = true;
            int tool = 0, layer = -1, positiveSegments = 0;
            var usedTools = new HashSet<int>();
            double minX = double.PositiveInfinity, minY = double.PositiveInfinity;
            double maxX = double.NegativeInfinity, maxY = double.NegativeInfinity;
            string role = "unknown";

            foreach (string raw in File.ReadLines(path))
            {
                string line = raw.Trim();
                if (line.StartsWith(";LAYER_CHANGE", StringComparison.OrdinalIgnoreCase))
                    ++layer;
                else if (line.StartsWith(";LAYER:", StringComparison.OrdinalIgnoreCase))
                    TryParseLeadingInt(line.Substring(7), ref layer);
                else if (line.StartsWith("; layer num/total_layer_count:", StringComparison.OrdinalIgnoreCase))
                    TryParseLeadingInt(line.Substring(31), ref layer);

                if (line.StartsWith(";TYPE:", StringComparison.OrdinalIgnoreCase))
                    role = line.Substring(6).Trim().ToLowerInvariant();
                else if (line.StartsWith(";FEATURE:", StringComparison.OrdinalIgnoreCase))
                    role = line.Substring(9).Trim().ToLowerInvariant();

                int comment = line.IndexOf(';');
                string command = (comment >= 0 ? line.Substring(0, comment) : line).Trim();
                if (command.Length == 0) continue;
                string opcode = GetOpcode(command);
                if (opcode == "G90") { xyzAbsolute = true; continue; }
                if (opcode == "G91") { xyzAbsolute = false; continue; }
                if (opcode == "M82") { eAbsolute = true; continue; }
                if (opcode == "M83") { eAbsolute = false; continue; }
                if (opcode.Length > 1 && opcode[0] == 'T')
                {
                    int parsedTool;
                    if (int.TryParse(opcode.Substring(1), NumberStyles.Integer, Invariant, out parsedTool)) tool = parsedTool;
                    continue;
                }
                if (opcode == "G92")
                {
                    double value;
                    if (TryGetParameter(command, 'X', out value)) x = value;
                    if (TryGetParameter(command, 'Y', out value)) y = value;
                    if (TryGetParameter(command, 'Z', out value)) z = value;
                    if (TryGetParameter(command, 'E', out value)) e = value;
                    continue;
                }
                if (opcode != "G0" && opcode != "G1" && opcode != "G00" && opcode != "G01") continue;

                double nx = x, ny = y, nz = z, ne = e, value2;
                if (TryGetParameter(command, 'X', out value2)) nx = xyzAbsolute ? value2 : x + value2;
                if (TryGetParameter(command, 'Y', out value2)) ny = xyzAbsolute ? value2 : y + value2;
                if (TryGetParameter(command, 'Z', out value2)) nz = xyzAbsolute ? value2 : z + value2;
                double deltaE = 0;
                if (TryGetParameter(command, 'E', out value2))
                {
                    if (eAbsolute) { ne = value2; deltaE = ne - e; }
                    else { deltaE = value2; ne = e + value2; }
                }

                double dx = nx - x, dy = ny - y;
                double length = Math.Sqrt(dx * dx + dy * dy);
                if (deltaE > 1e-8 && length > 1e-8)
                {
                    ++positiveSegments;
                    usedTools.Add(tool);
                    minX = Math.Min(minX, Math.Min(x, nx));
                    minY = Math.Min(minY, Math.Min(y, ny));
                    maxX = Math.Max(maxX, Math.Max(x, nx));
                    maxY = Math.Max(maxY, Math.Max(y, ny));
                    string group = string.Format(Invariant, "{0}|{1}|{2}|{3:0.0000}", layer, tool, role, nz);
                    string a = string.Format(Invariant, "{0:0.0000},{1:0.0000}", x, y);
                    string b = string.Format(Invariant, "{0:0.0000},{1:0.0000}", nx, ny);
                    if (StringComparer.Ordinal.Compare(a, b) > 0) { string swap = a; a = b; b = swap; }
                    segments.Add(string.Format(Invariant, "{0}|{1}|{2}|{3:0.000000}", group, a, b, deltaE));
                    double total;
                    lengths.TryGetValue(group, out total); lengths[group] = total + length;
                    extrusions.TryGetValue(group, out total); extrusions[group] = total + deltaE;

                    if (includeCoverage)
                    {
                        int samples = Math.Max(1, (int)Math.Ceiling(length / gridMm));
                        for (int sample = 0; sample <= samples; ++sample)
                        {
                            double t = sample / (double)samples;
                            int gx = (int)Math.Round((x + dx * t) / gridMm);
                            int gy = (int)Math.Round((y + dy * t) / gridMm);
                            coverage.Add(HashCell(group, gx, gy));
                        }
                    }
                }
                x = nx; y = ny; z = nz; e = ne;
            }

            var metricLines = new List<string>();
            var groups = new List<string>(lengths.Keys);
            groups.Sort(StringComparer.Ordinal);
            double totalLength = 0, totalExtrusion = 0;
            foreach (string group in groups)
            {
                metricLines.Add(string.Format(Invariant, "{0}|L={1:0.001}|E={2:0.0000}", group, lengths[group], extrusions[group]));
                totalLength += lengths[group];
                totalExtrusion += extrusions[group];
            }

            var sortedTools = new List<int>(usedTools);
            sortedTools.Sort();
            if (positiveSegments == 0) minX = minY = maxX = maxY = 0.0;
            return new GcodeGeometryResult {
                SegmentSha256 = HashSorted(segments),
                CoverageSha256 = includeCoverage ? HashSortedUlong(new List<ulong>(coverage)) : "",
                MetricsSha256 = HashLines(metricLines),
                PositiveSegments = positiveSegments,
                CoverageCells = includeCoverage ? coverage.Count : 0,
                TotalLength = totalLength,
                TotalExtrusion = totalExtrusion,
                LayerCount = Math.Max(0, layer + 1),
                ToolIds = String.Join(",", sortedTools),
                MinX = minX,
                MinY = minY,
                MaxX = maxX,
                MaxY = maxY,
                Coverage = coverage
            };
        }

        private static string GetOpcode(string command)
        {
            int end = 0;
            while (end < command.Length && !char.IsWhiteSpace(command[end])) ++end;
            return command.Substring(0, end).ToUpperInvariant();
        }

        private static bool TryGetParameter(string command, char name, out double value)
        {
            value = 0;
            int i = 0;
            while (i < command.Length)
            {
                while (i < command.Length && char.IsWhiteSpace(command[i])) ++i;
                if (i >= command.Length) break;
                int start = i++;
                while (i < command.Length && !char.IsWhiteSpace(command[i])) ++i;
                if (char.ToUpperInvariant(command[start]) != name || i <= start + 1) continue;
                if (double.TryParse(command.Substring(start + 1, i - start - 1), NumberStyles.Float, Invariant, out value)) return true;
            }
            return false;
        }

        private static void TryParseLeadingInt(string text, ref int value)
        {
            text = text.TrimStart();
            int end = 0;
            if (end < text.Length && text[end] == '-') ++end;
            while (end < text.Length && char.IsDigit(text[end])) ++end;
            int parsed;
            if (end > 0 && int.TryParse(text.Substring(0, end), NumberStyles.Integer, Invariant, out parsed)) value = parsed;
        }

        private static string HashSorted(List<string> lines)
        {
            lines.Sort(StringComparer.Ordinal);
            return HashLines(lines);
        }

        private static ulong HashCell(string group, int x, int y)
        {
            const ulong offset = 14695981039346656037UL;
            const ulong prime = 1099511628211UL;
            ulong hash = offset;
            byte[] bytes = Encoding.UTF8.GetBytes(group);
            for (int i = 0; i < bytes.Length; ++i) { hash ^= bytes[i]; hash *= prime; }
            unchecked {
                hash ^= (uint)x; hash *= prime;
                hash ^= (uint)y; hash *= prime;
            }
            return hash;
        }

        private static string HashSortedUlong(List<ulong> values)
        {
            values.Sort();
            using (SHA256 sha = SHA256.Create())
            {
                byte[] bytes = new byte[8];
                foreach (ulong value in values)
                {
                    for (int i = 0; i < 8; ++i) bytes[i] = (byte)(value >> (i * 8));
                    sha.TransformBlock(bytes, 0, bytes.Length, bytes, 0);
                }
                sha.TransformFinalBlock(new byte[0], 0, 0);
                return BitConverter.ToString(sha.Hash).Replace("-", "").ToLowerInvariant();
            }
        }

        private static string HashLines(List<string> lines)
        {
            using (SHA256 sha = SHA256.Create())
            {
                foreach (string line in lines)
                {
                    byte[] bytes = Encoding.UTF8.GetBytes(line + "\n");
                    sha.TransformBlock(bytes, 0, bytes.Length, bytes, 0);
                }
                sha.TransformFinalBlock(new byte[0], 0, 0);
                return BitConverter.ToString(sha.Hash).Replace("-", "").ToLowerInvariant();
            }
        }
    }
}
