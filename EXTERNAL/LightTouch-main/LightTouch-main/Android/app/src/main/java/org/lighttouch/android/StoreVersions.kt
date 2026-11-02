package org.lighttouch.android

/** Version numbers as the Legacy Store and apps write them: up to three numbers, the first not zero. No Android in it. */
object StoreVersions {
    fun parse(value: String?): List<Int>? {
        if (value == null || !Regex("[0-9]+(\\.[0-9]+){0,2}").matches(value)) return null
        val parts = value.split('.').map { it.toIntOrNull() ?: return null }
        return if (parts[0] == 0) null else parts + List(3 - parts.size) { 0 }
    }

    fun newer(a: List<Int>, b: List<Int>) = a.zip(b).firstOrNull { it.first != it.second }?.let { it.first > it.second } ?: false

    /** The highest of the version strings that read as versions, as the catalog wrote it (a note after a space is ignored for comparing). */
    fun newest(versions: List<String>): String? =
        versions.mapNotNull { text -> parse(text.substringBefore(' '))?.let { it to text } }
            .reduceOrNull { best, next -> if (newer(next.first, best.first)) next else best }?.second

    /** [newest] when it is newer than the [installed] version, else null: a version that doesn't read as one never claims an update. */
    fun updateTo(installed: String?, newest: String?): String? {
        val have = parse(installed?.trim()) ?: return null
        val want = parse(newest?.substringBefore(' ')) ?: return null
        return if (newer(want, have)) newest else null
    }
}
