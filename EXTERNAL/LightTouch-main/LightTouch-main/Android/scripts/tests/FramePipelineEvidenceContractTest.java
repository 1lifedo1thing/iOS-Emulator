package org.lighttouch.android;

import java.math.BigInteger;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Map;

public final class FramePipelineEvidenceContractTest {
    private static int checks;
    private static final String SCAN = "{\"schema\":1,\"resets\":0,\"scanAttempts\":12,\"vsyncScans\":8,\"pollScans\":4,\"duplicatePollSkips\":2,\"forcedChecks\":1,\"framebufferChecks\":10,\"updatedRegions\":4,\"unchangedChecks\":6,\"rotatedUpdates\":0,\"scanoutBase\":4294967295,\"glesGeneration\":0,\"updatedRegionsProveGuestWrites\":false}";
    private static final String PORTABLE = "{\"schema\":1,\"captureAttempts\":8,\"identicalSuppressed\":5,\"publications\":3,\"publishedSerial\":3,\"validCopyCalls\":11,\"noFrame\":6,\"metadataOnly\":2,\"bufferSmall\":1,\"pixelCopies\":2,\"lastCopiedSerial\":3,\"publishedBuffer\":2,\"pending\":true,\"atomicWithLcd\":false}";
    private static void check(boolean condition, String message) {
        checks++; if (!condition) throw new AssertionError(message);
    }
    private static void rejects(FramePipelineEvidence.Kind kind, String raw) {
        boolean rejected = false;
        try { FramePipelineEvidence.parse(kind, raw); } catch (IllegalArgumentException expected) { rejected = true; }
        check(rejected, "malformed fixed counter JSON was accepted: " + (raw == null ? "null" : raw.substring(0, Math.min(raw.length(), 100))));
    }
    private static void request(boolean allowed, String command, Map<String, String> arguments) {
        boolean accepted = true;
        try { FramePipelineEvidence.validateRequest(command, arguments); }
        catch (IllegalArgumentException expected) { accepted = false; }
        check(accepted == allowed, "wrong read-only QMP allowlist decision for " + command + " " + arguments);
    }
    public static void main(String[] args) throws Exception {
        String path = "/machine/unattached/device[0]";
        request(true, "qmp_capabilities", Map.of());
        request(true, "qom-list", Map.of("path", "/machine/unattached"));
        request(true, "qom-list", Map.of("path", path));
        for (String property : new String[]{"frame-timeline", "frame-scan-counters", "portable-frame-counters"})
            request(true, "qom-get", Map.of("path", path, "property", property));
        for (String command : new String[]{"qom-set", "stop", "cont", "quit", "screendump", "human-monitor-command", "query-status"})
            request(false, command, Map.of("path", path, "property", "frame-scan-counters"));
        request(false, "qmp_capabilities", Map.of("path", path));
        request(false, "qom-list", Map.of("path", path, "property", "frame-timeline"));
        request(false, "qom-get", Map.of("path", path));
        request(false, "qom-get", Map.of("path", path, "property", "frame-timeline", "value", "x"));
        request(false, "qom-get", Map.of("path", path, "property", "other"));
        for (String bad : new String[]{"/machine", "/machine/unattached", "/machine/unattached/.", "/machine/unattached/..",
                "/machine/unattached/a/b", "/machine/unattached/a\n", "/machine/unattached/" + "x".repeat(65)})
            request(false, "qom-get", Map.of("path", bad, "property", "frame-timeline"));
        check(FramePipelineEvidence.missingProperties(Map.of()).size() == 3, "old engine was not explicitly unsupported");
        check(FramePipelineEvidence.missingProperties(Map.of("frame-timeline", "str")).size() == 2, "timeline-only engine passed counter admission");
        check(FramePipelineEvidence.missingProperties(Map.of("frame-timeline", "str", "frame-scan-counters", "str", "portable-frame-counters", "str")).isEmpty(), "fixed engine properties not admitted");
        boolean wrongTypeRejected = false;
        try { FramePipelineEvidence.missingProperties(Map.of("frame-timeline", "uint64")); }
        catch (IllegalArgumentException expected) { wrongTypeRejected = true; }
        check(wrongTypeRejected, "wrong QOM getter type admitted");
        rejects(FramePipelineEvidence.Kind.SCAN, null);
        var scan = FramePipelineEvidence.parse(FramePipelineEvidence.Kind.SCAN, SCAN);
        var portable = FramePipelineEvidence.parse(FramePipelineEvidence.Kind.PORTABLE, PORTABLE);
        check(scan.number("scanoutBase").equals(new BigInteger("4294967295")), "uint32 address was signed or truncated");
        check(Boolean.TRUE.equals(portable.values.get("pending")), "actual pending flag disappeared");
        var same = FramePipelineEvidence.compare(scan, scan);
        check(same.allExact() && same.values.get("scanAttempts").signum() == 0, "unchanged snapshots were not measured zero");
        var advanced = FramePipelineEvidence.parse(FramePipelineEvidence.Kind.SCAN, SCAN.replace("\"scanAttempts\":12", "\"scanAttempts\":15"));
        check(FramePipelineEvidence.compare(scan, advanced).values.get("scanAttempts").equals(BigInteger.valueOf(3)), "exact delta failed");
        var decreased = FramePipelineEvidence.compare(advanced, scan);
        check(!decreased.allExact() && decreased.values.get("scanAttempts") == null && decreased.unavailable.containsKey("scanAttempts"), "counter regression became zero progress");
        var reset = FramePipelineEvidence.parse(FramePipelineEvidence.Kind.SCAN, SCAN.replace("\"resets\":0", "\"resets\":1"));
        var acrossReset = FramePipelineEvidence.compare(scan, reset);
        check(acrossReset.guestResetObserved && !acrossReset.allExact() && acrossReset.values.get("scanAttempts") == null, "guest reset retained a usable scan delta");
        check(acrossReset.values.get("resets").equals(BigInteger.ONE), "reset count itself was lost");
        var saturatedReset = FramePipelineEvidence.parse(FramePipelineEvidence.Kind.SCAN,
                SCAN.replace("\"resets\":0", "\"resets\":18446744073709551615"));
        var uncertainReset = FramePipelineEvidence.compare(saturatedReset, saturatedReset);
        check(!uncertainReset.guestResetObserved && uncertainReset.guestResetBoundaryUncertain &&
                uncertainReset.values.get("scanAttempts") == null && !uncertainReset.allExact(),
                "saturated reset counter allowed an exact interval or invented a reset");
        var high = FramePipelineEvidence.parse(FramePipelineEvidence.Kind.PORTABLE,
                PORTABLE.replace("\"pixelCopies\":2", "\"pixelCopies\":18446744073709551614"));
        check(high.number("pixelCopies").equals(FramePipelineEvidence.UINT64_MAX.subtract(BigInteger.ONE)), "uint64 value was rounded through double/signed long");
        var saturated = FramePipelineEvidence.parse(FramePipelineEvidence.Kind.PORTABLE,
                PORTABLE.replace("\"pixelCopies\":2", "\"pixelCopies\":18446744073709551615"));
        var bound = FramePipelineEvidence.compare(high, saturated);
        check(bound.values.get("pixelCopies") == null && !bound.allExact(), "saturation was presented as an exact increment");
        check(FramePipelineEvidence.compare(high, high).allExact(), "non-saturated uint64 values were rejected");
        check(!FramePipelineEvidence.compare(saturated, saturated).allExact(), "unchanged saturation became measured zero");
        check(FramePipelineEvidence.parse(FramePipelineEvidence.Kind.PORTABLE, PORTABLE.replace("\"publishedBuffer\":2", "\"publishedBuffer\":-1")).number("publishedBuffer").intValue() == -1, "unpublished buffer sentinel rejected");
        check(FramePipelineEvidence.parse(FramePipelineEvidence.Kind.SCAN, "\n " + SCAN + "\t\r\n").values.equals(scan.values), "legal JSON whitespace rejected");
        boolean immutable = false;
        try { scan.values.put("scanAttempts", BigInteger.ZERO); } catch (UnsupportedOperationException expected) { immutable = true; }
        check(immutable, "parsed evidence can be mutated");
        for (String raw : new String[]{"", "{}", "[]", SCAN.substring(0, SCAN.length()-1), SCAN + "{}",
                SCAN.replace("\"schema\":1", "\"schema\":2"), SCAN.replace("\"resets\":0,", ""),
                SCAN.replace("\"resets\":0", "\"unknown\":0"), SCAN.replace("\"resets\":0", "\"resets\":0,\"resets\":0"),
                SCAN.replace("\"resets\":0", "\"resets\":00"), SCAN.replace("\"resets\":0", "\"resets\":-0"),
                SCAN.replace("\"resets\":0", "\"resets\":-1"), SCAN.replace("\"resets\":0", "\"resets\":+1"),
                SCAN.replace("\"resets\":0", "\"resets\":1.0"), SCAN.replace("\"resets\":0", "\"resets\":1e2"),
                SCAN.replace("\"resets\":0", "\"resets\":\"1\""), SCAN.replace("\"resets\":0", "\"resets\":true"),
                SCAN.replace("\"resets\":0", "\"resets\":{}"), SCAN.replace("\"resets\":0", "\"resets\":null"),
                SCAN.replace("\"resets\":0", "\"resets\":18446744073709551616"),
                SCAN.replace("\"scanoutBase\":4294967295", "\"scanoutBase\":4294967296"),
                SCAN.replace("\"updatedRegionsProveGuestWrites\":false", "\"updatedRegionsProveGuestWrites\":true"),
                SCAN.replace("\"resets\"", "\"re\\u0073ets\""), SCAN.replace('}', ','),
                "x".repeat(FramePipelineEvidence.MAX_PROPERTY_CHARS+1)}) rejects(FramePipelineEvidence.Kind.SCAN, raw);
        for (String raw : new String[]{PORTABLE.replace("\"publishedBuffer\":2", "\"publishedBuffer\":3"),
                PORTABLE.replace("\"publishedBuffer\":2", "\"publishedBuffer\":-2"),
                PORTABLE.replace("\"pending\":true", "\"pending\":1"),
                PORTABLE.replace("\"atomicWithLcd\":false", "\"atomicWithLcd\":true"), SCAN})
            rejects(FramePipelineEvidence.Kind.PORTABLE, raw);
        if (args.length != 0) {
            check(args.length == 2, "expected two actual production getter fixture paths");
            var actualScan = FramePipelineEvidence.parse(FramePipelineEvidence.Kind.SCAN, Files.readString(Path.of(args[0])));
            var actualPortable = FramePipelineEvidence.parse(FramePipelineEvidence.Kind.PORTABLE, Files.readString(Path.of(args[1])));
            check(actualScan.number("scanAttempts").equals(BigInteger.valueOf(7)), "production scan snapshot disagrees with exercised paths");
            check(actualScan.number("rotatedUpdates").equals(BigInteger.ONE), "production rotated path missing");
            check(actualPortable.number("publications").equals(BigInteger.valueOf(2)), "production publication count missing");
            check(actualPortable.number("validCopyCalls").equals(BigInteger.valueOf(5)), "production copy classification missing");
        }
        System.out.println("FramePipelineEvidence contract PASS: " + checks + " assertions");
    }
}
