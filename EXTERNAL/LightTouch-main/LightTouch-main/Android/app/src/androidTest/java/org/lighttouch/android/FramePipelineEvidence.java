package org.lighttouch.android;

import java.math.BigInteger;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.LinkedHashSet;
import java.util.Map;
import java.util.Set;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/** Decoder for two fixed engine-owned flat JSON snapshots, never arbitrary QMP. */
final class FramePipelineEvidence {
    static final int MAX_PROPERTY_CHARS = 4_096;
    static final BigInteger UINT64_MAX = new BigInteger("18446744073709551615");
    private static final BigInteger UINT32_MAX = new BigInteger("4294967295");
    private static final Pattern FIELD = Pattern.compile(
            "\"([A-Za-z][A-Za-z0-9]{0,63})\"[ \\t\\r\\n]*:[ \\t\\r\\n]*(-?(?:0|[1-9][0-9]*)|true|false)");

    enum Kind {
        SCAN("frame-scan-counters",
                "schema resets scanAttempts vsyncScans pollScans duplicatePollSkips forcedChecks framebufferChecks updatedRegions unchangedChecks rotatedUpdates scanoutBase glesGeneration updatedRegionsProveGuestWrites",
                "resets scanAttempts vsyncScans pollScans duplicatePollSkips forcedChecks framebufferChecks updatedRegions unchangedChecks rotatedUpdates"),
        PORTABLE("portable-frame-counters",
                "schema captureAttempts identicalSuppressed publications publishedSerial validCopyCalls noFrame metadataOnly bufferSmall pixelCopies lastCopiedSerial publishedBuffer pending atomicWithLcd",
                "captureAttempts identicalSuppressed publications validCopyCalls noFrame metadataOnly bufferSmall pixelCopies");
        final String property;
        final Set<String> fields, cumulative;
        Kind(String property, String fields, String cumulative) {
            this.property = property;
            this.fields = words(fields); this.cumulative = words(cumulative);
        }
    }
    private static Set<String> words(String text) {
        Set<String> result = new LinkedHashSet<>();
        Collections.addAll(result, text.split(" "));
        return Collections.unmodifiableSet(result);
    }
    // Keep the transport allowlist independent of Android/JSONObject so tests
    // exercise the same decisions as the actual instrumentation socket writer.
    static void validateRequest(String command, Map<String, String> arguments) {
        boolean lcdPath = arguments.get("path") != null && arguments.get("path").matches(
                "/machine/unattached/[A-Za-z0-9_.\\[\\]-]{1,64}") &&
                !arguments.get("path").endsWith("/.") && !arguments.get("path").endsWith("/..");
        if ("qmp_capabilities".equals(command) && arguments.isEmpty()) return;
        if ("qom-list".equals(command) && arguments.keySet().equals(words("path")) &&
                ("/machine/unattached".equals(arguments.get("path")) || lcdPath)) return;
        if ("qom-get".equals(command) && arguments.keySet().equals(words("path property")) && lcdPath &&
                words("frame-timeline frame-scan-counters portable-frame-counters").contains(arguments.get("property"))) return;
        throw new IllegalArgumentException("Request is outside the fixed read-only frame diagnostic allowlist");
    }
    static Set<String> missingProperties(Map<String, String> properties) {
        Set<String> missing = new LinkedHashSet<>();
        for (String property : words("frame-timeline frame-scan-counters portable-frame-counters")) {
            if (!properties.containsKey(property)) missing.add(property);
            else if (!"str".equals(properties.get(property)))
                throw new IllegalArgumentException("Wrong type for fixed frame diagnostic property: " + property);
        }
        return Collections.unmodifiableSet(missing);
    }
    static final class Snapshot {
        final Kind kind;
        final Map<String, Object> values;
        private Snapshot(Kind kind, Map<String, Object> values) {
            this.kind = kind; this.values = Collections.unmodifiableMap(new LinkedHashMap<>(values));
        }
        BigInteger number(String name) { return (BigInteger) values.get(name); }
    }
    private static int whitespace(String text, int index) {
        while (index < text.length() && " \t\r\n".indexOf(text.charAt(index)) >= 0) index++;
        return index;
    }
    static Snapshot parse(Kind kind, String text) {
        if (kind == null || text == null) throw new IllegalArgumentException("Missing frame counter kind or value");
        if (text.length() > MAX_PROPERTY_CHARS) throw new IllegalArgumentException("Oversized frame counters");
        int cursor = whitespace(text, 0);
        if (cursor >= text.length() || text.charAt(cursor++) != '{')
            throw new IllegalArgumentException("Frame counters object missing");
        Map<String, Object> values = new LinkedHashMap<>();
        for (;;) {
            cursor = whitespace(text, cursor);
            Matcher match = FIELD.matcher(text); match.region(cursor, text.length());
            if (!match.lookingAt()) throw new IllegalArgumentException("Invalid fixed frame counter field");
            String name = match.group(1), raw = match.group(2);
            if (!kind.fields.contains(name) || values.containsKey(name))
                throw new IllegalArgumentException("Unknown or duplicate frame counter field");
            Object value;
            if (name.equals("pending") || name.equals("atomicWithLcd") || name.equals("updatedRegionsProveGuestWrites")) {
                if (!raw.equals("true") && !raw.equals("false")) throw new IllegalArgumentException("Frame flag is not boolean");
                value = Boolean.valueOf(raw);
                if (!name.equals("pending") && !Boolean.FALSE.equals(value))
                    throw new IllegalArgumentException("Unsupported frame evidence claim");
            } else {
                if (raw.length() > 20 || raw.equals("true") || raw.equals("false") || raw.equals("-0"))
                    throw new IllegalArgumentException("Frame counter is not an unsigned integer");
                BigInteger number = new BigInteger(raw);
                if (name.equals("publishedBuffer")) {
                    if (number.compareTo(BigInteger.valueOf(-1)) < 0 || number.compareTo(BigInteger.valueOf(2)) > 0)
                        throw new IllegalArgumentException("Invalid published buffer index");
                } else if (number.signum() < 0 || number.compareTo(UINT64_MAX) > 0 ||
                        (name.equals("scanoutBase") && number.compareTo(UINT32_MAX) > 0) ||
                        (name.equals("schema") && !number.equals(BigInteger.ONE))) {
                    throw new IllegalArgumentException("Frame counter outside fixed schema range");
                }
                value = number;
            }
            values.put(name, value); cursor = whitespace(text, match.end());
            if (cursor >= text.length()) throw new IllegalArgumentException("Incomplete frame counters");
            char delimiter = text.charAt(cursor++);
            if (delimiter == '}') break;
            if (delimiter != ',') throw new IllegalArgumentException("Invalid frame counter delimiter");
        }
        if (whitespace(text, cursor) != text.length() || !values.keySet().equals(kind.fields))
            throw new IllegalArgumentException("Missing or trailing frame counter data");
        return new Snapshot(kind, values);
    }
    static final class Delta {
        final Map<String, BigInteger> values;
        final Map<String, String> unavailable;
        final boolean guestResetObserved, guestResetBoundaryUncertain;
        private Delta(Map<String, BigInteger> values, Map<String, String> unavailable, boolean reset, boolean resetUncertain) {
            this.values = Collections.unmodifiableMap(values);
            this.unavailable = Collections.unmodifiableMap(unavailable);
            guestResetObserved = reset; guestResetBoundaryUncertain = resetUncertain;
        }
        boolean allExact() { return unavailable.isEmpty(); }
    }
    static Delta compare(Snapshot before, Snapshot after) {
        if (before.kind != after.kind) throw new IllegalArgumentException("Frame counter kinds differ");
        boolean reset = before.kind == Kind.SCAN && !before.number("resets").equals(after.number("resets"));
        boolean resetUncertain = before.kind == Kind.SCAN &&
                (before.number("resets").equals(UINT64_MAX) || after.number("resets").equals(UINT64_MAX));
        Map<String, BigInteger> values = new LinkedHashMap<>();
        Map<String, String> unavailable = new LinkedHashMap<>();
        for (String name : before.kind.cumulative) {
            BigInteger old = before.number(name), now = after.number(name);
            String reason = null;
            if (old.equals(UINT64_MAX) || now.equals(UINT64_MAX)) reason = "counter saturated";
            else if (now.compareTo(old) < 0) reason = "counter decreased or process changed";
            else if (resetUncertain && !name.equals("resets")) reason = "guest reset counter saturated";
            else if (reset && !name.equals("resets")) reason = "guest reset observed";
            if (reason == null) values.put(name, now.subtract(old));
            else { values.put(name, null); unavailable.put(name, reason); }
        }
        return new Delta(values, unavailable, reset, resetUncertain);
    }
}
