package io.github.daristyle.freemonitor

data class DiscoveredHost(
    val address: String,
    val port: Int,
    val version: Int,
    val name: String,
    val tethering: Boolean = false,
) {
    val endpoint: String
        get() = "$address:$port"

    fun compatibility(appVersion: Int): HostCompatibility =
        when {
            version == appVersion -> HostCompatibility.COMPATIBLE
            version < appVersion -> HostCompatibility.HOST_IS_OLDER
            else -> HostCompatibility.APP_IS_OLDER
        }

    companion object {
        private const val ADDRESS_MASK = 0xFFFF_FFFFL
        private const val PORT_SHIFT = 32
        private const val PORT_MASK = 0xFFFFL
        private const val VERSION_SHIFT = 48
        private const val VERSION_MASK = 0xFFL
        private const val OCTET_MASK = 0xFF
        private val OCTET_SHIFTS = intArrayOf(24, 16, 8, 0)

        val LIST_ORDER: Comparator<DiscoveredHost> =
            compareBy(String.CASE_INSENSITIVE_ORDER, DiscoveredHost::name)
                .thenByDescending(DiscoveredHost::tethering)
                .thenBy(DiscoveredHost::endpoint)

        fun fromNative(
            info: Long,
            name: ByteArray,
            tetheringSubnets: TetheringSubnets = TetheringSubnets.NONE,
        ): DiscoveredHost {
            val address = (info and ADDRESS_MASK).toInt()
            return DiscoveredHost(
                address = ipv4Text(address),
                port = ((info ushr PORT_SHIFT) and PORT_MASK).toInt(),
                version = ((info ushr VERSION_SHIFT) and VERSION_MASK).toInt(),
                name = String(name, Charsets.UTF_8),
                tethering = tetheringSubnets.contains(address),
            )
        }

        private fun ipv4Text(address: Int): String =
            OCTET_SHIFTS.joinToString(".") { shift -> ((address ushr shift) and OCTET_MASK).toString() }
    }
}
