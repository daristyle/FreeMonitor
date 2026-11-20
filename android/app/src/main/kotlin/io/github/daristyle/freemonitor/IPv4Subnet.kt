package io.github.daristyle.freemonitor

data class IPv4Subnet(
    val address: Int,
    val prefixLength: Int,
) {
    private val mask: Int
        get() = if (prefixLength <= 0) 0 else ALL_BITS shl (ADDRESS_BITS - prefixLength.coerceAtMost(ADDRESS_BITS))

    fun contains(other: Int): Boolean = (address and mask) == (other and mask)

    private companion object {
        const val ALL_BITS = -1
        const val ADDRESS_BITS = 32
    }
}
