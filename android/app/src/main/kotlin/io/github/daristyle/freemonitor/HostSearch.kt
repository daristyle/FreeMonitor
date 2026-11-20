package io.github.daristyle.freemonitor

object HostSearch {
    const val DISCOVERY_PORT = 50501
    const val DISCOVERY_TIME_MS = 1000
    const val MAX_HOSTS = 64
    private const val LIMITED_BROADCAST_ADDRESS = 0xFFFF_FFFF.toInt()

    fun search(tetheringSubnets: TetheringSubnets): List<DiscoveredHost> =
        find(
            intArrayOf(LIMITED_BROADCAST_ADDRESS) + NativeBridge.broadcastAddresses(),
            DISCOVERY_PORT,
            DISCOVERY_TIME_MS,
            tetheringSubnets,
        )

    fun find(
        addresses: IntArray,
        port: Int,
        durationMs: Int,
        tetheringSubnets: TetheringSubnets = TetheringSubnets.NONE,
    ): List<DiscoveredHost> {
        val hostInfo = LongArray(MAX_HOSTS)
        val names = NativeBridge.searchHosts(addresses, port, durationMs, hostInfo)
        return names.indices
            .map { DiscoveredHost.fromNative(hostInfo[it], names[it], tetheringSubnets) }
            .sortedWith(DiscoveredHost.LIST_ORDER)
    }
}
